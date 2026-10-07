/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavformat/pyrowave_common.h"

typedef struct PyroWaveEncodeContext
{
    AVClass *class;
    pyrowave_device device;
    pyrowave_encoder encoder;
    int64_t target_payload_size;
    const struct pyrowave_format_mapping *format;
    int doublebuffer;
    int flushing;
    int64_t frame_count;
    int64_t pending_pts;
} PyroWaveEncodeContext;

static void set_extradata(AVCodecContext *avctx, const struct pyrowave_format_mapping *format)
{
    struct PWV1Header *header = av_mallocz(sizeof(*header));
    avctx->extradata = (void *)header;
    avctx->extradata_size = sizeof(*header);

    // Frame rate is ignored (0). Container decides.
    header->magic = MKTAG('P', 'W', 'V', '1');
    header->reference_bit_depth = format->depth;
    header->header_version = 1;
    header->pyro.chroma_resolution = format->subsampled ? CHROMA_RESOLUTION_420 : CHROMA_RESOLUTION_444;
    header->pyro.width_minus_1 = avctx->width - 1;
    header->pyro.height_minus_1 = avctx->height - 1;

    header->pyro.color_primaries = avctx->color_primaries == AVCOL_PRI_BT2020 ?
        COLOR_PRIMARIES_BT2020 : COLOR_PRIMARIES_SRGB;
    header->pyro.transfer_function = avctx->color_trc == AVCOL_TRC_SMPTE2084 ?
        TRANSFER_FUNCTION_PQ : TRANSFER_FUNCTION_SRGB;
    header->pyro.ycbcr_range = avctx->color_range == AVCOL_RANGE_JPEG ?
        YCBCR_RANGE_FULL : YCBCR_RANGE_LIMITED;
    header->pyro.ycbcr_transform = avctx->colorspace == AVCOL_SPC_BT2020_NCL ?
        YCBCR_TRANSFORM_BT2020 : YCBCR_TRANSFORM_BT709;

    // Make some default assumptions if chroma siting is not specified.
    if (avctx->color_range == AVCOL_RANGE_JPEG && avctx->chroma_sample_location == AVCHROMA_LOC_UNSPECIFIED) {
        header->pyro.chroma_siting = CHROMA_SITING_CENTER;
        av_log(avctx, AV_LOG_WARNING, "Chroma siting is not set, assuming CENTER due to JPEG range.\n");
    } else if (avctx->color_range == AVCOL_RANGE_MPEG && avctx->chroma_sample_location == AVCHROMA_LOC_UNSPECIFIED) {
        header->pyro.chroma_siting = CHROMA_SITING_LEFT;
        av_log(avctx, AV_LOG_WARNING, "Chroma siting is not set, guessing LEFT due to MPEG range.\n");
    } else if (avctx->chroma_sample_location == AVCHROMA_LOC_CENTER) {
        header->pyro.chroma_siting = CHROMA_SITING_CENTER;
    } else if (avctx->chroma_sample_location == AVCHROMA_LOC_LEFT) {
        header->pyro.chroma_siting = CHROMA_SITING_LEFT;
    } else if (avctx->color_range == AVCOL_RANGE_JPEG) {
        av_log(avctx, AV_LOG_WARNING, "Unsupported chroma siting, assuming CENTER.\n");
        header->pyro.chroma_siting = CHROMA_SITING_CENTER;
    } else {
        av_log(avctx, AV_LOG_WARNING, "Unsupported chroma siting, assuming LEFT.\n");
        header->pyro.chroma_siting = CHROMA_SITING_LEFT;
    }
}

static av_cold int pwv_encode_init(AVCodecContext *avctx)
{
    PyroWaveEncodeContext *context = avctx->priv_data;
    pyrowave_encoder_create_info create_info = {};
    pyrowave_result result;

    result = pyrowave_create_default_device(&context->device);

    if (result == PYROWAVE_ERROR_NO_VULKAN) {
        av_log(avctx, AV_LOG_ERROR, "No available Vulkan implementation for PyroWave.\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    if (result != PYROWAVE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create Vulkan device.\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    context->format = NULL;
    for (int i = 0; i < FF_ARRAY_ELEMS(pwv1_format_mapping); i++) {
        if (pwv1_format_mapping[i].pix_fmt == avctx->pix_fmt) {
            context->format = &pwv1_format_mapping[i];
            break;
        }
    }

    if (!context->format) {
        // Shouldn't really happen, but be safe?
        av_log(avctx, AV_LOG_ERROR, "Unexpected format %d\n", avctx->pix_fmt);
        return AVERROR(EINVAL);
    }

    // For e.g. MKV where we want to store the PWV1Header in global data.
    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
        set_extradata(avctx, context->format);

    create_info.width = avctx->width;
    create_info.height = avctx->height;
    create_info.chroma = context->format->subsampled ? PYROWAVE_CHROMA_SUBSAMPLING_420 : PYROWAVE_CHROMA_SUBSAMPLING_444;
    create_info.device = context->device;

    result = pyrowave_encoder_create(&create_info, &context->encoder);
    if (result != PYROWAVE_SUCCESS) {
        pyrowave_device_destroy(context->device);
        context->device = NULL;
        return AVERROR_ENCODER_NOT_FOUND;
    }

    context->target_payload_size = av_rescale_rnd(avctx->bit_rate,
        avctx->framerate.den, avctx->framerate.num * 8, AV_ROUND_UP);
    context->target_payload_size -= context->target_payload_size & 3;

    return 0;
}

static int pwv_encode_close(AVCodecContext *avctx)
{
    PyroWaveEncodeContext *context = avctx->priv_data;

    if (context->encoder) {
        pyrowave_encoder_destroy(context->encoder);
        context->encoder = NULL;
    }

    if (context->device) {
        pyrowave_device_destroy(context->device);
        context->device = NULL;
    }

    return 0;
}

static int pwv_encode(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame, int *got_packet)
{
    PyroWaveEncodeContext *context = avctx->priv_data;
    pyrowave_result result = PYROWAVE_SUCCESS;
    pyrowave_rate_control rate_control = {};
    pyrowave_cpu_buffer cpu_buffers = {};
    pyrowave_packet packet;
    size_t num_packets;
    int ret;

    *got_packet = 0;

    if (!frame && !context->doublebuffer)
        return AVERROR(EINVAL);

    if (context->flushing == 2) {
        // Only one frame of buffering.
        return 0;
    }

    if (!frame && context->doublebuffer && context->flushing == 0)
        context->flushing = 1;

    result = PYROWAVE_SUCCESS;

    if (!context->flushing) {
        cpu_buffers.format = context->format->pyro_cpu_format;
        cpu_buffers.width = avctx->width;
        cpu_buffers.height = avctx->height;

        for (int i = 0; i < 3; i++) {
            if (frame->linesize[i] < 0) {
                av_log(avctx, AV_LOG_ERROR, "Negative linesize not supported.\n");
                return AVERROR(EINVAL);
            }
            cpu_buffers.data[i] = frame->data[i];
            cpu_buffers.row_stride_in_bytes[i] = frame->linesize[i];

            cpu_buffers.plane_size_in_bytes[i] = frame->linesize[i] * avctx->height;
            if (i && context->format->subsampled)
                cpu_buffers.plane_size_in_bytes[i] /= 2;
        }

        rate_control.maximum_bitstream_size = context->target_payload_size;

        if (context->doublebuffer)
            pyrowave_encoder_set_frame_context(context->encoder, context->frame_count % 2);
        result = pyrowave_encoder_encode_cpu(context->encoder, &cpu_buffers, &rate_control);
    }

    if (result != PYROWAVE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to encode frame.\n");
        return AVERROR(EINVAL);
    }

    if (!context->doublebuffer || context->frame_count > 0) {
        if ((ret = ff_alloc_packet(avctx, pkt, context->target_payload_size)) < 0)
            return ret;

        if (context->doublebuffer)
            pyrowave_encoder_set_frame_context(context->encoder, (context->frame_count - 1) % 2);

        num_packets = 1;
        result = pyrowave_encoder_packetize(context->encoder, &packet, context->target_payload_size,
                &num_packets, pkt->data, context->target_payload_size);

        if (result != PYROWAVE_SUCCESS ||
            num_packets != 1 || packet.offset != 0 ||
            packet.size > context->target_payload_size) {
            av_log(avctx, AV_LOG_ERROR, "Something unexpected happened with packetization.\n");
            return AVERROR_UNKNOWN;
        }

        pkt->size = packet.size;
        pkt->flags = AV_PKT_FLAG_KEY;
        pkt->pts = context->doublebuffer ? context->pending_pts : frame->pts;
        *got_packet = 1;
    }

    if (frame)
        context->pending_pts = frame->pts;

    if (context->flushing)
        context->flushing++;

    context->frame_count++;
    return 0;
}

#define OFFSET(x) offsetof(PyroWaveEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "doublebuffer", "Double buffer upload with encode", OFFSET(doublebuffer), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VE },
    { NULL },
};

static const AVClass libpyrowave_class = {
    .class_name = "libpyrowave encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_libpyrowave_encoder = {
    .p.name         = "libpyrowave",
    CODEC_LONG_NAME("raw video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PYROWAVE,
    .p.capabilities = AV_CODEC_CAP_DELAY, // Only applies to doublebuffer mode.
    .priv_data_size = sizeof(PyroWaveEncodeContext),
    .init           = pwv_encode_init,
    .close          = pwv_encode_close,
    FF_CODEC_ENCODE_CB(pwv_encode),
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV420P16,
        AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV444P16),
    .color_ranges = AVCOL_RANGE_JPEG | AVCOL_RANGE_MPEG,
    .caps_internal = FF_CODEC_CAP_INIT_CLEANUP,
    .p.priv_class   = &libpyrowave_class,
};
