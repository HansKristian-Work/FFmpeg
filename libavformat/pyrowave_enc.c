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

#include "avformat.h"
#include "mux.h"
#include "pyrowave_common.h"
#include "libavutil/pixdesc.h"

static int pyrowave_init(AVFormatContext *s)
{
    if (s->streams[0]->codecpar->codec_id != AV_CODEC_ID_PYROWAVE) {
        av_log(s, AV_LOG_ERROR, "ERROR: Codec not supported.\n");
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int pyrowave_write_header(AVFormatContext *s)
{
    const AVPixFmtDescriptor *pix_desc;
    struct PWV1Header header = {};
    AVIOContext *pb = s->pb;
    AVStream *st;

    st = s->streams[0];

    pix_desc = av_pix_fmt_desc_get(st->codecpar->format);
    if (pix_desc->nb_components != 3) {
        av_log(s, AV_LOG_ERROR, "Only three component formats are supported for encode.\n");
        return AVERROR(EINVAL);
    }

    if (pix_desc->log2_chroma_w != pix_desc->log2_chroma_h) {
        av_log(s, AV_LOG_ERROR, "4:2:2 is not supported.\n");
        return AVERROR(EINVAL);
    }

    if (pix_desc->log2_chroma_w > 1) {
        av_log(s, AV_LOG_ERROR, "Only 4:4:4 and 4:2:0 is supported.\n");
        return AVERROR(EINVAL);
    }

    header.magic = MKTAG('P', 'W', 'V', '1');
    header.reference_bit_depth = pix_desc->comp[0].depth;
    header.frame_rate_num = st->codecpar->framerate.num;
    header.frame_rate_den = st->codecpar->framerate.den;
    header.header_version = 1;
    header.pyro.chroma_resolution = pix_desc->log2_chroma_w == 1 ?
        CHROMA_RESOLUTION_420 : CHROMA_RESOLUTION_444;
    header.pyro.width_minus_1 = st->codecpar->width - 1;
    header.pyro.height_minus_1 = st->codecpar->height - 1;

    // Default to
    header.pyro.color_primaries = st->codecpar->color_primaries == AVCOL_PRI_BT2020 ?
        COLOR_PRIMARIES_BT2020 : COLOR_PRIMARIES_SRGB;
    header.pyro.transfer_function = st->codecpar->color_trc == AVCOL_TRC_SMPTE2084 ?
        TRANSFER_FUNCTION_PQ : TRANSFER_FUNCTION_SRGB;
    header.pyro.ycbcr_range = st->codecpar->color_range == AVCOL_RANGE_JPEG ?
        YCBCR_RANGE_FULL : YCBCR_RANGE_LIMITED;
    header.pyro.ycbcr_transform = st->codecpar->color_space == AVCOL_SPC_BT2020_NCL ?
        YCBCR_TRANSFORM_BT2020 : YCBCR_TRANSFORM_BT709;

    // Make some default assumptions if chroma siting is not specified.
    if (st->codecpar->color_range == AVCOL_RANGE_JPEG && st->codecpar->chroma_location == AVCHROMA_LOC_UNSPECIFIED) {
        header.pyro.chroma_siting = CHROMA_SITING_CENTER;
        av_log(s, AV_LOG_WARNING, "Chroma siting is not set, assuming CENTER due to JPEG range.\n");
    } else if (st->codecpar->color_range == AVCOL_RANGE_MPEG && st->codecpar->chroma_location == AVCHROMA_LOC_UNSPECIFIED) {
        header.pyro.chroma_siting = CHROMA_SITING_LEFT;
        av_log(s, AV_LOG_WARNING, "Chroma siting is not set, guessing LEFT due to MPEG range.\n");
    } else if (st->codecpar->chroma_location == AVCHROMA_LOC_CENTER) {
        header.pyro.chroma_siting = CHROMA_SITING_CENTER;
    } else if (st->codecpar->chroma_location == AVCHROMA_LOC_LEFT) {
        header.pyro.chroma_siting = CHROMA_SITING_LEFT;
    } else if (st->codecpar->color_range == AVCOL_RANGE_JPEG) {
        av_log(s, AV_LOG_WARNING, "Unsupported chroma siting, assuming CENTER.\n");
        header.pyro.chroma_siting = CHROMA_SITING_CENTER;
    } else {
        av_log(s, AV_LOG_WARNING, "Unsupported chroma siting, assuming LEFT.\n");
        header.pyro.chroma_siting = CHROMA_SITING_LEFT;
    }

    avio_write(pb, (const void *)&header, sizeof(header));
    return 0;
}

static int pyrowave_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    avio_wl32(pb, pkt->size);
    avio_write(pb, pkt->data, pkt->size);
    return 0;
}

const FFOutputFormat ff_pyrowave_muxer = {
    .p.name           = "pyrowave",
    .p.long_name      = NULL_IF_CONFIG_SMALL("pyrowave raw bitstream"),
    .p.extensions     = "pwv1",
    .p.audio_codec    = AV_CODEC_ID_NONE,
    .p.video_codec    = AV_CODEC_ID_PYROWAVE,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH,
    .init             = pyrowave_init,
    .write_header     = pyrowave_write_header,
    .write_packet     = pyrowave_write_packet,
};
