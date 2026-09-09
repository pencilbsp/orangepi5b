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

#include "config.h"
#include "request.h"

#include <assert.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>


#include "context.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

VAStatus RequestCreateConfig(VADriverContextP context, VAProfile profile,
			     VAEntrypoint entrypoint,
			     VAConfigAttrib *attributes, int attributes_count,
			     VAConfigID *config_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	VAConfigID id;
	int i, index;

	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileHEVCMain:
		if (entrypoint != VAEntrypointVLD)
			return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
		break;

	default:
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}

	if (attributes_count > V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES)
		attributes_count = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES;

	id = object_heap_allocate(&driver_data->config_heap);
	config_object = CONFIG(driver_data, id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	config_object->profile = profile;
	config_object->entrypoint = entrypoint;
	config_object->attributes[0].type = VAConfigAttribRTFormat;
	config_object->attributes[0].value = VA_RT_FORMAT_YUV420;
	config_object->attributes_count = 1;

	for (i = 1; i < attributes_count; i++) {
		index = config_object->attributes_count++;
		config_object->attributes[index].type = attributes[index].type;
		config_object->attributes[index].value =
			attributes[index].value;
	}

	*config_id = id;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestDestroyConfig(VADriverContextP context, VAConfigID config_id)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	object_heap_free(&driver_data->config_heap,
			 (struct object_base *)config_object);

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigProfiles(VADriverContextP context,
				    VAProfile *profiles, int *profiles_count)
{
	struct request_data *driver_data = context->pDriverData;
	unsigned int index = 0;
	bool found;

	/*
	 * Advertise only what this driver can actually decode. Claiming a
	 * profile it cannot deliver is worse than silence: the client commits
	 * to the hardware path, gets a config and a context, and only finds
	 * out mid-stream.
	 *
	 * Deliberately absent:
	 *   MPEG-2            - no backend here
	 *   VP9, AV1, VP8     - out of scope; AV1 lives on a different block
	 *   H264 Multiview /
	 *   Stereo High       - rkvdec decodes a single view
	 *   HEVC Main 10      - the hardware manages it, but rkvdec only emits
	 *                       NV15 and libva has no FOURCC for that layout,
	 *                       so there is nothing to hand a client
	 */
	found = v4l2_find_format_any(driver_data->video_fd,
				     V4L2_PIX_FMT_H264_SLICE);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 3)) {
		profiles[index++] = VAProfileH264ConstrainedBaseline;
		profiles[index++] = VAProfileH264Main;
		profiles[index++] = VAProfileH264High;
	}

	found = v4l2_find_format_any(driver_data->video_fd,
				     V4L2_PIX_FMT_HEVC_SLICE);
	if (found && index < (V4L2_REQUEST_MAX_PROFILES - 1))
		profiles[index++] = VAProfileHEVCMain;

	*profiles_count = index;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigEntrypoints(VADriverContextP context,
				       VAProfile profile,
				       VAEntrypoint *entrypoints,
				       int *entrypoints_count)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
	case VAProfileHEVCMain:
		entrypoints[0] = VAEntrypointVLD;
		*entrypoints_count = 1;
		break;

	default:
		*entrypoints_count = 0;
		break;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigAttributes(VADriverContextP context,
				      VAConfigID config_id, VAProfile *profile,
				      VAEntrypoint *entrypoint,
				      VAConfigAttrib *attributes,
				      int *attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_config *config_object;
	int i;

	config_object = CONFIG(driver_data, config_id);
	if (config_object == NULL)
		return VA_STATUS_ERROR_INVALID_CONFIG;

	if (profile != NULL)
		*profile = config_object->profile;

	if (entrypoint != NULL)
		*entrypoint = config_object->entrypoint;

	if (attributes_count != NULL)
		*attributes_count = config_object->attributes_count;

	/* Attributes might be NULL to retrieve the associated count. */
	if (attributes != NULL)
		for (i = 0; i < config_object->attributes_count; i++)
			attributes[i] = config_object->attributes[i];

	return VA_STATUS_SUCCESS;
}

VAStatus RequestGetConfigAttributes(VADriverContextP context, VAProfile profile,
				    VAEntrypoint entrypoint,
				    VAConfigAttrib *attributes,
				    int attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct v4l2_frame_limits limits;
	unsigned int pixelformat;
	bool have_limits = false;
	int i;

	(void)entrypoint;

	if (profile_to_pixelformat(profile, &pixelformat) == 0)
		have_limits = v4l2_get_frame_sizes(driver_data->video_fd,
						   pixelformat, &limits) == 0;

	for (i = 0; i < attributes_count; i++) {
		switch (attributes[i].type) {
		case VAConfigAttribRTFormat:
			attributes[i].value = VA_RT_FORMAT_YUV420;
			break;

		/*
		 * Answer with what the device accepts. A client that asks and
		 * is told nothing has to guess, and the usual guess is 4K.
		 */
		case VAConfigAttribMaxPictureWidth:
			attributes[i].value = have_limits ? limits.max_width :
							    VA_ATTRIB_NOT_SUPPORTED;
			break;
		case VAConfigAttribMaxPictureHeight:
			attributes[i].value = have_limits ? limits.max_height :
							    VA_ATTRIB_NOT_SUPPORTED;
			break;

		default:
			attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryDisplayAttributes(VADriverContextP context,
				       VADisplayAttribute *attributes,
				       int *attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestGetDisplayAttributes(VADriverContextP context,
				     VADisplayAttribute *attributes,
				     int attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestSetDisplayAttributes(VADriverContextP context,
				     VADisplayAttribute *attributes,
				     int attributes_count)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}
