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

#include "picture.h"
#include "buffer.h"
#include "config.h"
#include "context.h"
#include "encode.h"
#include "request.h"
#include "surface.h"
#include "video.h"

#include "h264.h"
#include "h265.h"
#include "vp9.h"

#include <assert.h>
#include <string.h>

#include <errno.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

/*
 * Bind a free OUTPUT pool buffer to @surface_object on first use and
 * mmap it as the source-side slice-data staging area. The pool was
 * sized in RequestCreateContext to absorb FFmpeg's lazy frame growth.
 * Destroyed surfaces put their indices on the session's free stacks, so a
 * long-running Chrome process does not consume the pool monotonically.
 */
static int bind_source_buffer(struct decoder_session *session,
			      struct object_surface *surface_object)
{
	struct video_format *video_format = session->video_format;
	unsigned int output_type, length, offset;
	int rc;

	if (surface_object->source_index != SURFACE_INDEX_UNASSIGNED)
		return 0;

	if (session->free_output_count == 0 &&
	    session->next_output_buf >= session->num_output_buffers) {
		request_log("picture: OUTPUT pool exhausted (%u)\n",
			    session->num_output_buffers);
		return -1;
	}

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	if (session->free_output_count > 0)
		surface_object->source_index =
			session->free_output[--session->free_output_count];
	else
		surface_object->source_index = session->next_output_buf++;
	surface_object->session = session;

	rc = v4l2_query_buffer(session->video_fd, output_type,
			       surface_object->source_index, &length, &offset,
			       1);
	if (rc < 0)
		return -1;

	surface_object->source_data = mmap(NULL, length,
					   PROT_READ | PROT_WRITE, MAP_SHARED,
					   session->video_fd, offset);
	if (surface_object->source_data == MAP_FAILED) {
		surface_object->source_data = NULL;
		return -1;
	}
	surface_object->source_size = length;
	return 0;
}

/*
 * A coded OUTPUT buffer is needed only from BeginPicture through the
 * synchronous completion in EndPicture. Return it immediately afterwards;
 * keeping one per displayed surface wastes 4 MiB each at 4K and prevents the
 * CAPTURE pool from being large enough for Chrome's live frame set.
 */
static void release_source_buffer(struct decoder_session *session,
				  struct object_surface *surface_object)
{
	if (surface_object->source_data != NULL &&
	    surface_object->source_data != MAP_FAILED &&
	    surface_object->source_size > 0)
		munmap(surface_object->source_data, surface_object->source_size);

	surface_object->source_data = NULL;
	surface_object->source_size = 0;

	if (surface_object->source_index != SURFACE_INDEX_UNASSIGNED &&
	    session->free_output_count < DECODER_SESSION_MAX_BUFFERS)
		session->free_output[session->free_output_count++] =
			surface_object->source_index;

	surface_object->source_index = SURFACE_INDEX_UNASSIGNED;
}

int request_bind_destination_buffer(struct decoder_session *session,
				    struct object_surface *surface_object)
{
	struct video_format *video_format = session->video_format;
	unsigned int capture_type, format_width, format_height;
	unsigned int destination_sizes[VIDEO_MAX_PLANES] = { 0 };
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES] = { 0 };
	unsigned int destination_planes_count = video_format->planes_count;
	unsigned int j;
	int rc;

	if (surface_object->destination_index != SURFACE_INDEX_UNASSIGNED)
		return 0;

	if (session->free_capture_count == 0 &&
	    session->next_capture_buf >= session->num_capture_buffers) {
		request_log("picture: CAPTURE pool exhausted (%u)\n",
			    session->num_capture_buffers);
		return -1;
	}

	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_get_format(session->video_fd, capture_type,
			     &format_width, &format_height,
			     destination_bytesperlines, destination_sizes,
			     NULL);
	if (rc < 0)
		return -1;

	if (session->free_capture_count > 0)
		surface_object->destination_index =
			session->free_capture[--session->free_capture_count];
	else
		surface_object->destination_index = session->next_capture_buf++;
	/* Remember where the buffer came from: only this session can use it. */
	surface_object->session = session;
	surface_object->destination_buffers_count =
		video_format->v4l2_buffers_count;
	surface_object->destination_planes_count = destination_planes_count;

	rc = v4l2_query_buffer(session->video_fd, capture_type,
			       surface_object->destination_index,
			       surface_object->destination_map_lengths,
			       surface_object->destination_map_offsets,
			       video_format->v4l2_buffers_count);
	if (rc < 0)
		return -1;

	for (j = 0; j < video_format->v4l2_buffers_count; j++) {
		surface_object->destination_map[j] =
			mmap(NULL,
			     surface_object->destination_map_lengths[j],
			     PROT_READ | PROT_WRITE, MAP_SHARED,
			     session->video_fd,
			     surface_object->destination_map_offsets[j]);
		if (surface_object->destination_map[j] == MAP_FAILED) {
			surface_object->destination_map[j] = NULL;
			return -1;
		}
	}

	if (video_format->v4l2_buffers_count == 1) {
		destination_sizes[0] = destination_bytesperlines[0] *
				       format_height;
		for (j = 1; j < destination_planes_count; j++)
			destination_sizes[j] = destination_sizes[0] / 2;

		for (j = 0; j < destination_planes_count; j++) {
			surface_object->destination_offsets[j] =
				j > 0 ? destination_sizes[j - 1] : 0;
			surface_object->destination_data[j] =
				(unsigned char *)surface_object->destination_map[0] +
				surface_object->destination_offsets[j];
			surface_object->destination_sizes[j] =
				destination_sizes[j];
			surface_object->destination_bytesperlines[j] =
				destination_bytesperlines[0];
		}
	} else if (video_format->v4l2_buffers_count == destination_planes_count) {
		for (j = 0; j < destination_planes_count; j++) {
			surface_object->destination_offsets[j] = 0;
			surface_object->destination_data[j] =
				surface_object->destination_map[j];
			surface_object->destination_sizes[j] =
				destination_sizes[j];
			surface_object->destination_bytesperlines[j] =
				destination_bytesperlines[j];
		}
	} else {
		return -1;
	}

	return 0;
}

static VAStatus codec_store_buffer(struct request_data *driver_data,
				   VAProfile profile,
				   struct object_surface *surface_object,
				   struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VASliceDataBufferType: {
		static const unsigned char start_code[] = { 0x00, 0x00, 0x00, 0x01 };
		unsigned int size = buffer_object->size * buffer_object->count;
		unsigned int prefix_size = profile == VAProfileVP9Profile0 ? 0 :
					   sizeof(start_code);

		/*
		 * Since there is no guarantee that the allocation order is the
		 * same as the submission order (via RenderPicture), we can't
		 * use a V4L2 buffer directly and have to copy from a regular
		 * buffer.
		 *
		 * VA-API hands over each slice as a bare NAL unit: the header
		 * byte is there, the start code is not. The queue is programmed
		 * for Annex-B, which is what rkvdec wants, so the framing has
		 * to be put back on the way through. Without it the hardware is
		 * handed a buffer with no start code at offset 0 and every
		 * frame fails to decode.
		 */
		if (surface_object->slices_size + prefix_size + size >
		    surface_object->source_size) {
			request_log("picture: slice does not fit the coded buffer\n");
			return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
		}

		/* VP9_FRAME is an unframed frame. H.264/HEVC require Annex-B. */
		if (prefix_size != 0) {
			memcpy(surface_object->source_data + surface_object->slices_size,
			       start_code, sizeof(start_code));
			surface_object->slices_size += sizeof(start_code);
		}

		memcpy(surface_object->source_data + surface_object->slices_size,
		       buffer_object->data, size);
		surface_object->slices_size += size;
		surface_object->slices_count++;
		break;
	}

	case VAPictureParameterBufferType:
		switch (profile) {
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			memcpy(&surface_object->params.h264.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.picture));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.picture));
			break;

		case VAProfileVP9Profile0:
			memcpy(&surface_object->params.vp9.picture,
			       buffer_object->data,
			       sizeof(surface_object->params.vp9.picture));
			surface_object->params.vp9.picture_set = true;
			break;

		default:
			break;
		}
		break;

	case VASliceParameterBufferType:
		switch (profile) {
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			memcpy(&surface_object->params.h264.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.slice));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.slice));
			break;

		case VAProfileVP9Profile0:
			memcpy(&surface_object->params.vp9.slice,
			       buffer_object->data,
			       sizeof(surface_object->params.vp9.slice));
			surface_object->params.vp9.slice_set = true;
			break;

		default:
			break;
		}
		break;

	case VAIQMatrixBufferType:
		switch (profile) {
		case VAProfileH264ConstrainedBaseline:
		case VAProfileH264Main:
		case VAProfileH264High:
			memcpy(&surface_object->params.h264.matrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h264.matrix));
			break;

		case VAProfileHEVCMain:
			memcpy(&surface_object->params.h265.iqmatrix,
			       buffer_object->data,
			       sizeof(surface_object->params.h265.iqmatrix));
			surface_object->params.h265.iqmatrix_set = true;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus codec_set_controls(struct request_data *driver_data,
				   struct decoder_session *session,
				   struct object_context *context,
				   VAProfile profile,
				   struct object_surface *surface_object)
{
	int rc;

	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		rc = h264_set_controls(driver_data, session, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileHEVCMain:
		rc = h265_set_controls(driver_data, session, context, surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	case VAProfileVP9Profile0:
		rc = vp9_set_controls(driver_data, session, context,
				      surface_object);
		if (rc < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		break;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	return VA_STATUS_SUCCESS;
}

static bool session_matches_context(struct decoder_session *session,
				    struct object_config *config_object,
				    struct object_context *context_object)
{
	unsigned int pixelformat;

	if (profile_to_pixelformat(config_object->profile, &pixelformat) < 0)
		return false;

	return session->video_format != NULL &&
	       session->num_capture_buffers > 0 &&
	       session->programmed_pixelformat == pixelformat &&
	       session->programmed_width == context_object->picture_width &&
	       session->programmed_height == context_object->picture_height;
}

static void rewire_probe_surfaces(struct request_data *driver_data,
				  struct decoder_session *session)
{
	struct object_surface *surface_object;
	int iterator;

	surface_object = (struct object_surface *)object_heap_first(
		&driver_data->surface_heap, &iterator);
	while (surface_object != NULL) {
		if (surface_object->session == &driver_data->probe_session)
			surface_object->session = session;

		surface_object = (struct object_surface *)object_heap_next(
			&driver_data->surface_heap, &iterator);
	}
}

static void reset_probe_session(struct request_data *driver_data)
{
	memset(&driver_data->probe_session, 0,
	       sizeof(driver_data->probe_session));
	driver_data->probe_session.video_fd = -1;
	driver_data->probe_session.media_fd = -1;
}

static int context_adopt_probe_session(struct request_data *driver_data,
				       struct object_context *context_object,
				       struct object_config *config_object,
				       struct object_surface *surface_object)
{
	if (surface_object->session == NULL ||
	    surface_object->session == &context_object->session)
		return 0;

	if (surface_object->session != &driver_data->probe_session) {
		request_log("picture: surface belongs to another decode session\n");
		return -1;
	}

	if (context_object->session.streaming ||
	    context_object->session.next_output_buf != 0 ||
	    context_object->session.next_capture_buf != 0) {
		request_log("picture: cannot adopt probe session after decode started\n");
		return -1;
	}

	if (!session_matches_context(&driver_data->probe_session,
				     config_object, context_object)) {
		request_log("picture: exported probe session does not match context\n");
		return -1;
	}

	if (context_object->session.video_fd >= 0)
		decoder_session_close(&context_object->session);

	/*
	 * Chrome may create the context without a render-target list. In that
	 * case CreateContext cannot see the exported surfaces, so the first
	 * BeginPicture has to transfer the already-exported CAPTURE pool to
	 * the decode context. The compositor keeps dmabufs from this session;
	 * decode and SyncSurface must use the same file handles.
	 */
	context_object->session = driver_data->probe_session;
	rewire_probe_surfaces(driver_data, &context_object->session);
	reset_probe_session(driver_data);
	request_log("picture: adopted exported probe session for context 0x%x\n",
		    context_object->base.id);

	return 0;
}

VAStatus RequestBeginPicture(VADriverContextP context, VAContextID context_id,
			     VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	VAStatus status;
	int rc;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/*
	 * A surface can reach a context without ever having been a render
	 * target -- Chrome creates its contexts with an empty list -- so this
	 * is the other place its role becomes known.
	 */
	surface_set_role(driver_data, surface_id,
			 context_object->is_encoder ? SURFACE_ROLE_ENCODE :
						      SURFACE_ROLE_DECODE);

	if (context_object->is_encoder) {
		if (surface_object->status == VASurfaceRendering)
			RequestSyncSurface(context, surface_id);

		status = encode_surface_storage(surface_object);
		if (status != VA_STATUS_SUCCESS)
			return status;

		encode_params_reset(&context_object->encode_params);
		surface_object->status = VASurfaceRendering;
		surface_object->slices_count = 0;
		surface_object->slices_size = 0;
		context_object->render_surface_id = surface_id;

		return VA_STATUS_SUCCESS;
	}

	if (context_adopt_probe_session(driver_data, context_object,
					config_object, surface_object) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (surface_object->status == VASurfaceRendering)
		RequestSyncSurface(context, surface_id);

	rc = bind_source_buffer(&context_object->session, surface_object);
	if (rc < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	rc = request_bind_destination_buffer(&context_object->session,
					     surface_object);
	if (rc < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	surface_object->status = VASurfaceRendering;
	surface_object->slices_count = 0;
	surface_object->slices_size = 0;
	if (config_object->profile == VAProfileVP9Profile0)
		memset(&surface_object->params.vp9, 0,
		       sizeof(surface_object->params.vp9));
	context_object->render_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestRenderPicture(VADriverContextP context, VAContextID context_id,
			      VABufferID *buffers_ids, int buffers_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	int rc;
	int i;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	for (i = 0; i < buffers_count; i++) {
		buffer_object = BUFFER(driver_data, buffers_ids[i]);
		if (buffer_object == NULL)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		if (context_object->is_encoder) {
			rc = encode_params_collect(&context_object->encode_params,
						   buffer_object);
			if (rc != VA_STATUS_SUCCESS)
				return rc;
			continue;
		}

		rc = codec_store_buffer(driver_data, config_object->profile,
					surface_object, buffer_object);
		if (rc != VA_STATUS_SUCCESS)
			return rc;
	}

	return VA_STATUS_SUCCESS;
}

/*
 * Start streaming once the first access unit has arrived.
 *
 * A stateless decoder validates the sequence in its start hook, and a control
 * attached to a request is not applied until that request is queued -- which
 * cannot happen before STREAMON. So the sequence header from this first frame
 * is set on the device, and only then do the queues start.
 */
static int codec_begin_streaming(struct decoder_session *session,
				 VAProfile profile,
				 struct object_surface *surface,
				 unsigned int output_type,
				 unsigned int capture_type)
{
	int rc;

	if (session->streaming)
		return 0;

	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		rc = h264_set_device_sps(session, surface);
		break;

	case VAProfileHEVCMain:
		rc = h265_set_device_sps(session, surface);
		break;

	case VAProfileVP9Profile0:
		/* VP9 has no sequence control that must precede STREAMON. */
		rc = 0;
		break;

	default:
		return -1;
	}

	if (rc < 0) {
		request_log("picture: sequence header rejected before STREAMON\n");
		return -1;
	}

	if (v4l2_set_stream(session->video_fd, output_type, true) < 0)
		return -1;
	if (v4l2_set_stream(session->video_fd, capture_type, true) < 0) {
		v4l2_set_stream(session->video_fd, output_type, false);
		return -1;
	}

	session->streaming = true;
	return 0;
}

/* Put the one VP9 frame at byte zero of the V4L2 OUTPUT buffer. */
static int vp9_prepare_frame_data(struct object_surface *surface)
{
	const VASliceParameterBufferVP9 *slice = &surface->params.vp9.slice;
	unsigned int offset;
	unsigned int size;

	if (!surface->params.vp9.picture_set ||
	    !surface->params.vp9.slice_set || surface->slices_count != 1)
		return -1;

	offset = slice->slice_data_offset;
	size = slice->slice_data_size;
	if (offset > surface->slices_size ||
	    size > surface->slices_size - offset)
		return -1;

	if (offset != 0)
		memmove(surface->source_data,
			(unsigned char *)surface->source_data + offset, size);
	surface->slices_size = size;
	return 0;
}

VAStatus RequestEndPicture(VADriverContextP context, VAContextID context_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_context *context_object;
	struct object_config *config_object;
	struct object_surface *surface_object;
	struct decoder_session *session;
	struct video_format *video_format;
	struct object_buffer *coded_buffer;
	unsigned int output_type, capture_type;
	int request_fd;
	VAStatus status;
	int rc;

	context_object = CONTEXT(driver_data, context_id);
	if (context_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONTEXT;

	config_object = CONFIG(driver_data, context_object->config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	surface_object =
		SURFACE(driver_data, context_object->render_surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (context_object->is_encoder) {
		if (!context_object->encode_params.have_picture)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		coded_buffer = BUFFER(driver_data,
				      context_object->encode_params.picture.coded_buf);
		if (coded_buffer == NULL ||
		    coded_buffer->type != VAEncCodedBufferType)
			return VA_STATUS_ERROR_INVALID_BUFFER;

		status = encode_picture_submit(&context_object->encode,
					       surface_object,
					       &context_object->encode_params,
					       coded_buffer);
		if (status != VA_STATUS_SUCCESS)
			return status;

		surface_object->status = VASurfaceReady;
		context_object->render_surface_id = VA_INVALID_ID;

		return VA_STATUS_SUCCESS;
	}

	session = &context_object->session;
	video_format = session->video_format;
	if (video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	if (config_object->profile == VAProfileVP9Profile0 &&
	    vp9_prepare_frame_data(surface_object) < 0) {
		request_log("picture: invalid VP9 picture/slice data\n");
		return VA_STATUS_ERROR_INVALID_BUFFER;
	}

	gettimeofday(&surface_object->timestamp, NULL);

	request_fd = surface_object->request_fd;
	if (request_fd < 0) {
		request_fd = media_request_alloc(session->media_fd);
		if (request_fd < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;

		surface_object->request_fd = request_fd;
	}

	if (codec_begin_streaming(session, config_object->profile,
				  surface_object, output_type,
				  capture_type) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = codec_set_controls(driver_data, session, context_object,
				config_object->profile, surface_object);
	if (rc != VA_STATUS_SUCCESS)
		return rc;

	rc = v4l2_queue_buffer(session->video_fd, -1, capture_type, NULL,
			       surface_object->destination_index, 0,
			       surface_object->destination_buffers_count);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_queue_buffer(session->video_fd, request_fd, output_type,
			       &surface_object->timestamp,
			       surface_object->source_index,
			       surface_object->slices_size, 1);
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	surface_object->slices_size = 0;

	status = RequestSyncSurface(context, context_object->render_surface_id);
	if (status != VA_STATUS_SUCCESS)
		return status;

	release_source_buffer(session, surface_object);

	context_object->render_surface_id = VA_INVALID_ID;

	return VA_STATUS_SUCCESS;
}
