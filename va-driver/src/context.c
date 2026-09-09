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

#include "context.h"
#include "config.h"
#include "encode.h"
#include "request.h"
#include "surface.h"
#include "video.h"

#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>


#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

/*
 * CAPTURE/OUTPUT pool size for FFmpeg's lazy frame-pool growth.
 *
 * H.264 allows up to 16 reference frames, and FFmpeg routinely allocates well
 * past that per stream while it cycles surfaces through the output queue, so a
 * generous pool lets the lazy-bind path in picture.c satisfy a fresh surface
 * without recycling.
 *
 * But the pool cannot be a constant. An 8K NV12 frame is about 50 MiB, so 64
 * of them would ask for 3.2 GiB from a 512 MiB CMA region -- the allocation
 * fails and the stream never starts. Scale the depth down as frames grow, and
 * never below what the codec's reference handling actually needs.
 */
#define POOL_MAX_BUFFERS	64u
#define POOL_MIN_BUFFERS	20u
#define POOL_BUDGET_BYTES	(192u * 1024u * 1024u)

static unsigned int pool_depth(int width, int height)
{
	unsigned int frame_bytes = (unsigned int)width * (unsigned int)height * 3u / 2u;
	unsigned int depth;

	if (frame_bytes == 0)
		return POOL_MAX_BUFFERS;

	depth = POOL_BUDGET_BYTES / frame_bytes;
	if (depth > POOL_MAX_BUFFERS)
		depth = POOL_MAX_BUFFERS;
	if (depth < POOL_MIN_BUFFERS)
		depth = POOL_MIN_BUFFERS;

	return depth;
}

static unsigned int context_config_rate_control(struct object_config *config)
{
	int i;

	for (i = 0; i < config->attributes_count; i++)
		if (config->attributes[i].type == VAConfigAttribRateControl)
			return config->attributes[i].value;

	return VA_RC_CQP;
}

static int detect_capture_format(struct decoder_session *session)
{
	struct video_format *video_format;

	if (session->video_format != NULL)
		return 0;

	/*
	 * rkvdec advertises NV12 under VIDEO_CAPTURE_MPLANE. The single-plane
	 * form is kept for other V4L2 stateless decoders; the Allwinner tiled
	 * format this used to try is not produced by anything on this SoC.
	 */
	if (v4l2_find_format(session->video_fd,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			     V4L2_PIX_FMT_NV12)) {
		video_format = video_format_find_mplane(V4L2_PIX_FMT_NV12,
							true);
	} else if (v4l2_find_format(session->video_fd,
				    V4L2_BUF_TYPE_VIDEO_CAPTURE,
				    V4L2_PIX_FMT_NV12)) {
		video_format = video_format_find_mplane(V4L2_PIX_FMT_NV12,
							false);
	} else {
		return -1;
	}

	if (video_format == NULL)
		return -1;

	session->video_format = video_format;
	return 0;
}

int profile_to_pixelformat(VAProfile profile, unsigned int *pixelformat)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		*pixelformat = V4L2_PIX_FMT_H264_SLICE;
		return 0;

	case VAProfileHEVCMain:
		*pixelformat = V4L2_PIX_FMT_HEVC_SLICE;
		return 0;

	default:
		return -1;
	}
}

int request_ensure_v4l2_initialized(struct decoder_session *session,
				    VAProfile profile,
				    int picture_width,
				    int picture_height)
{
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	unsigned int pixelformat;
	unsigned int depth;
	int rc;

	if (profile_to_pixelformat(profile, &pixelformat) < 0)
		return -1;

	/*
	 * Re-enter freely, but only skip the work when the queues already
	 * describe *this* stream. Testing "configured at all" instead would
	 * leave a context decoding against the previous codec's OUTPUT
	 * pixelformat -- which is what selects the hardware backend -- or
	 * against buffers sized for the previous resolution.
	 */
	if (session->video_format != NULL &&
	    session->num_capture_buffers > 0 &&
	    session->programmed_pixelformat == pixelformat &&
	    session->programmed_width == picture_width &&
	    session->programmed_height == picture_height)
		return 0;

	/*
	 * Reprogramming means tearing the queues down first: S_FMT is refused
	 * with EBUSY once REQBUFS has latched a queue, and streaming buffers
	 * from the previous configuration must not survive into this one.
	 */
	if (session->num_capture_buffers > 0) {
		unsigned int old_output, old_capture;

		old_output = v4l2_type_video_output(
			session->video_format->v4l2_mplane);
		old_capture = v4l2_type_video_capture(
			session->video_format->v4l2_mplane);

		v4l2_set_stream(session->video_fd, old_output, false);
		v4l2_set_stream(session->video_fd, old_capture, false);
		v4l2_request_buffers(session->video_fd, old_output, 0);
		v4l2_request_buffers(session->video_fd, old_capture, 0);

		session->num_output_buffers = 0;
		session->num_capture_buffers = 0;
		session->next_output_buf = 0;
		session->next_capture_buf = 0;
		session->streaming = false;
	}

	if (detect_capture_format(session) < 0) {
		request_log("context: NV12 CAPTURE not advertised by driver\n");
		return -1;
	}

	video_format = session->video_format;
	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	/* S_FMT order matters on rk3588 rkvdec: OUTPUT first (codec
	 * pixelformat + coded width/height), CAPTURE afterwards. Doing
	 * CAPTURE first triggers EBUSY on the subsequent S_FMT(OUTPUT)
	 * because REQBUFS implicitly latched the queue. */
	rc = v4l2_set_format(session->video_fd, output_type, pixelformat,
			     picture_width, picture_height);
	if (rc < 0) {
		request_log("context: S_FMT(OUTPUT 0x%x) failed\n", pixelformat);
		return -1;
	}

	rc = v4l2_set_format(session->video_fd, capture_type,
			     video_format->v4l2_format,
			     picture_width, picture_height);
	if (rc < 0) {
		request_log("context: S_FMT(CAPTURE 0x%x) failed\n",
			    video_format->v4l2_format);
		return -1;
	}

	/*
	 * Decode mode and start code describe the queue, not a frame, so they
	 * are set once here -- after S_FMT has selected the codec and before
	 * REQBUFS latches the queue.
	 *
	 * rkvdec is frame-based: it wants one access unit per OUTPUT buffer
	 * with Annex-B start codes, and correspondingly does not advertise the
	 * per-slice controls.
	 */
	if (pixelformat == V4L2_PIX_FMT_H264_SLICE) {
		v4l2_set_control_value(session->video_fd, -1,
			V4L2_CID_STATELESS_H264_DECODE_MODE,
			V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED);
		v4l2_set_control_value(session->video_fd, -1,
			V4L2_CID_STATELESS_H264_START_CODE,
			V4L2_STATELESS_H264_START_CODE_ANNEX_B);
	} else if (pixelformat == V4L2_PIX_FMT_HEVC_SLICE) {
		v4l2_set_control_value(session->video_fd, -1,
			V4L2_CID_STATELESS_HEVC_DECODE_MODE,
			V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED);
		v4l2_set_control_value(session->video_fd, -1,
			V4L2_CID_STATELESS_HEVC_START_CODE,
			V4L2_STATELESS_HEVC_START_CODE_ANNEX_B);
	}

	depth = pool_depth(picture_width, picture_height);

	rc = v4l2_request_buffers(session->video_fd, output_type, depth);
	if (rc < 0) {
		request_log("context: REQBUFS(OUTPUT, %u) failed\n", depth);
		return -1;
	}

	rc = v4l2_request_buffers(session->video_fd, capture_type, depth);
	if (rc < 0) {
		request_log("context: REQBUFS(CAPTURE, %u) failed\n", depth);
		return -1;
	}

	session->num_output_buffers = depth;
	session->num_capture_buffers = depth;
	session->next_output_buf = 0;
	session->next_capture_buf = 0;
	session->programmed_pixelformat = pixelformat;
	session->programmed_width = picture_width;
	session->programmed_height = picture_height;

	/*
	 * Streaming starts later, from RequestEndPicture, once the first
	 * access unit has told us what the sequence actually is.
	 */
	session->streaming = false;

	return 0;
}

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	struct object_context *context_object = NULL;
	VASurfaceID *ids = NULL;
	VAContextID id;
	VAStatus status;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	id = object_heap_allocate(&driver_data->context_heap);
	context_object = CONTEXT(driver_data, id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	memset(context_object, 0, sizeof(*context_object));
	context_object->base.id = id;
	context_object->base.next_free = OBJECT_HEAP_ALLOCATED;
	context_object->session.video_fd = -1;
	context_object->session.media_fd = -1;
	context_object->encode.video_fd = -1;
	memset(&context_object->dpb, 0, sizeof(context_object->dpb));

	if (config_object->entrypoint == VAEntrypointEncSlice) {
		if (encode_context_create(driver_data, &context_object->encode,
					  config_object->profile,
					  context_config_rate_control(config_object),
					  picture_width, picture_height) < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}

		context_object->is_encoder = true;
	} else {
		if (decoder_session_open(driver_data, &context_object->session) < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}

		if (request_ensure_v4l2_initialized(&context_object->session,
						    config_object->profile,
						    picture_width,
						    picture_height) < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}
	}

	/* Snapshot the caller's surface_ids — its lifetime is unspecified
	 * by libva and we need it on context teardown. */
	if (surfaces_count > 0) {
		ids = malloc((size_t)surfaces_count * sizeof(VASurfaceID));
		if (ids == NULL) {
			status = VA_STATUS_ERROR_ALLOCATION_FAILED;
			goto error;
		}
		memcpy(ids, surfaces_ids,
		       (size_t)surfaces_count * sizeof(VASurfaceID));
	}

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->surfaces_ids = ids;
	context_object->surfaces_count = surfaces_count;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;

	*context_id = id;

	return VA_STATUS_SUCCESS;

error:
	if (ids != NULL)
		free(ids);

	if (context_object != NULL) {
		encode_context_destroy(&context_object->encode);
		decoder_session_close(&context_object->session);
		object_heap_free(&driver_data->context_heap,
				 (struct object_base *)context_object);
	}

	return status;
}

VAStatus RequestDestroyContext(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct decoder_session *session;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	VAStatus status;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	session = &context_object->session;
	video_format = session->video_format;

	/*
	 * Only this context's queues are torn down. Reaching for the driver's
	 * handles here would stop a decode running in another context.
	 */
	if (context_object->is_encoder) {
		encode_context_destroy(&context_object->encode);
	} else if (video_format != NULL) {
		output_type = v4l2_type_video_output(video_format->v4l2_mplane);
		capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

		v4l2_set_stream(session->video_fd, output_type, false);
		v4l2_set_stream(session->video_fd, capture_type, false);
		v4l2_request_buffers(session->video_fd, output_type, 0);
		v4l2_request_buffers(session->video_fd, capture_type, 0);
	}

	status = RequestDestroySurfaces(context, context_object->surfaces_ids,
					context_object->surfaces_count);
	if (status != VA_STATUS_SUCCESS)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	free(context_object->surfaces_ids);

	/*
	 * Surfaces created outside this context -- Chrome creates its contexts
	 * with no render-target list -- are not on surfaces_ids and survive
	 * it, so they have to be cut loose before the slot is recycled.
	 */
	if (!context_object->is_encoder)
		surface_detach_session(driver_data, session);

	/*
	 * Closing the handles is what actually releases the decode session;
	 * the cached description of it goes with them, so a context created
	 * afterwards cannot read "already configured" from a queue that is
	 * gone.
	 */
	if (!context_object->is_encoder)
		decoder_session_close(session);

	object_heap_free(&driver_data->context_heap,
			 (struct object_base *)context_object);

	return VA_STATUS_SUCCESS;
}
