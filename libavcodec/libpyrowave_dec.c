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
#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include "libavutil/pixdesc.h"
#include "libavformat/pyrowave_common.h"
#include "libavutil/opt.h"

typedef struct PyroWaveDecodeContext
{
    AVClass *class;
    pyrowave_device device;
    pyrowave_decoder decoder;
    const struct pyrowave_format_mapping *format;
    int doublebuffer;
    int flushing;
    int64_t frame_count;
    int64_t pending_pts;
} PyroWaveDecodeContext;

static void parse_extradata(AVCodecContext *avctx)
{
    const struct PWV1Header *header = (const void *)avctx->extradata;
    if (avctx->extradata_size < sizeof(struct PWV1Header))
        return;

    if (header->magic != MKTAG('P', 'W', 'V', '1'))
        return;

    for (int i = 0; i < FF_ARRAY_ELEMS(pwv1_format_mapping); i++) {
        if (pwv1_format_mapping[i].chroma_resolution == header->pyro.chroma_resolution &&
            pwv1_format_mapping[i].depth == header->reference_bit_depth) {
            avctx->pix_fmt = pwv1_format_mapping[i].pix_fmt;
            break;
        }
    }

    avctx->chroma_sample_location =
        header->pyro.chroma_siting == CHROMA_SITING_CENTER ? AVCHROMA_LOC_CENTER : AVCHROMA_LOC_LEFT;
    avctx->color_range =
        header->pyro.ycbcr_range == YCBCR_RANGE_FULL ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    // Leave sRGB vague. Common game captures include both gamma 2.2 or sRGB piecewise, and don't
    // want to give too much confidence to a decoder which exact curve is assumed.
    avctx->color_trc =
        header->pyro.transfer_function == TRANSFER_FUNCTION_PQ ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_UNSPECIFIED;
    avctx->color_primaries =
        header->pyro.color_primaries == COLOR_PRIMARIES_BT2020 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    avctx->colorspace =
        header->pyro.ycbcr_transform == YCBCR_TRANSFORM_BT2020 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
}

static void pwv_flush_decoder(AVCodecContext *avctx)
{
    PyroWaveDecodeContext *context = avctx->priv_data;
    // Resets the decoder to initial state. For seeking this is important since the bitstream seq counter
    // may jump unexpectedly.
    pyrowave_decoder_clear(context->decoder);
}

static av_cold int pwv_init_decoder(AVCodecContext *avctx)
{
    PyroWaveDecodeContext *context = avctx->priv_data;
    pyrowave_decoder_create_info create_info = {};
    const AVPixFmtDescriptor *pix_desc;
    pyrowave_result result;

    parse_extradata(avctx);

    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        // TODO: We can defer to decoder time and pick yuv420p or yuv444p based on bitstream chroma, but we won't
        // know if we should actually be doing 10 or 16-bit decode, or which video usability configuration to use,
        // so it's kinda useless on its own.
        av_log(avctx, AV_LOG_ERROR, "Cannot determine correct CPU decode format. Missing the global PWV1Header. "
               "Make sure to request global headers (e.g. MKV) or use the raw muxer.\n");
        return AVERROR(EINVAL);
    }

    pix_desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    if (!pix_desc || pix_desc->nb_components != 3) {
        av_log(avctx, AV_LOG_ERROR, "Only three component formats are supported for decode.\n");
        return AVERROR(EINVAL);
    }

    if (pix_desc->log2_chroma_w != pix_desc->log2_chroma_h) {
        av_log(avctx, AV_LOG_ERROR, "4:2:2 is not supported.\n");
        return AVERROR(EINVAL);
    }

    if (pix_desc->log2_chroma_w > 1) {
        av_log(avctx, AV_LOG_ERROR, "Only 4:4:4 and 4:2:0 is supported.\n");
        return AVERROR(EINVAL);
    }

    context->format = NULL;

    while (context->format == NULL) {
        for (int i = 0; i < FF_ARRAY_ELEMS(pwv1_format_mapping); i++) {
            if (pwv1_format_mapping[i].pix_fmt == avctx->pix_fmt) {
                context->format = &pwv1_format_mapping[i];
                break;
            }
        }

        if (!context->format) {
            av_log(avctx, AV_LOG_WARNING, "Unrecognized format %d for decode, falling back.\n", avctx->pix_fmt);
            avctx->pix_fmt = pix_desc->log2_chroma_w == 1 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV444P;
        }
    }

    result = pyrowave_create_default_device(&context->device);
    if (result == PYROWAVE_ERROR_NO_VULKAN) {
        av_log(avctx, AV_LOG_ERROR, "No available Vulkan implementation for PyroWave.\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    if (result != PYROWAVE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create Vulkan device.\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    create_info.device = context->device;
    create_info.width = avctx->width;
    create_info.height = avctx->height;
    create_info.chroma = context->format->subsampled ? PYROWAVE_CHROMA_SUBSAMPLING_420 : PYROWAVE_CHROMA_SUBSAMPLING_444;

    result = pyrowave_decoder_create(&create_info, &context->decoder);
    if (result != PYROWAVE_SUCCESS) {
        pyrowave_device_destroy(context->device);
        context->device = NULL;
        return AVERROR_DECODER_NOT_FOUND;
    }

    return 0;
}

static av_cold int pwv_close_decoder(AVCodecContext *avctx)
{
    PyroWaveDecodeContext *context = avctx->priv_data;

    if (context->decoder) {
        pyrowave_decoder_destroy(context->decoder);
        context->decoder = NULL;
    }

    if (context->device) {
        pyrowave_device_destroy(context->device);
        context->device = NULL;
    }

    return 0;
}

static int pwv_decode(AVCodecContext *avctx, AVFrame *frame,
                      int *got_frame, AVPacket *avpkt)
{
    PyroWaveDecodeContext *context = avctx->priv_data;
    pyrowave_result result = PYROWAVE_SUCCESS;
    pyrowave_cpu_buffer cpu_buffers = {};

    *got_frame = 0;

    if (!context->doublebuffer && !avpkt)
        return AVERROR(EINVAL);

    if (context->flushing == 2)
        return 0;

    if (avpkt && avpkt->size) {
        result = pyrowave_decoder_push_packet(context->decoder, avpkt->data, avpkt->size);
        if (result != PYROWAVE_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to push packet. Corrupt packet?\n");
            return AVERROR(EINVAL);
        }

        // Partial packets.
        if (!pyrowave_decoder_decode_is_ready(context->decoder, false))
            return 0;
    } else if (!context->flushing) {
        context->flushing = 1;
    }

    // TODO: Is this really the correct way to do this?
    av_frame_unref(frame);
    frame->width = avctx->width;
    frame->height = avctx->height;
    frame->format = avctx->pix_fmt;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate frame buffer.\n");
        return AVERROR(ENOMEM);
    }

    cpu_buffers.width = avctx->width;
    cpu_buffers.height = avctx->height;
    cpu_buffers.format = context->format->pyro_cpu_format;

    for (int i = 0; i < 3; i++) {
        if (frame->linesize[i] < 0) {
            av_log(avctx, AV_LOG_ERROR, "Negative linesize not currently supported.\n");
            return AVERROR(EINVAL);
        }
        cpu_buffers.data[i] = frame->data[i];
        cpu_buffers.row_stride_in_bytes[i] = frame->linesize[i];

        // Assume it's safe to write in the padding area.
        cpu_buffers.plane_size_in_bytes[i] = frame->linesize[i] * avctx->height;
        if (i && context->format->subsampled)
            cpu_buffers.plane_size_in_bytes[i] /= 2;
    }

    if (!context->doublebuffer) {
        result = pyrowave_decoder_decode_cpu_buffer_synchronous(context->decoder, &cpu_buffers);
        *got_frame = result == PYROWAVE_SUCCESS;
        frame->pts = avpkt->pts;
    } else {
        if (avpkt && avpkt->size) {
            result = pyrowave_decoder_decode_cpu_buffer_async(context->decoder, &cpu_buffers,
                                                              context->frame_count % 2);
        }

        if (result == PYROWAVE_SUCCESS && context->frame_count) {
            // A more extreme API would import host pointers from AVFrames, but that's getting rather silly.
            // Probably better to leave to a Vulkan HW device integration path.
            result = pyrowave_decoder_decode_cpu_buffer_complete(context->decoder, &cpu_buffers,
                                                                 (context->frame_count - 1) % 2);
            *got_frame = result == PYROWAVE_SUCCESS;
            frame->pts = context->pending_pts;
        } else {
            // There's probably something smarter we can do here on first frame.
            // We just need the strides and plane sizes to match to kick off the async decode, but that's it.
            av_frame_unref(frame);
        }

        if (avpkt)
            context->pending_pts = avpkt->pts;
    }

    if (result != PYROWAVE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to decode frame.\n");
        return AVERROR(EINVAL);
    }

    if (context->flushing)
        context->flushing++;

    context->frame_count++;
    return 0;
}

#define OFFSET(x) offsetof(PyroWaveDecodeContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "doublebuffer", "Double buffer download with decode", OFFSET(doublebuffer), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VD },
    { NULL },
};

static const AVClass libpyrowave_class = {
    .class_name = "libpyrowave decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_libpyrowave_decoder = {
    .p.name                = "libpyrowave",
    CODEC_LONG_NAME("PyroWave"),
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_PYROWAVE,
    .p.capabilities        = AV_CODEC_CAP_DELAY,
    .priv_data_size        = sizeof(PyroWaveDecodeContext),
    .init                  = pwv_init_decoder,
    .close                 = pwv_close_decoder,
    .flush                 = pwv_flush_decoder,
    .caps_internal         = FF_CODEC_CAP_INIT_CLEANUP,
    FF_CODEC_DECODE_CB(pwv_decode),
    .p.priv_class   = &libpyrowave_class,
};
