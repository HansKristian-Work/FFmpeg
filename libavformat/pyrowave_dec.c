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

#include "libavutil/opt.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "pyrowave_common.h"

typedef struct PyroWaveDemuxerContext
{
    int64_t pts;
} PyroWaveDemuxerContext;

static int sanitze_pwv1_header(const struct PWV1Header *p)
{
    // Only expect to run correctly on little-endian systems.
    // Vulkan endianness needs to match host, and libpyrowave
    // has not been implemented with big-endian in mind at this time.
    return p->magic == MKTAG('P', 'W', 'V', '1') &&
        p->pyro.sequence == 0 && p->header_version == 1 &&
        p->pyro.code == 0 && p->pyro.extended == 0 &&
        p->pyro.total_blocks == 0 && p->padding[0] == 0 && p->padding[1] == 0;
}

static int pyrowave_probe(const AVProbeData *p)
{
    struct PWV1Header pwv1;

    if (p->buf_size < 4 + sizeof(struct PWV1Header)) {
        // Too small to fit magic and header.
        return 0;
    }

    if (memcmp(p->buf, "PWV1", 4) != 0) {
        return 0;
    }

    memcpy(&pwv1, p->buf + 4, sizeof(struct PWV1Header));

    if (!sanitze_pwv1_header(&pwv1))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int pyrowave_read_header(AVFormatContext *s)
{
    PyroWaveDemuxerContext *ctx = s->priv_data;
    enum AVPixelFormat pix_fmt;
    AVIOContext *pb = s->pb;
    struct PWV1Header pwv1;
    AVStream *st;

    if (avio_read(pb, (void *)&pwv1, sizeof(pwv1)) != sizeof(pwv1)) {
        av_log(s, AV_LOG_ERROR, "Failed to read PWV1 header.\n");
        return AVERROR(EINVAL);
    }

    if (!sanitze_pwv1_header(&pwv1)) {
        av_log(s, AV_LOG_ERROR, "PWV1 header contains invalid values.\n");
        return AVERROR(EINVAL);
    }

    pix_fmt = AV_PIX_FMT_NONE;
    for (int i = 0; i < FF_ARRAY_ELEMS(pwv1_format_mapping); i++) {
        if (pwv1_format_mapping[i].chroma_resolution == pwv1.pyro.chroma_resolution &&
            pwv1_format_mapping[i].depth == pwv1.reference_bit_depth) {
            pix_fmt = pwv1_format_mapping[i].pix_fmt;
            break;
        }
    }

    if (pix_fmt == AV_PIX_FMT_NONE) {
        av_log(s, AV_LOG_ERROR, "PWV1 header contains unexpected reference bit depth of %d.\n",
               pwv1.reference_bit_depth);
        return AVERROR(EINVAL);
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->width  = pwv1.pyro.width_minus_1 + 1;
    st->codecpar->height = pwv1.pyro.height_minus_1 + 1;

    int raten, rated;
    av_reduce(&raten, &rated, pwv1.frame_rate_num, pwv1.frame_rate_den, (1UL << 31) - 1);
    avpriv_set_pts_info(st, 64, rated, raten);
    st->avg_frame_rate = av_inv_q(st->time_base);
    st->codecpar->format = pix_fmt;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_PYROWAVE;
    st->sample_aspect_ratio = (AVRational){1, 1};
    st->codecpar->chroma_location =
        pwv1.pyro.chroma_siting == CHROMA_SITING_CENTER ? AVCHROMA_LOC_CENTER : AVCHROMA_LOC_LEFT;
    st->codecpar->color_range =
        pwv1.pyro.ycbcr_range == YCBCR_RANGE_FULL ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    // Leave sRGB vague. Common game captures include both gamma 2.2 or sRGB piecewise, and don't
    // want to give too much confidence to a decoder which exact curve is assumed.
    st->codecpar->color_trc =
        pwv1.pyro.transfer_function == TRANSFER_FUNCTION_PQ ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_UNSPECIFIED;
    st->codecpar->color_primaries =
        pwv1.pyro.color_primaries == COLOR_PRIMARIES_BT2020 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    st->codecpar->color_space =
        pwv1.pyro.ycbcr_transform == YCBCR_TRANSFORM_BT2020 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;

    ctx->pts = 0;

    return 0;
}

static int pyrowave_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    PyroWaveDemuxerContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t packet_size;
    int ret;

    packet_size = avio_rl32(pb);

    ret = av_get_packet(s->pb, pkt, packet_size);
    if (ret < 0)
        return ret;
    else if (ret != packet_size)
        return pb->eof_reached ? AVERROR_EOF : AVERROR_INVALIDDATA;

    pkt->stream_index = 0;
    pkt->pts = ctx->pts++;
    pkt->dts = pkt->pts;
    pkt->duration = 1;
    return 0;
}

const FFInputFormat ff_pyrowave_demuxer = {
    .p.name         = "pyrowave",
    .p.long_name    = NULL_IF_CONFIG_SMALL("pyrowave raw bitstream"),
    .p.extensions   = "pwv1",
    .priv_data_size = sizeof(PyroWaveDemuxerContext),
    .read_probe     = pyrowave_probe,
    .read_header    = pyrowave_read_header,
    .read_packet    = pyrowave_read_packet,
};
