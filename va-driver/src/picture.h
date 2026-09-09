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

#ifndef _PICTURE_H_
#define _PICTURE_H_

#include <va/va_backend.h>

#include "session.h"

#include "object_heap.h"

struct request_data;
struct object_surface;

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
			     VASurfaceID surface_id);
VAStatus RequestRenderPicture(VADriverContextP context, VAContextID context_id,
			      VABufferID *buffers, int buffers_count);
VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id);

/*
 * Bind a freshly-allocated VA surface to the next free V4L2 CAPTURE
 * pool slot: assigns destination_index, mmaps the buffer, fills
 * destination_data / offsets / sizes / bytesperlines.
 *
 * Idempotent: returns 0 immediately if surface_object->destination_index
 * is already valid. Requires driver_data->video_format to be set
 * (request_ensure_v4l2_initialized() must have succeeded).
 *
 * Returns 0 on success, -1 on failure. Best-effort callers may ignore
 * the return — the lazy-bind path in RequestBeginPicture will retry.
 */
int request_bind_destination_buffer(struct decoder_session *session,
				    struct object_surface *surface_object);

#endif
