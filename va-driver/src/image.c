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
#include "request.h"
#include "surface.h"
#include "video.h"

#include <assert.h>
#include <string.h>

#include "utils.h"
#include "v4l2.h"

VAStatus RequestCreateImage(VADriverContextP context, VAImageFormat *format,
			    int width, int height, VAImage *image)
{
	struct request_data *driver_data = context->pDriverData;
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
	unsigned int destination_planes_count;
	unsigned int size;
	struct object_image *image_object;
	VABufferID buffer_id;
	VAImageID id;
	VAStatus status;
	unsigned int i;

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
	if (format->fourcc != VA_FOURCC_NV12)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	destination_planes_count = 2;

	destination_bytesperlines[0] = width;
	destination_sizes[0] = destination_bytesperlines[0] * height;
	destination_bytesperlines[1] = width;
	destination_sizes[1] = destination_bytesperlines[1] * (height / 2);

	size = destination_sizes[0] + destination_sizes[1];

	/* Here we calculate the sizes assuming NV12. */

	destination_sizes[0] = destination_bytesperlines[0] * height;

	for (i = 1; i < destination_planes_count; i++) {
		destination_bytesperlines[i] = destination_bytesperlines[0];
		destination_sizes[i] = destination_sizes[0] / 2;
	}

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

	memset(image, 0, sizeof(*image));

	image->format = *format;
	image->width = width;
	image->height = height;
	image->buf = buffer_id;
	image->image_id = id;

	image->num_planes = destination_planes_count;
	image->data_size = size;

	for (i = 0; i < image->num_planes; i++) {
		image->pitches[i] = destination_bytesperlines[i];
		image->offsets[i] = i > 0 ? destination_sizes[i - 1] : 0;
	}

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
	if (buffer_object == NULL)
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
		unsigned int rows = i == 0 ? image->height : image->height / 2;
		unsigned char *src = surface_object->destination_data[i];
		unsigned char *dst = buffer_object->data + image->offsets[i];
		unsigned int row;

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

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (surface_object->status == VASurfaceRendering) {
		status = RequestSyncSurface(context, surface_id);
		if (status != VA_STATUS_SUCCESS)
			return status;
	}

	format.fourcc = VA_FOURCC_NV12;

	status = RequestCreateImage(context, &format, surface_object->width,
				    surface_object->height, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = copy_surface_to_image (driver_data, surface_object, image);
	if (status != VA_STATUS_SUCCESS)
		return status;

	surface_object->status = VASurfaceReady;

	buffer_object = BUFFER(driver_data, image->buf);
	buffer_object->derived_surface_id = surface_id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryImageFormats(VADriverContextP context,
				  VAImageFormat *formats, int *formats_count)
{
	formats[0].fourcc = VA_FOURCC_NV12;
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

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	image_object = IMAGE(driver_data, image_id);
	if (image_object == NULL)
		return VA_STATUS_ERROR_INVALID_IMAGE;

	image = &image_object->image;
	if (x != 0 || y != 0 || width != image->width || height != image->height)
		return VA_STATUS_ERROR_UNIMPLEMENTED;

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
