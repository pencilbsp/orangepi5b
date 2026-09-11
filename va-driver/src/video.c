/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "utils.h"
#include "video.h"

#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#endif

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 fourcc_code('P', '0', '1', '0')
#endif

static struct video_format formats[] = {
	{
		.description		= "P010 YUV (MPLANE)",
		.v4l2_format		= V4L2_PIX_FMT_P010,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= true,
		.drm_format		= DRM_FORMAT_P010,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 24,
		.height_alignment	= 1,
	},
	{
		.description		= "P010 YUV",
		.v4l2_format		= V4L2_PIX_FMT_P010,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= false,
		.drm_format		= DRM_FORMAT_P010,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 24,
		.height_alignment	= 1,
	},
	{
		.description		= "NV12 YUV (MPLANE)",
		.v4l2_format		= V4L2_PIX_FMT_NV12,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= true,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 12,
		.height_alignment	= 1,
	},
	{
		.description		= "NV12 YUV",
		.v4l2_format		= V4L2_PIX_FMT_NV12,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= false,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_NONE,
		.planes_count		= 2,
		.bpp			= 12,
		.height_alignment	= 1,
	},
	{
		.description		= "Sunxi tiled NV12 YUV",
		.v4l2_format		= V4L2_PIX_FMT_SUNXI_TILED_NV12,
		.v4l2_buffers_count	= 1,
		.v4l2_mplane		= false,
		.drm_format		= DRM_FORMAT_NV12,
		.drm_modifier		= DRM_FORMAT_MOD_ALLWINNER_TILED,
		.planes_count		= 2,
		.bpp			= 12,
		.height_alignment	= 1,
	},
};

static unsigned int formats_count = sizeof(formats) / sizeof(formats[0]);

struct video_format *video_format_find_mplane(unsigned int pixelformat,
					      bool mplane)
{
	unsigned int i;

	for (i = 0; i < formats_count; i++)
		if (formats[i].v4l2_format == pixelformat &&
		    formats[i].v4l2_mplane == mplane)
			return &formats[i];

	return NULL;
}

bool video_format_is_linear(struct video_format *format)
{
	if (format == NULL)
		return true;

	return format->drm_modifier == DRM_FORMAT_MOD_NONE;
}

unsigned int video_format_storage_height(struct video_format *format,
					 unsigned int height)
{
	unsigned int alignment;

	if (format == NULL)
		return height;

	alignment = format->height_alignment;
	if (alignment <= 1)
		return height;

	return (height + alignment - 1) & ~(alignment - 1);
}
