/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
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

#ifndef _CONTEXT_H_
#define _CONTEXT_H_

#include <stdbool.h>
#include <stdint.h>

#include <va/va_backend.h>

#include "encode.h"
#include "object_heap.h"
#include "h264.h"
#include "session.h"
#include "vp9.h"

#define CONTEXT(data, id)                                                      \
	((struct object_context *)object_heap_lookup(&(data)->context_heap, id))
#define CONTEXT_ID_OFFSET		0x02000000

struct object_context {
	struct object_base base;

	VAConfigID config_id;
	VASurfaceID render_surface_id;

	int picture_width;
	int picture_height;
	int flags;

	/* This context's own decoder handles and queue state. */
	struct decoder_session session;

	bool is_encoder;
	struct encode_context encode;
	struct encode_picture_params encode_params;

	/* H264 only */
	struct h264_dpb dpb;

	/* VP9 loop-filter and segmentation state inherited across frames. */
	struct vp9_persistent_state vp9_state;

};

int profile_to_pixelformat(VAProfile profile, unsigned int *pixelformat);

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id);
VAStatus RequestDestroyContext(VADriverContextP context,
			       VAContextID context_id);

/*
 * Idempotent helper: configures the V4L2 device (S_FMT OUTPUT+CAPTURE,
 * REQBUFS, STREAMON) for the given codec profile and coded dimensions.
 * Safe to call multiple times — re-entries after the first successful
 * call return 0 immediately. Returns 0 on success, -1 on failure.
 *
 * Used by RequestCreateContext (canonical client path) and by
 * RequestCreateSurfaces2 (eager init for probe-pattern clients such as
 * mpv --hwdec=vaapi and VLC glconv_vaapi that touch vaDeriveImage /
 * vaExportSurfaceHandle before vaCreateContext).
 */
struct request_data;
int request_ensure_v4l2_initialized(struct decoder_session *session,
				    VAProfile profile,
				    int picture_width,
				    int picture_height);

#endif
