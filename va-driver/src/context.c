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

#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#endif

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
 * fails and the stream never starts. Scale the CAPTURE depth down as frames
 * grow, and never below what the codec's reference handling actually needs.
 *
 * RK3588 AV1 needs special accounting. Its Hantro post-processor writes the
 * linear CAPTURE image while the decoder keeps a tiled image plus motion
 * vectors for the eight AV1 references and the current frame. The kernel caps
 * that private pool at nine buffers; charge those buffers here before sizing
 * the public pool. Otherwise 1440p/4K exhausts CMA at STREAMON even though the
 * CAPTURE allocation itself succeeded.
 *
 * OUTPUT is different: decoding is synchronous and picture.c releases the
 * compressed-frame slot as soon as the request completes. Three OUTPUT slots
 * therefore suffice and, at 4 MiB each for 4K, leave enough CMA for the 32
 * live CAPTURE surfaces Chrome reaches while presenting a 4K stream.
 */
#define POOL_MAX_BUFFERS	DECODER_SESSION_MAX_BUFFERS
#define POOL_MIN_BUFFERS	20u
/* Below this the pool cannot hold a reference frame and the current one. */
#define POOL_FLOOR_BUFFERS	3u
#define POOL_BUDGET_BYTES	(384u * 1024u * 1024u)
#define AV1_POOL_MAX_BUFFERS	25u
#define AV1_REFERENCE_BUFFERS	9u
#define AV1_POOL_BUDGET_BYTES	(448u * 1024u * 1024u)

static uint64_t av1_motion_vector_bytes(unsigned int width,
					unsigned int height)
{
	uint64_t superblocks = ((uint64_t)width + 63) / 64 *
			       (((uint64_t)height + 63) / 64);

	/* Keep this in sync with hantro_av1_mv_size() in the kernel driver. */
	return superblocks * 384 * 2 + 512;
}

static unsigned int pool_depth(int width, int height, unsigned int bpp,
			       unsigned int pixelformat)
{
	uint64_t frame_bytes = (uint64_t)(unsigned int)width *
			       (unsigned int)height * bpp / 8;
	uint64_t budget = POOL_BUDGET_BYTES;
	unsigned int maximum = POOL_MAX_BUFFERS;
	unsigned int minimum = POOL_MIN_BUFFERS;
	unsigned int depth;

	if (frame_bytes == 0)
		return POOL_MAX_BUFFERS;

	if (pixelformat == V4L2_PIX_FMT_AV1_FRAME) {
		uint64_t private_bytes = frame_bytes +
			av1_motion_vector_bytes(width, height);

		budget = AV1_POOL_BUDGET_BYTES;
		maximum = AV1_POOL_MAX_BUFFERS;
		minimum = AV1_REFERENCE_BUFFERS;
		if (private_bytes * AV1_REFERENCE_BUFFERS >= budget)
			return minimum;
		budget -= private_bytes * AV1_REFERENCE_BUFFERS;
	}

	depth = budget / frame_bytes;
	if (depth > maximum)
		depth = maximum;
	if (depth < minimum)
		depth = minimum;

	return depth;
}

/*
 * REQBUFS may fail outright when CMA is fragmented instead of returning a
 * smaller allocation. Try the desired size first, then retreat. Values above
 * @want are skipped; the ladder is fallback policy, not implicit rounding.
 */
static int request_buffers_backoff(int video_fd, unsigned int type,
				   unsigned int want, unsigned int *count)
{
	static const unsigned int ladder[] = { 64, 48, 32, 24, 20, 16, 12, 10, 6, 3 };
	unsigned int i;

	if (v4l2_request_buffers(video_fd, type, want, count) >= 0)
		return 0;

	for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		if (ladder[i] >= want)
			continue;
		if (v4l2_request_buffers(video_fd, type, ladder[i], count) >= 0)
			return 0;
	}

	return -1;
}

static unsigned int context_config_rate_control(struct object_config *config)
{
	int i;

	for (i = 0; i < config->attributes_count; i++)
		if (config->attributes[i].type == VAConfigAttribRateControl)
			return config->attributes[i].value;

	return VA_RC_CQP;
}

static bool context_surface_matches_config(struct request_data *driver_data,
					   VASurfaceID surface_id,
					   struct object_config *config)
{
	struct object_surface *surface = SURFACE(driver_data, surface_id);
	enum surface_role expected_role =
		config->entrypoint == VAEntrypointEncSlice ?
		SURFACE_ROLE_ENCODE : SURFACE_ROLE_DECODE;

	return surface != NULL &&
	       (surface->rt_format &
		config_profile_rt_formats(config->profile)) != 0 &&
	       ((surface->rt_format == VA_RT_FORMAT_YUV420 &&
		 surface->pixel_format == VA_FOURCC_NV12) ||
		(surface->rt_format == VA_RT_FORMAT_YUV420_10 &&
		 surface->pixel_format == VA_FOURCC_P010)) &&
	       (surface->role == SURFACE_ROLE_UNKNOWN ||
		surface->role == expected_role);
}

static int detect_capture_format(struct decoder_session *session,
				 unsigned int coded_format,
				 unsigned int capture_format)
{
	struct video_format *video_format;
	bool mplane;

	/*
	 * rkvdec advertises NV12 under VIDEO_CAPTURE_MPLANE. The single-plane
	 * form is kept for other V4L2 stateless decoders; the Allwinner tiled
	 * format this used to try is not produced by anything on this SoC.
	 */
	if (v4l2_find_format(session->video_fd,
			     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			     coded_format)) {
		mplane = true;
	} else if (v4l2_find_format(session->video_fd,
				    V4L2_BUF_TYPE_VIDEO_OUTPUT,
				    coded_format)) {
		mplane = false;
	} else {
		return -1;
	}

	video_format = video_format_find_mplane(capture_format, mplane);

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

	case VAProfileVP9Profile0:
		*pixelformat = V4L2_PIX_FMT_VP9_FRAME;
		return 0;

	case VAProfileAV1Profile0:
		*pixelformat = V4L2_PIX_FMT_AV1_FRAME;
		return 0;

	default:
		return -1;
	}
}

int request_ensure_v4l2_initialized(struct decoder_session *session,
				    VAProfile profile,
				    unsigned int rt_format,
				    int picture_width,
				    int picture_height)
{
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	unsigned int pixelformat;
	unsigned int capture_pixelformat;
	unsigned int capture_depth;
	unsigned int output_depth = POOL_FLOOR_BUFFERS;
	unsigned int output_count = 0;
	unsigned int capture_count = 0;
	int rc;

	if (profile_to_pixelformat(profile, &pixelformat) < 0)
		return -1;

	capture_pixelformat = profile == VAProfileAV1Profile0 &&
		(rt_format & VA_RT_FORMAT_YUV420_10) != 0 &&
		(rt_format & VA_RT_FORMAT_YUV420) == 0 ?
		V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;

	/*
	 * Re-enter freely, but only skip the work when the queues already
	 * describe *this* stream. Testing "configured at all" instead would
	 * leave a context decoding against the previous codec's OUTPUT
	 * pixelformat -- which is what selects the hardware backend -- or
	 * against buffers sized for the previous resolution.
	 */
	if (session->video_format != NULL &&
	    session->num_capture_buffers > 0 &&
	    session->video_format->v4l2_format == capture_pixelformat &&
	    session->programmed_pixelformat == pixelformat &&
	    session->programmed_profile == profile &&
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
		v4l2_request_buffers(session->video_fd, old_output, 0, NULL);
		v4l2_request_buffers(session->video_fd, old_capture, 0, NULL);

		session->num_output_buffers = 0;
		session->num_capture_buffers = 0;
		session->next_output_buf = 0;
		session->next_capture_buf = 0;
		session->free_output_count = 0;
		session->free_capture_count = 0;
		session->streaming = false;
	}

	if (detect_capture_format(session, pixelformat,
				  capture_pixelformat) < 0) {
		request_log("context: no CAPTURE layout for VA profile %d\n",
			    profile);
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

	/* S_FMT(OUTPUT) resets rkvdec's image-format state. Select Profile 0
	 * before CAPTURE S_FMT/REQBUFS so this VA backend always receives
	 * NV12. Profile 2 remains available through the raw V4L2 interface. */
	if (profile == VAProfileVP9Profile0) {
		rc = v4l2_set_control_value(session->video_fd, -1,
					    V4L2_CID_MPEG_VIDEO_VP9_PROFILE,
					    V4L2_MPEG_VIDEO_VP9_PROFILE_0);
		if (rc < 0) {
			request_log("context: VP9 Profile 0 rejected\n");
			return -1;
		}
	} else if (profile == VAProfileAV1Profile0) {
		struct v4l2_ctrl_av1_sequence seq;

		/* The Hantro post-processor exposes P010 only after seeing the
		 * sequence bit depth. Chrome exports its frame pool before the first
		 * picture, so prime that choice before CAPTURE S_FMT/REQBUFS. */
		memset(&seq, 0, sizeof(seq));
		seq.seq_profile = 0;
		seq.order_hint_bits = 1;
		seq.bit_depth = capture_pixelformat == V4L2_PIX_FMT_P010 ? 10 : 8;
		seq.max_frame_width_minus_1 = picture_width - 1;
		seq.max_frame_height_minus_1 = picture_height - 1;
		seq.flags = V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X |
			    V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
		rc = v4l2_set_control(session->video_fd, -1,
				      V4L2_CID_STATELESS_AV1_SEQUENCE,
				      &seq, sizeof(seq));
		if (rc < 0) {
			request_log("context: AV1 sequence setup failed\n");
			return -1;
		}
	}

	if (!v4l2_find_format(session->video_fd, capture_type,
			      video_format->v4l2_format)) {
		request_log("context: CAPTURE 0x%x not advertised for profile %d\n",
			    video_format->v4l2_format, profile);
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

	capture_depth = pool_depth(picture_width, picture_height,
				   video_format->bpp, pixelformat);

	rc = request_buffers_backoff(session->video_fd, output_type,
				     output_depth, &output_count);
	if (rc < 0) {
		request_log("context: REQBUFS(OUTPUT, %u) failed\n",
			    output_depth);
		return -1;
	}

	rc = request_buffers_backoff(session->video_fd, capture_type,
				     capture_depth, &capture_count);
	if (rc < 0) {
		request_log("context: REQBUFS(CAPTURE, %u) failed\n",
			    capture_depth);
		return -1;
	}

	/*
	 * Carry on with what the kernel gave rather than what was asked for,
	 * but not below the point where the pool cannot hold a reference and
	 * the frame being decoded into at the same time.
	 */
	if (output_count < POOL_FLOOR_BUFFERS ||
	    capture_count < POOL_FLOOR_BUFFERS) {
		request_log("context: pool too small: %u output, %u capture "
			    "(%u/%u requested)\n", output_count, capture_count,
			    output_depth, capture_depth);
		return -1;
	}

	if (output_count < output_depth || capture_count < capture_depth)
		request_log("context: pool short: %u/%u output, %u/%u capture\n",
			    output_count, output_depth,
			    capture_count, capture_depth);

	session->num_output_buffers = output_count;
	session->num_capture_buffers = capture_count;
	session->next_output_buf = 0;
	session->next_capture_buf = 0;
	session->free_output_count = 0;
	session->free_capture_count = 0;
	session->programmed_pixelformat = pixelformat;
	session->programmed_profile = profile;
	session->programmed_width = picture_width;
	session->programmed_height = picture_height;

	/*
	 * Streaming starts later, from RequestEndPicture, once the first
	 * access unit has told us what the sequence actually is.
	 */
	session->streaming = false;

	return 0;
}

int request_ensure_av1_capture_bit_depth(struct decoder_session *session,
					 int picture_width,
					 int picture_height,
					 unsigned int bit_depth)
{
	struct video_format *old_format = session->video_format;
	struct v4l2_ctrl_av1_sequence seq;
	unsigned int old_capture_type, capture_type;
	unsigned int capture_pixelformat;
	unsigned int capture_depth, capture_count = 0;
	int rc;

	if (session->programmed_pixelformat != V4L2_PIX_FMT_AV1_FRAME)
		return 0;

	if (bit_depth == 8)
		capture_pixelformat = V4L2_PIX_FMT_NV12;
	else if (bit_depth == 10)
		capture_pixelformat = V4L2_PIX_FMT_P010;
	else
		return -1;

	if (old_format != NULL &&
	    old_format->v4l2_format == capture_pixelformat)
		return 0;

	/* Exported or decoded buffers cannot be silently replaced underneath
	 * their owner. The fallback is only for clients such as FFmpeg which
	 * reveal AV1 bit depth with the first picture parameter buffer. */
	if (old_format == NULL || session->streaming ||
	    session->next_capture_buf > 0 || session->free_capture_count > 0) {
		request_log("context: cannot switch AV1 CAPTURE format after buffers were bound\n");
		return -1;
	}

	old_capture_type = v4l2_type_video_capture(old_format->v4l2_mplane);
	if (v4l2_request_buffers(session->video_fd, old_capture_type, 0, NULL) < 0)
		return -1;
	session->num_capture_buffers = 0;
	session->next_capture_buf = 0;
	session->free_capture_count = 0;

	memset(&seq, 0, sizeof(seq));
	seq.seq_profile = 0;
	seq.order_hint_bits = 1;
	seq.bit_depth = bit_depth;
	seq.max_frame_width_minus_1 = picture_width - 1;
	seq.max_frame_height_minus_1 = picture_height - 1;
	seq.flags = V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X |
		    V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
	if (v4l2_set_control(session->video_fd, -1,
			     V4L2_CID_STATELESS_AV1_SEQUENCE,
			     &seq, sizeof(seq)) < 0)
		return -1;

	session->video_format = NULL;
	if (detect_capture_format(session, V4L2_PIX_FMT_AV1_FRAME,
				  capture_pixelformat) < 0)
		return -1;

	capture_type = v4l2_type_video_capture(
		session->video_format->v4l2_mplane);
	if (!v4l2_find_format(session->video_fd, capture_type,
			      capture_pixelformat) ||
	    v4l2_set_format(session->video_fd, capture_type,
			    capture_pixelformat, picture_width,
			    picture_height) < 0)
		return -1;

	capture_depth = pool_depth(picture_width, picture_height,
				   session->video_format->bpp,
				   V4L2_PIX_FMT_AV1_FRAME);
	rc = request_buffers_backoff(session->video_fd, capture_type,
				     capture_depth, &capture_count);
	if (rc < 0 || capture_count < POOL_FLOOR_BUFFERS)
		return -1;

	session->num_capture_buffers = capture_count;
	request_log("context: switched AV1 CAPTURE to %s for %u-bit stream\n",
		    session->video_format->description, bit_depth);
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
	VAContextID id;
	int i;
	VAStatus status;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	if (picture_width <= 0 || picture_height <= 0 || surfaces_count < 0 ||
	    (surfaces_count > 0 && surfaces_ids == NULL))
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	for (i = 0; i < surfaces_count; i++)
		if (!context_surface_matches_config(driver_data, surfaces_ids[i],
						    config_object))
			return VA_STATUS_ERROR_INVALID_SURFACE;

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
		if (decoder_session_open(driver_data, &context_object->session,
					 config_object->profile) < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}

		if (request_ensure_v4l2_initialized(&context_object->session,
						    config_object->profile,
						    config_rt_format(config_object),
						    picture_width,
						    picture_height) < 0) {
			status = VA_STATUS_ERROR_OPERATION_FAILED;
			goto error;
		}
	}

	/*
	 * The render targets are stamped with what this context will use them
	 * for and then let go. They belong to the client, which created them
	 * with vaCreateSurfaces and will destroy them with vaDestroySurfaces;
	 * a context that kept the list would only be tempted to act on it.
	 */
	for (i = 0; i < surfaces_count; i++)
		surface_set_role(driver_data, surfaces_ids[i],
				 context_object->is_encoder ?
					SURFACE_ROLE_ENCODE :
					SURFACE_ROLE_DECODE);

	context_object->config_id = config_id;
	context_object->render_surface_id = VA_INVALID_ID;
	context_object->picture_width = picture_width;
	context_object->picture_height = picture_height;
	context_object->flags = flags;

	*context_id = id;

	return VA_STATUS_SUCCESS;

error:
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

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	session = &context_object->session;
	video_format = session->video_format;

	/*
	 * A context owns its queues and its session. It does not own the
	 * surfaces it was handed: those come from vaCreateSurfaces and go back
	 * through vaDestroySurfaces, which the client calls separately and in
	 * whichever order it likes -- libva-utils destroys the context first,
	 * then the surfaces.
	 *
	 * Destroying them from here returned their heap slots to the free list
	 * while the client still held the ids. The client's own
	 * vaDestroySurfaces then failed, or, if anything had allocated a
	 * surface in between, tore down whichever live surface had been handed
	 * that recycled slot. Called the other way round it was worse: the
	 * lookups came back NULL, this function bailed out early, and the
	 * session's two file descriptors were never closed.
	 *
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
		v4l2_request_buffers(session->video_fd, output_type, 0, NULL);
		v4l2_request_buffers(session->video_fd, capture_type, 0, NULL);
	}

	/*
	 * Surfaces outlive the context, so whatever they hold of this session
	 * has to be taken back before its handles close: their mappings point
	 * into queues that are about to go away. Encode surfaces carry no
	 * session state, only their own dma-buf store, so there is nothing to
	 * detach on that path.
	 *
	 * Closing the handles is what actually releases the decode session;
	 * the cached description of it goes with them, so a context created
	 * afterwards cannot read "already configured" from a queue that is
	 * gone.
	 */
	if (!context_object->is_encoder) {
		surface_detach_session(driver_data, session);
		decoder_session_close(session);
	}

	object_heap_free(&driver_data->context_heap,
			 (struct object_base *)context_object);

	return VA_STATUS_SUCCESS;
}
