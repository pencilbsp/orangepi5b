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

#include "buffer.h"
#include "device.h"
#include "sync.h"
#include "config.h"
#include "context.h"
#include "image.h"
#include "picture.h"
#include "subpicture.h"
#include "surface.h"

#include "autoconfig.h"

#include <va/va_backend.h>

#include "request.h"
#include "utils.h"
#include "v4l2.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>

/* Set default visibility for the init function only. */
VAStatus __attribute__((visibility("default")))
VA_DRIVER_INIT_FUNC(VADriverContextP context);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP context)
{
	struct request_data *driver_data;
	struct VADriverVTable *vtable = context->vtable;
	VAStatus status;
	unsigned int capabilities;
	unsigned int capabilities_required;
	int video_fd = -1;
	int media_fd = -1;
	int rc;

	context->version_major = VA_MAJOR_VERSION;
	context->version_minor = VA_MINOR_VERSION;
	context->max_profiles = V4L2_REQUEST_MAX_PROFILES;
	context->max_entrypoints = V4L2_REQUEST_MAX_ENTRYPOINTS;
	context->max_attributes = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;
	context->max_image_formats = V4L2_REQUEST_MAX_IMAGE_FORMATS;
	context->max_subpic_formats = V4L2_REQUEST_MAX_SUBPIC_FORMATS;
	context->max_display_attributes = V4L2_REQUEST_MAX_DISPLAY_ATTRIBUTES;
	context->str_vendor = V4L2_REQUEST_STR_VENDOR;

	vtable->vaTerminate = RequestTerminate;
	vtable->vaQueryConfigEntrypoints = SyncQueryConfigEntrypoints;
	vtable->vaQueryConfigProfiles = SyncQueryConfigProfiles;
	vtable->vaQueryConfigEntrypoints = SyncQueryConfigEntrypoints;
	vtable->vaQueryConfigAttributes = SyncQueryConfigAttributes;
	vtable->vaCreateConfig = SyncCreateConfig;
	vtable->vaDestroyConfig = SyncDestroyConfig;
	vtable->vaGetConfigAttributes = SyncGetConfigAttributes;
	vtable->vaCreateSurfaces = SyncCreateSurfaces;
	vtable->vaCreateSurfaces2 = SyncCreateSurfaces2;
	vtable->vaDestroySurfaces = SyncDestroySurfaces;
	vtable->vaExportSurfaceHandle = SyncExportSurfaceHandle;
	vtable->vaCreateContext = SyncCreateContext;
	vtable->vaDestroyContext = SyncDestroyContext;
	vtable->vaCreateBuffer = SyncCreateBuffer;
	vtable->vaBufferSetNumElements = SyncBufferSetNumElements;
	vtable->vaMapBuffer = SyncMapBuffer;
	vtable->vaUnmapBuffer = SyncUnmapBuffer;
	vtable->vaDestroyBuffer = SyncDestroyBuffer;
	vtable->vaBufferInfo = SyncBufferInfo;
	vtable->vaAcquireBufferHandle = SyncAcquireBufferHandle;
	vtable->vaReleaseBufferHandle = SyncReleaseBufferHandle;
	vtable->vaBeginPicture = SyncBeginPicture;
	vtable->vaRenderPicture = SyncRenderPicture;
	vtable->vaEndPicture = SyncEndPicture;
	vtable->vaSyncSurface = SyncSyncSurface;
	vtable->vaQuerySurfaceAttributes = SyncQuerySurfaceAttributes;
	vtable->vaQuerySurfaceStatus = SyncQuerySurfaceStatus;
	vtable->vaPutSurface = SyncPutSurface;
	vtable->vaQueryImageFormats = SyncQueryImageFormats;
	vtable->vaCreateImage = SyncCreateImage;
	vtable->vaDeriveImage = SyncDeriveImage;
	vtable->vaDestroyImage = SyncDestroyImage;
	vtable->vaSetImagePalette = SyncSetImagePalette;
	vtable->vaGetImage = SyncGetImage;
	vtable->vaPutImage = SyncPutImage;
	vtable->vaQuerySubpictureFormats = SyncQuerySubpictureFormats;
	vtable->vaCreateSubpicture = SyncCreateSubpicture;
	vtable->vaDestroySubpicture = SyncDestroySubpicture;
	vtable->vaSetSubpictureImage = SyncSetSubpictureImage;
	vtable->vaSetSubpictureChromakey = SyncSetSubpictureChromakey;
	vtable->vaSetSubpictureGlobalAlpha = SyncSetSubpictureGlobalAlpha;
	vtable->vaAssociateSubpicture = SyncAssociateSubpicture;
	vtable->vaDeassociateSubpicture = SyncDeassociateSubpicture;
	vtable->vaQueryDisplayAttributes = SyncQueryDisplayAttributes;
	vtable->vaGetDisplayAttributes = SyncGetDisplayAttributes;
	vtable->vaSetDisplayAttributes = SyncSetDisplayAttributes;
	vtable->vaLockSurface = SyncLockSurface;
	vtable->vaUnlockSurface = SyncUnlockSurface;

	driver_data = malloc(sizeof(*driver_data));
	if (driver_data == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	memset(driver_data, 0, sizeof(*driver_data));

	context->pDriverData = driver_data;

	pthread_mutex_init(&driver_data->lock, NULL);

	/*
	 * driver_data is zeroed above, so an unopened session would read
	 * fd 0 -- stdin -- as a live handle. Every decoder_session field
	 * that means "no handle" has to say -1 explicitly.
	 */
	driver_data->probe_session.video_fd = -1;
	driver_data->probe_session.media_fd = -1;

	object_heap_init(&driver_data->config_heap,
			 sizeof(struct object_config), CONFIG_ID_OFFSET);
	object_heap_init(&driver_data->context_heap,
			 sizeof(struct object_context), CONTEXT_ID_OFFSET);
	object_heap_init(&driver_data->surface_heap,
			 sizeof(struct object_surface), SURFACE_ID_OFFSET);
	object_heap_init(&driver_data->buffer_heap,
			 sizeof(struct object_buffer), BUFFER_ID_OFFSET);
	object_heap_init(&driver_data->image_heap, sizeof(struct object_image),
			 IMAGE_ID_OFFSET);

	if (decoder_device_open(&video_fd, &media_fd,
				driver_data->video_path,
				driver_data->media_path,
				sizeof(driver_data->video_path)) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rc = v4l2_query_capabilities(video_fd, &capabilities);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	capabilities_required = V4L2_CAP_STREAMING;

	if ((capabilities & capabilities_required) != capabilities_required) {
		request_log("Missing required driver capabilities\n");
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	driver_data->video_fd = video_fd;
	driver_data->media_fd = media_fd;

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	status = VA_STATUS_ERROR_OPERATION_FAILED;

	if (video_fd >= 0)
		close(video_fd);

	if (media_fd >= 0)
		close(media_fd);

complete:
	return status;
}

VAStatus RequestTerminate(VADriverContextP context)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_buffer *buffer_object;
	struct object_image *image_object;
	struct object_surface *surface_object;
	struct object_context *context_object;
	struct object_config *config_object;
	int iterator;

	close(driver_data->video_fd);
	close(driver_data->media_fd);

	/*
	 * A probe session opened for a client that exported before any
	 * context existed is owned by nobody else, so it is released
	 * here or its two handles live as long as the VADisplay.
	 */
	decoder_session_close(&driver_data->probe_session);

	/* Cleanup leftover buffers. */

	image_object = (struct object_image *)
		object_heap_first(&driver_data->image_heap, &iterator);
	while (image_object != NULL) {
		RequestDestroyImage(context, (VAImageID)image_object->base.id);
		image_object = (struct object_image *)
			object_heap_next(&driver_data->image_heap, &iterator);
	}

	object_heap_destroy(&driver_data->image_heap);

	buffer_object = (struct object_buffer *)
		object_heap_first(&driver_data->buffer_heap, &iterator);
	while (buffer_object != NULL) {
		RequestDestroyBuffer(context,
				     (VABufferID)buffer_object->base.id);
		buffer_object = (struct object_buffer *)
			object_heap_next(&driver_data->buffer_heap, &iterator);
	}

	object_heap_destroy(&driver_data->buffer_heap);

	surface_object = (struct object_surface *)
		object_heap_first(&driver_data->surface_heap, &iterator);
	while (surface_object != NULL) {
		RequestDestroySurfaces(context,
				      (VASurfaceID *)&surface_object->base.id, 1);
		surface_object = (struct object_surface *)
			object_heap_next(&driver_data->surface_heap, &iterator);
	}

	object_heap_destroy(&driver_data->surface_heap);

	context_object = (struct object_context *)
		object_heap_first(&driver_data->context_heap, &iterator);
	while (context_object != NULL) {
		RequestDestroyContext(context,
				      (VAContextID)context_object->base.id);
		context_object = (struct object_context *)
			object_heap_next(&driver_data->context_heap, &iterator);
	}

	object_heap_destroy(&driver_data->context_heap);

	config_object = (struct object_config *)
		object_heap_first(&driver_data->config_heap, &iterator);
	while (config_object != NULL) {
		RequestDestroyConfig(context,
				     (VAConfigID)config_object->base.id);
		config_object = (struct object_config *)
			object_heap_next(&driver_data->config_heap, &iterator);
	}

	object_heap_destroy(&driver_data->config_heap);

	free(context->pDriverData);
	context->pDriverData = NULL;

	return VA_STATUS_SUCCESS;
}
