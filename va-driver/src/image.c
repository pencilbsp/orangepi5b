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

#include "image.h"
#include "buffer.h"
#include "config.h"
#include "context.h"
#include "encode.h"
#include "request.h"
#include "surface.h"
#include "video.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"
#include "v4l2.h"

static bool image_surface_has_encode_context(struct request_data *driver_data,
					     struct object_surface *surface_object)
{
	struct object_context *context_object;
	int iterator;

	context_object = (struct object_context *)object_heap_first(
		&driver_data->context_heap, &iterator);
	while (context_object != NULL) {
		if (context_object->is_encoder &&
		    context_object->picture_width == surface_object->width &&
		    context_object->picture_height == surface_object->height)
			return true;

		context_object = (struct object_context *)object_heap_next(
			&driver_data->context_heap, &iterator);
	}

	return false;
}

static bool image_have_encode_config(struct request_data *driver_data)
{
	struct object_config *config_object;
	int iterator;

	config_object = (struct object_config *)object_heap_first(
		&driver_data->config_heap, &iterator);
	while (config_object != NULL) {
		if (config_object->entrypoint == VAEntrypointEncSlice)
			return true;

		config_object = (struct object_config *)object_heap_next(
			&driver_data->config_heap, &iterator);
	}

	return false;
}

static bool image_surface_is_encode(struct request_data *driver_data,
				    struct object_surface *surface_object)
{
	if (surface_object->role == SURFACE_ROLE_ENCODE)
		return true;

	if (surface_object->role == SURFACE_ROLE_DECODE)
		return false;

	/*
	 * Nothing has claimed this surface yet -- a client can derive an image
	 * from one before it ever reaches a context, and ffmpeg does -- so
	 * fall back to the old guess. It is only ever consulted here, where
	 * there is genuinely nothing better to go on, and it can no longer
	 * override a surface whose role is known.
	 */
	if (surface_object->encode_data != NULL)
		return true;

	if (image_surface_has_encode_context(driver_data, surface_object))
		return true;

	return surface_object->session == NULL &&
	       driver_data->probe_session.video_format == NULL &&
	       image_have_encode_config(driver_data);
}

static VAStatus alias_encode_surface_to_image(struct request_data *driver_data,
					      struct object_surface *surface_object,
					      VAImage *image)
{
	struct object_buffer *buffer_object;
	struct object_image *image_object;
	VAStatus status;

	status = encode_surface_storage(surface_object);
	if (status != VA_STATUS_SUCCESS)
		return status;

	image->pitches[0] = surface_object->encode_pitch;
	image->pitches[1] = surface_object->encode_pitch;
	image->offsets[0] = 0;
	image->offsets[1] = surface_object->encode_pitch *
			    (unsigned int)surface_object->height;
	image->data_size = surface_object->encode_size;

	buffer_object = BUFFER(driver_data, image->buf);
	if (buffer_object == NULL)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (buffer_object->data != NULL && !buffer_object->data_borrowed)
		free(buffer_object->data);

	buffer_object->data = surface_object->encode_data;
	buffer_object->size = image->data_size;
	buffer_object->initial_count = 1;
	buffer_object->count = 1;
	buffer_object->data_borrowed = true;
	buffer_object->derived_surface_id = surface_object->base.id;

	image_object = IMAGE(driver_data, image->image_id);
	if (image_object != NULL)
		image_object->image = *image;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestCreateImage(VADriverContextP context, VAImageFormat *format,
			    int width, int height, VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_buffer *buffer_object;
	unsigned int pitch;
	unsigned int luma_size;
	unsigned int chroma_size;
	unsigned int size;
	uint64_t aligned_width;
	uint64_t pitch64;
	uint64_t luma_size64;
	uint64_t chroma_size64;
	uint64_t size64;
	struct object_image *image_object;
	VABufferID buffer_id;
	VAImageID id;
	VAStatus status;

	/*
	 * vaCreateImage names no surface, so there is no queue whose geometry
	 * would be the right one to copy. Reading it from "whichever decoder
	 * was asked last" is what makes vaGetImage memcpy past the end of a
	 * mapping once two streams are in flight.
	 *
	 * The image is a CPU-side buffer of our own, so give it a layout of
	 * our own -- tight NV12 at the requested size -- and let vaGetImage
	 * reconcile it with the surface row by row.
	 */
	if (format == NULL || image == NULL || width <= 0 || height <= 0)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	if (format->fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/* NV12 needs room for a complete final UV pair when
	 * width is odd. All arithmetic is widened before entering VA's u32
	 * image fields and RequestCreateBuffer. */
	aligned_width = ((uint64_t)(unsigned int)width + 1) & ~1ull;
	pitch64 = aligned_width;
	luma_size64 = pitch64 * (unsigned int)height;
	chroma_size64 = pitch64 * (((unsigned int)height + 1) / 2);
	size64 = luma_size64 + chroma_size64;
	if (pitch64 > UINT_MAX || luma_size64 > UINT_MAX ||
	    chroma_size64 > UINT_MAX || size64 > UINT_MAX)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	pitch = (unsigned int)pitch64;
	luma_size = (unsigned int)luma_size64;
	chroma_size = (unsigned int)chroma_size64;
	size = (unsigned int)size64;

	id = object_heap_allocate(&driver_data->image_heap);
	image_object = IMAGE(driver_data, id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	status = RequestCreateBuffer(context, 0, VAImageBufferType, size, 1,
				     NULL, &buffer_id);
	if (status != VA_STATUS_SUCCESS) {
		object_heap_free(&driver_data->image_heap,
				 (struct object_base *)image_object);
		return status;
	}
	buffer_object = BUFFER(driver_data, buffer_id);
	if (buffer_object == NULL || buffer_object->data == NULL) {
		RequestDestroyBuffer(context, buffer_id);
		object_heap_free(&driver_data->image_heap,
				 (struct object_base *)image_object);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	memset(buffer_object->data, 0, size);

	memset(image, 0, sizeof(*image));

	image->format = *format;
	image->width = width;
	image->height = height;
	image->buf = buffer_id;
	image->image_id = id;

	image->num_planes = 2;
	image->data_size = size;
	image->pitches[0] = pitch;
	image->pitches[1] = pitch;
	image->offsets[0] = 0;
	image->offsets[1] = luma_size;
	(void)chroma_size;

	image_object->image = *image;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDestroyImage(VADriverContextP context, VAImageID image_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_image *image_object;
	VAStatus status;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	status = RequestDestroyBuffer(context, image_object->image.buf);
	if (status != VA_STATUS_SUCCESS)
		return status;

	object_heap_free(&driver_data->image_heap,
			 (struct object_base *)image_object);

	return VA_STATUS_SUCCESS;
}

static VAStatus copy_surface_to_image (struct request_data *driver_data,
				       struct object_surface *surface_object,
				       VAImage *image)
{
	struct object_buffer *buffer_object;
	unsigned int i;

	buffer_object = BUFFER(driver_data, image->buf);
	if (buffer_object == NULL || buffer_object->data == NULL ||
	    buffer_object->size < image->data_size)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	/*
	 * Probe-pattern clients (mpv --hwdec=vaapi, VLC glconv_vaapi)
	 * call vaDeriveImage on a surface they never decoded into to
	 * inspect the NV12 layout. The surface has no V4L2 CAPTURE
	 * buffer bound yet (destination_planes_count == 0,
	 * destination_data[i] == NULL). Returning the freshly-allocated
	 * VAImage with zero-initialised data is the right answer for a
	 * probe — the format / pitches / offsets carry the information
	 * the client needs, and skipping the memcpy avoids a NULL deref.
	 */
	if (surface_object->destination_planes_count == 0 ||
	    surface_object->destination_data[0] == NULL)
		return VA_STATUS_SUCCESS;

	if (image->format.fourcc != VA_FOURCC_NV12 ||
	    surface_object->pixel_format != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/*
	 * The surface's pitch is whatever the hardware chose; the image's is
	 * whatever RequestCreateImage chose. Copy a row at a time so the two
	 * never have to agree -- a whole-plane memcpy would read or write past
	 * the shorter of them.
	 */
	for (i = 0; i < surface_object->destination_planes_count &&
		    i < image->num_planes; i++) {
		unsigned int src_pitch =
			surface_object->destination_bytesperlines[i];
		unsigned int dst_pitch = image->pitches[i];
		unsigned int pitch = src_pitch < dst_pitch ? src_pitch : dst_pitch;
		unsigned int rows = i == 0 ? image->height :
						     (image->height + 1) / 2;
		unsigned char *src = surface_object->destination_data[i];
		unsigned char *dst = buffer_object->data + image->offsets[i];
		unsigned int row;

		uint64_t source_end;
		uint64_t destination_end;

		if (rows == 0)
			continue;
		source_end = (uint64_t)(rows - 1) * src_pitch + pitch;
		destination_end = (uint64_t)image->offsets[i] +
				  (uint64_t)(rows - 1) * dst_pitch + pitch;
		if (source_end > surface_object->destination_sizes[i] ||
		    destination_end > image->data_size)
			return VA_STATUS_ERROR_OPERATION_FAILED;

		for (row = 0; row < rows; row++)
			memcpy(dst + (size_t)row * dst_pitch,
			       src + (size_t)row * src_pitch, pitch);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDeriveImage(VADriverContextP context, VASurfaceID surface_id,
			    VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_buffer *buffer_object;
	VAImageFormat format;
	VAStatus status;
	bool encode_surface;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (surface_object->status == VASurfaceRendering) {
		status = RequestSyncSurface(context, surface_id);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	encode_surface = image_surface_is_encode(driver_data, surface_object);
	memset(&format, 0, sizeof(format));
	format.fourcc = encode_surface ? VA_FOURCC_NV12 :
				       surface_object->pixel_format;
	format.byte_order = VA_LSB_FIRST;
	format.bits_per_pixel = 12;

	status = RequestCreateImage(context, &format, surface_object->width,
				    surface_object->height, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	if (encode_surface) {
		status = alias_encode_surface_to_image(driver_data,
						       surface_object, image);
		if (status != VA_STATUS_SUCCESS)
			return status;

		surface_object->status = VASurfaceReady;
		return VA_STATUS_SUCCESS;
	}

	status = copy_surface_to_image (driver_data, surface_object, image);
	if (status != VA_STATUS_SUCCESS) {
		RequestDestroyImage(context, image->image_id);
		return status;
	}

	surface_object->status = VASurfaceReady;

	buffer_object = BUFFER(driver_data, image->buf);
	buffer_object->derived_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryImageFormats(VADriverContextP context,
				  VAImageFormat *formats, int *formats_count)
{
	if (formats_count == NULL)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (formats == NULL) {
		*formats_count = 1;
		return VA_STATUS_SUCCESS;
	}

	memset(formats, 0, sizeof(*formats));
	formats[0].fourcc = VA_FOURCC_NV12;
	formats[0].byte_order = VA_LSB_FIRST;
	formats[0].bits_per_pixel = 12;
	*formats_count = 1;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestSetImagePalette(VADriverContextP context, VAImageID image_id,
				unsigned char *palette)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestGetImage(VADriverContextP context, VASurfaceID surface_id,
			 int x, int y, unsigned int width, unsigned int height,
			 VAImageID image_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	struct object_image *image_object;
	VAImage *image;
	VAStatus status;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;
	if (x != 0 || y != 0 || width != image->width || height != image->height ||
	    width != (unsigned int)surface_object->width ||
	    height != (unsigned int)surface_object->height)
		return VA_STATUS_ERROR_UNIMPLEMENTED;
	if (surface_object->status == VASurfaceRendering) {
		status = RequestSyncSurface(context, surface_id);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	return copy_surface_to_image (driver_data, surface_object, image);
}

VAStatus RequestPutImage(VADriverContextP context, VASurfaceID surface_id,
			 VAImageID image, int src_x, int src_y,
			 unsigned int src_width, unsigned int src_height,
			 int dst_x, int dst_y, unsigned int dst_width,
			 unsigned int dst_height)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
