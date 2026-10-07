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

#ifndef PYROWAVE_COMMON_H
#define PYROWAVE_COMMON_H

#include <stdint.h>
#include <libavutil/pixfmt.h>
#include <vulkan/vulkan.h>
#include <pyrowave.h>

struct PyroWaveBitstreamSequenceHeader
{
    uint32_t width_minus_1 : 14;
    uint32_t height_minus_1 : 14;
    uint32_t sequence : 3;
    uint32_t extended : 1;
    uint32_t total_blocks : 24;
    uint32_t code : 2;
    uint32_t chroma_resolution : 1;
    uint32_t color_primaries : 1;
    uint32_t transfer_function : 1;
    uint32_t ycbcr_transform : 1;
    uint32_t ycbcr_range : 1;
    uint32_t chroma_siting : 1;
};

struct PWV1Header
{
    uint32_t magic;
    struct PyroWaveBitstreamSequenceHeader pyro;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint8_t reference_bit_depth;
    uint8_t header_version; // Must be 1.
    uint8_t padding[2]; // Reserved, must be 0.
};

enum {
    CHROMA_RESOLUTION_420 = 0,
    CHROMA_RESOLUTION_444 = 1
};

enum {
    CHROMA_SITING_CENTER = 0,
    CHROMA_SITING_LEFT = 1
};

enum {
    YCBCR_RANGE_FULL = 0,
    YCBCR_RANGE_LIMITED = 1
};

enum {
    COLOR_PRIMARIES_SRGB = 0, // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    COLOR_PRIMARIES_BT2020 = 1 // VK_COLOR_SPACE_HDR10_ST2084
};

enum {
    YCBCR_TRANSFORM_BT709 = 0,
    YCBCR_TRANSFORM_BT2020 = 1
};

enum {
    TRANSFER_FUNCTION_SRGB = 0, // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    TRANSFER_FUNCTION_PQ = 1 // VK_COLOR_SPACE_HDR10_ST2084
};

struct pyrowave_format_mapping {
    int chroma_resolution;
    int depth;
    enum AVPixelFormat pix_fmt;
    pyrowave_cpu_buffer_format pyro_cpu_format;
    int subsampled;
};

static const struct pyrowave_format_mapping pwv1_format_mapping[] = {
    { CHROMA_RESOLUTION_420, 8, AV_PIX_FMT_YUV420P, PYROWAVE_CPU_BUFFER_FORMAT_YUV420P, 1 },
    { CHROMA_RESOLUTION_420, 10, AV_PIX_FMT_YUV420P10, PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10, 1 },
    { CHROMA_RESOLUTION_420, 16, AV_PIX_FMT_YUV420P16, PYROWAVE_CPU_BUFFER_FORMAT_YUV420P16, 1 },
    { CHROMA_RESOLUTION_444, 8, AV_PIX_FMT_YUV444P, PYROWAVE_CPU_BUFFER_FORMAT_YUV444P, 0 },
    { CHROMA_RESOLUTION_444, 10, AV_PIX_FMT_YUV444P10, PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10, 0 },
    { CHROMA_RESOLUTION_444, 16, AV_PIX_FMT_YUV444P16, PYROWAVE_CPU_BUFFER_FORMAT_YUV444P16, 0 },
};

#endif

