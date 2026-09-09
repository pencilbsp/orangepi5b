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

#include "request.h"
#include "surface.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va_drmcommon.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "config.h"
#include "context.h"
#include "media.h"
#include "picture.h"
#include "utils.h"
#include "v4l2.h"
#include "video.h"

static struct decoder_session *
find_unique_context_session(struct request_data *driver_data,
			    unsigned int width, unsigned int height)
{
	struct decoder_session *session = NULL;
	struct object_context *context_object;
	int iterator;

	context_object = (struct object_context *)object_heap_first(
		&driver_data->context_heap, &iterator);
	while (context_object != NULL) {
		if (context_object->session.video_format != NULL &&
		    context_object->picture_width == (int)width &&
		    context_object->picture_height == (int)height) {
			if (session != NULL)
				return NULL;

			session = &context_object->session;
		}

		context_object = (struct object_context *)object_heap_next(
			&driver_data->context_heap, &iterator);
	}

	return session;
}

/*
 * Allocate VA surface objects without performing any V4L2 work.
 *
 * On rk3588 rkvdec the OUTPUT pixel format depends on the codec
 * (chosen at vaCreateConfig + vaCreateContext); setting CAPTURE first
 * causes rkvdec to reject the subsequent S_FMT(OUTPUT) with EBUSY
 * because REQBUFS has already been issued. We therefore defer ALL
 * V4L2 setup (S_FMT, REQBUFS, MMAP, STREAMON) to RequestCreateContext
 * once the codec is known. Each surface is bound to OUTPUT/CAPTURE
 * pool slots lazily in RequestBeginPicture — that also covers
 * FFmpeg's habit of creating extra surfaces after the context.
 */
VAStatus RequestCreateSurfaces2(VADriverContextP context, unsigned int format,
				unsigned int width, unsigned int height,
				VASurfaceID *surfaces_ids,
				unsigned int surfaces_count,
				VASurfaceAttrib *attributes,
				unsigned int attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	unsigned int i;
	VASurfaceID id;

	(void)attributes;
	(void)attributes_count;

	if (format != VA_RT_FORMAT_YUV420)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	for (i = 0; i < surfaces_count; i++) {
		id = object_heap_allocate(&driver_data->surface_heap);
		surface_object = SURFACE(driver_data, id);
		if (surface_object == NULL) {
			/*
			 * The call fails, so the client will not destroy the
			 * ids written so far -- it never learns they exist.
			 * Give them back here or they are leaked for the life
			 * of the display.
			 */
			RequestDestroySurfaces(context, surfaces_ids, (int)i);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}

		/*
		 * object_heap hands out memory from a malloc'd bucket that it
		 * never clears, so a recycled slot still holds the previous
		 * surface's bytes -- and a field left out here is read back as
		 * whatever that surface wrote. The pages of a fresh bucket
		 * come from the kernel already zeroed, which is why such a
		 * field survives every short-lived client and only fails under
		 * one that runs long enough to recycle a slot: a stale
		 * ->session cost Chrome a SIGSEGV in the GPU process.
		 *
		 * Clearing the body once removes the whole class. Only the
		 * heap's own bookkeeping is off limits; the fields whose
		 * resting value is not zero are set right below.
		 */
		memset((char *)surface_object + sizeof(surface_object->base), 0,
		       sizeof(*surface_object) - sizeof(surface_object->base));

		surface_object->status = VASurfaceReady;
		surface_object->width = width;
		surface_object->height = height;
		surface_object->source_index = SURFACE_INDEX_UNASSIGNED;
		surface_object->destination_index = SURFACE_INDEX_UNASSIGNED;
		surface_object->request_fd = -1;

		surfaces_ids[i] = id;
	}

	/*
	 * Eager V4L2 init for probe-pattern clients (mpv --hwdec=vaapi,
	 * VLC glconv_vaapi) that touch vaDeriveImage / vaExportSurfaceHandle
	 * BEFORE vaCreateContext -- and sometimes (VLC) BEFORE vaCreateConfig.
	 * They want the NV12 layout, which needs the CAPTURE queue programmed.
	 *
	 * Take the codec from the first config in the heap when one exists.
	 * With no config at all there is nothing to derive it from, so this
	 * picks H.264 to get a pool of the right shape. Guessing is safe here:
	 * a guess only decides the OUTPUT pixelformat, and
	 * request_ensure_v4l2_initialized() reprograms the queues when the
	 * real context asks for a different codec or resolution.
	 */
	if (driver_data->probe_session.video_format == NULL &&
	    find_unique_context_session(driver_data, width, height) == NULL) {
		VAProfile probe_profile;
		int it;
		struct object_config *cfg =
			(struct object_config *)object_heap_first(
				&driver_data->config_heap, &it);

		probe_profile = cfg != NULL ? cfg->profile : VAProfileH264High;

		if (decoder_session_open(driver_data,
					 &driver_data->probe_session) == 0)
			(void)request_ensure_v4l2_initialized(
				&driver_data->probe_session, probe_profile,
				(int)width, (int)height);
	}

	/*
	 * Intentionally NOT binding surfaces to CAPTURE pool slots here.
	 * Pre-binding mmaps the kernel CAPTURE buffer before any decode
	 * happens. VLC's display path then reads that uninitialised
	 * buffer via vaDeriveImage and renders a solid green frame
	 * (NV12 all-zero luma/chroma). The legacy lazy bind in
	 * RequestBeginPicture happens right before the decode submission,
	 * so the CAPTURE buffer always contains real data by the time the
	 * display path reads it. The eager V4L2 init above is enough to
	 * unblock probe-pattern clients that only need video_format set
	 * (vaDeriveImage / vaExportSurfaceHandle no longer fail-fast on
	 * NULL video_format).
	 */

	return VA_STATUS_SUCCESS;
}

VAStatus RequestCreateSurfaces(VADriverContextP context, int width, int height,
			       int format, int surfaces_count,
			       VASurfaceID *surfaces_ids)
{
	return RequestCreateSurfaces2(context, format, width, height,
				      surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus RequestDestroySurfaces(VADriverContextP context,
				VASurfaceID *surfaces_ids, int surfaces_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	unsigned int j;
	int i;

	/*
	 * surfaces_count is signed in the VA ABI. Counting with an unsigned i
	 * would promote a negative count to a huge one and walk off the array;
	 * a signed counter just runs zero times.
	 */
	for (i = 0; i < surfaces_count; i++) {
		surface_object = SURFACE(driver_data, surfaces_ids[i]);
		if (surface_object == NULL)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		if (surface_object->source_data != NULL &&
		    surface_object->source_data != MAP_FAILED &&
		    surface_object->source_size > 0)
			munmap(surface_object->source_data,
			       surface_object->source_size);

		for (j = 0; j < surface_object->destination_buffers_count; j++)
			if (surface_object->destination_map[j] != NULL &&
			    surface_object->destination_map[j] != MAP_FAILED &&
			    surface_object->destination_map_lengths[j] > 0)
				munmap(surface_object->destination_map[j],
				       surface_object->destination_map_lengths[j]);

		if (surface_object->request_fd >= 0)
			close(surface_object->request_fd);

		object_heap_free(&driver_data->surface_heap,
				 (struct object_base *)surface_object);
	}

	return VA_STATUS_SUCCESS;
}

/*
 * Cut every surface loose from a session that is about to disappear.
 *
 * A context destroys the surfaces it was created with, but Chrome creates its
 * contexts with no render-target list at all, so those surfaces are not on
 * that list and outlive the context. Their session pointer would then name an
 * object_context slot that the heap has put back on its free list and will
 * hand to the next vaCreateContext -- the surface would quietly start using
 * another stream's file handles.
 *
 * The CAPTURE buffers went away with the session's handles, so the surface is
 * also marked unbound: whatever session picks it up next has to bind it again
 * rather than reuse a buffer index that means nothing there.
 */
void surface_detach_session(struct request_data *driver_data,
			    struct decoder_session *session)
{
	struct object_surface *surface_object;
	int iterator;
	unsigned int i;

	surface_object = (struct object_surface *)object_heap_first(
		&driver_data->surface_heap, &iterator);
	while (surface_object != NULL) {
		if (surface_object->session == session) {
			for (i = 0; i < surface_object->destination_buffers_count; i++)
				if (surface_object->destination_map[i] != NULL &&
				    surface_object->destination_map[i] != MAP_FAILED &&
				    surface_object->destination_map_lengths[i] > 0)
					munmap(surface_object->destination_map[i],
					       surface_object->destination_map_lengths[i]);

			memset(surface_object->destination_map, 0,
			       sizeof(surface_object->destination_map));
			memset(surface_object->destination_map_lengths, 0,
			       sizeof(surface_object->destination_map_lengths));

			surface_object->destination_index = SURFACE_INDEX_UNASSIGNED;
			surface_object->destination_buffers_count = 0;
			surface_object->destination_planes_count = 0;
			surface_object->source_index = SURFACE_INDEX_UNASSIGNED;
			surface_object->session = NULL;
		}

		surface_object = (struct object_surface *)object_heap_next(
			&driver_data->surface_heap, &iterator);
	}
}

VAStatus RequestSyncSurface(VADriverContextP context, VASurfaceID surface_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct decoder_session *session;
	VAStatus status;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	int request_fd = -1;
	int rc;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL) {
		status = VA_STATUS_ERROR_INVALID_SURFACE;
		goto error;
	}

	if (surface_object->status != VASurfaceRendering) {
		status = VA_STATUS_SUCCESS;
		goto complete;
	}

	/*
	 * The buffers were allocated on the session that bound this surface,
	 * so they can only be dequeued there. A never-rendered Ready surface
	 * legitimately has no session, and was handled above.
	 */
	session = surface_object->session;
	if (session == NULL || session->video_format == NULL) {
		request_log("sync: rendering surface 0x%x has no bound decode session\n",
			    surface_id);
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	video_format = session->video_format;
	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	request_fd = surface_object->request_fd;
	if (request_fd < 0) {
		request_log("sync: surface 0x%x is rendering without a request fd\n",
			    surface_id);
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = media_request_queue(request_fd);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * Nearly all of a decode's wall time is spent here. Holding the driver
	 * lock across it would serialise decoding itself rather than just the
	 * bookkeeping around it, and two streams would take turns instead of
	 * running together.
	 *
	 * The surface cannot go away underneath this: a client that is waiting
	 * on a surface is not also destroying it.
	 */
	pthread_mutex_unlock(&driver_data->lock);
	rc = media_request_wait_completion(request_fd);
	pthread_mutex_lock(&driver_data->lock);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = media_request_reinit(request_fd);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = v4l2_dequeue_buffer(session->video_fd, -1, output_type,
				 surface_object->source_index, 1);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = v4l2_dequeue_buffer(session->video_fd, -1, capture_type,
				 surface_object->destination_index,
				 surface_object->destination_buffers_count);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	surface_object->status = VASurfaceDisplaying;

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	if (request_fd >= 0) {
		close(request_fd);
		surface_object->request_fd = -1;
	}

complete:
	return status;
}

/*
 * The coded format a config decodes decides the frame sizes the device will
 * accept, so the limits are per config rather than per driver.
 */
static int config_frame_limits(struct request_data *driver_data,
			       VAConfigID config_id,
			       struct v4l2_frame_limits *limits)
{
	struct object_config *config_object;
	unsigned int pixelformat;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return -1;

	if (profile_to_pixelformat(config_object->profile, &pixelformat) < 0)
		return -1;

	return v4l2_get_frame_sizes(driver_data->video_fd, pixelformat, limits);
}

VAStatus RequestQuerySurfaceAttributes(VADriverContextP context,
				       VAConfigID config,
				       VASurfaceAttrib *attributes,
				       unsigned int *attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	VASurfaceAttrib *attributes_list;
	unsigned int attributes_list_size = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES *
					    sizeof(*attributes);
	struct v4l2_frame_limits limits;
	int memory_types;
	unsigned int i = 0;

	attributes_list = malloc(attributes_list_size);
	if (attributes_list == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	memset(attributes_list, 0, attributes_list_size);

	attributes_list[i].type = VASurfaceAttribPixelFormat;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = VA_FOURCC_NV12;
	i++;

	/*
	 * Report what the device accepts, not a constant. rkvdec decodes well
	 * past 4K -- the previous hardcoded 3840x2160 turned a hardware limit
	 * into a driver limit -- and a constant would be wrong the other way
	 * on a device that does less.
	 */
	if (config_frame_limits(driver_data, config, &limits) == 0) {
		attributes_list[i].type = VASurfaceAttribMinWidth;
		attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
		attributes_list[i].value.type = VAGenericValueTypeInteger;
		attributes_list[i].value.value.i = limits.min_width;
		i++;

		attributes_list[i].type = VASurfaceAttribMaxWidth;
		attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
		attributes_list[i].value.type = VAGenericValueTypeInteger;
		attributes_list[i].value.value.i = limits.max_width;
		i++;

		attributes_list[i].type = VASurfaceAttribMinHeight;
		attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
		attributes_list[i].value.type = VAGenericValueTypeInteger;
		attributes_list[i].value.value.i = limits.min_height;
		i++;

		attributes_list[i].type = VASurfaceAttribMaxHeight;
		attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
		attributes_list[i].value.type = VAGenericValueTypeInteger;
		attributes_list[i].value.value.i = limits.max_height;
		i++;
	}

	attributes_list[i].type = VASurfaceAttribMemoryType;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE |
				   VA_SURFACE_ATTRIB_SETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;

	memory_types = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
		VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

	/*
	 * First version of DRM prime export does not handle modifiers,
	 * that are required for supporting the tiled output format.
	 */

	if (video_format_is_linear(driver_data->probe_session.video_format))
		memory_types |= VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

	attributes_list[i].value.value.i = memory_types;
	i++;

	attributes_list_size = i * sizeof(*attributes);

	if (attributes != NULL)
		memcpy(attributes, attributes_list, attributes_list_size);

	free(attributes_list);

	*attributes_count = i;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQuerySurfaceStatus(VADriverContextP context,
				   VASurfaceID surface_id,
				   VASurfaceStatus *status)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	*status = surface_object->status;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestPutSurface(VADriverContextP context, VASurfaceID surface_id,
			   void *draw, short src_x, short src_y,
			   unsigned short src_width, unsigned short src_height,
			   short dst_x, short dst_y, unsigned short dst_width,
			   unsigned short dst_height, VARectangle *cliprects,
			   unsigned int cliprects_count, unsigned int flags)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestLockSurface(VADriverContextP context, VASurfaceID surface_id,
			    unsigned int *fourcc, unsigned int *luma_stride,
			    unsigned int *chroma_u_stride,
			    unsigned int *chroma_v_stride,
			    unsigned int *luma_offset,
			    unsigned int *chroma_u_offset,
			    unsigned int *chroma_v_offset,
			    unsigned int *buffer_name, void **buffer)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestUnlockSurface(VADriverContextP context, VASurfaceID surface_id)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestExportSurfaceHandle(VADriverContextP context,
				    VASurfaceID surface_id, uint32_t mem_type,
				    uint32_t flags, void *descriptor)
{
	struct request_data *driver_data = context->pDriverData;
	VADRMPRIMESurfaceDescriptor *surface_descriptor = descriptor;
	struct object_surface *surface_object;
	struct decoder_session *session;
	struct video_format *video_format;
	int *export_fds = NULL;
	unsigned int export_fds_count;
	unsigned int planes_count;
	unsigned int capture_type;
	unsigned int size;
	unsigned int i;
	VAStatus status;
	int rc;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	/*
	 * A surface exported before any context exists -- Chrome does this --
	 * has no session of its own yet, so it borrows the probe session and
	 * keeps it.
	 */
	session = surface_object->session;
	if (session == NULL)
		session = find_unique_context_session(driver_data,
						      surface_object->width,
						      surface_object->height);
	if (session == NULL)
		session = &driver_data->probe_session;
	if (session->video_format == NULL)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	video_format = session->video_format;

	/*
	 * Bind a CAPTURE buffer if this surface has none yet.
	 *
	 * Chrome allocates every frame through the driver: it calls
	 * vaCreateSurfaces and then vaExportSurfaceHandle immediately, before
	 * a single picture has been decoded, and decodes into that same
	 * surface afterwards. Refusing an unbound surface makes Chrome give up
	 * with "video decoder fallback after initial decode error" -- reported
	 * to MediaLog, so nothing appears on stderr even at --vmodule=*=4.
	 *
	 * Binding here is what makes the exported dmabuf point at the buffer
	 * the decode will land in. A client that exports a surface and then
	 * never decodes into it sees uninitialised contents; that is the
	 * client asking for the memory before there is anything in it.
	 */
	if (surface_object->destination_index == SURFACE_INDEX_UNASSIGNED &&
	    request_bind_destination_buffer(session, surface_object) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	export_fds_count = surface_object->destination_buffers_count;
	export_fds = malloc(export_fds_count * sizeof(*export_fds));
	if (export_fds == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_export_buffer(session->video_fd, capture_type,
				surface_object->destination_index, O_RDONLY,
				export_fds, export_fds_count);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	planes_count = surface_object->destination_planes_count;

	surface_descriptor->fourcc = VA_FOURCC_NV12;
	surface_descriptor->width = surface_object->width;
	surface_descriptor->height = surface_object->height;
	surface_descriptor->num_objects = export_fds_count;

	size = 0;

	if (export_fds_count == 1)
		for (i = 0; i < planes_count; i++)
			size += surface_object->destination_sizes[i];

	for (i = 0; i < export_fds_count; i++) {
		surface_descriptor->objects[i].drm_format_modifier =
			video_format->drm_modifier;
		surface_descriptor->objects[i].fd = export_fds[i];
		surface_descriptor->objects[i].size = export_fds_count == 1 ?
						      size :
						      surface_object->destination_sizes[i];
	}

	/*
	 * A client asks for one layer per plane or for everything composed
	 * into one, and it then reads the descriptor the way it asked. Chrome
	 * asks for SEPARATE_LAYERS and reads one plane per layer, so always
	 * answering with a composed descriptor hands it a plane count it does
	 * not expect.
	 */
	if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
		surface_descriptor->num_layers = planes_count;

		for (i = 0; i < planes_count; i++) {
			/*
			 * Each NV12 plane is its own image: luma is one byte
			 * per sample, chroma is an interleaved Cb/Cr pair.
			 */
			surface_descriptor->layers[i].drm_format =
				i == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
			surface_descriptor->layers[i].num_planes = 1;
			surface_descriptor->layers[i].object_index[0] =
				export_fds_count == 1 ? 0 : i;
			surface_descriptor->layers[i].offset[0] =
				surface_object->destination_offsets[i];
			surface_descriptor->layers[i].pitch[0] =
				surface_object->destination_bytesperlines[i];
		}
	} else {
		surface_descriptor->num_layers = 1;

		surface_descriptor->layers[0].drm_format =
			video_format->drm_format;
		surface_descriptor->layers[0].num_planes = planes_count;

		for (i = 0; i < planes_count; i++) {
			surface_descriptor->layers[0].object_index[i] =
				export_fds_count == 1 ? 0 : i;
			surface_descriptor->layers[0].offset[i] =
				surface_object->destination_offsets[i];
			surface_descriptor->layers[0].pitch[i] =
				surface_object->destination_bytesperlines[i];
		}
	}

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	for (i = 0; i < export_fds_count; i++)
		if (export_fds[i] >= 0)
			close(export_fds[i]);

complete:
	if (export_fds != NULL)
		free(export_fds);

	return status;
}
