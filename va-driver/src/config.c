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
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include "context.h"
#include "utils.h"
#include "v4l2.h"

#include "autoconfig.h"

static bool config_profile_is_h264(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		return true;
	default:
		return false;
	}
}

static bool config_encoder_limits(struct request_data *driver_data,
				  struct v4l2_frame_limits *limits)
{
	int fd;
	int rc;

	if (!driver_data->has_encoder)
		return false;

	fd = open(driver_data->encoder_video_path, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return false;

	rc = v4l2_get_frame_sizes(fd, V4L2_PIX_FMT_H264, limits);
	close(fd);

	return rc == 0;
}

static void config_get_encode_attribute(bool have_limits,
					struct v4l2_frame_limits *limits,
					VAConfigAttrib *attribute)
{
	switch (attribute->type) {
	case VAConfigAttribRTFormat:
		attribute->value = VA_RT_FORMAT_YUV420;
		break;
	case VAConfigAttribRateControl:
		attribute->value = VA_RC_CQP;
		break;
	case VAConfigAttribEncMaxRefFrames:
		attribute->value = 1;
		break;
	case VAConfigAttribEncPackedHeaders:
		attribute->value = VA_ENC_PACKED_HEADER_SEQUENCE |
				    VA_ENC_PACKED_HEADER_PICTURE |
				    VA_ENC_PACKED_HEADER_SLICE |
				    VA_ENC_PACKED_HEADER_MISC |
				    VA_ENC_PACKED_HEADER_RAW_DATA;
		break;
	case VAConfigAttribEncMaxSlices:
		attribute->value = 1;
		break;
	case VAConfigAttribEncSliceStructure:
		attribute->value = VA_ENC_SLICE_STRUCTURE_EQUAL_MULTI_ROWS;
		break;
	case VAConfigAttribMaxPictureWidth:
		attribute->value = have_limits ? limits->max_width :
						  VA_ATTRIB_NOT_SUPPORTED;
		break;
	case VAConfigAttribMaxPictureHeight:
		attribute->value = have_limits ? limits->max_height :
						  VA_ATTRIB_NOT_SUPPORTED;
		break;
	case VAConfigAttribEncQualityRange:
		attribute->value = 1;
		break;
	case VAConfigAttribEncMacroblockInfo:
	case VAConfigAttribEncQuantization:
	case VAConfigAttribEncIntraRefresh:
	case VAConfigAttribEncROI:
		attribute->value = 0;
		break;
	default:
		attribute->value = VA_ATTRIB_NOT_SUPPORTED;
		break;
	}
}

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
		if (entrypoint == VAEntrypointVLD)
			break;
		if (entrypoint == VAEntrypointEncSlice && driver_data->has_encoder)
			break;
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

	case VAProfileHEVCMain:
	case VAProfileVP9Profile0:
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

	for (i = 0; i < attributes_count; i++) {
		if (attributes[i].type == VAConfigAttribRTFormat) {
			if (attributes[i].value != VA_RT_FORMAT_YUV420) {
				object_heap_free(&driver_data->config_heap,
						 (struct object_base *)config_object);
				return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
			}
			config_object->attributes[0].value = attributes[i].value;
			continue;
		}

		index = config_object->attributes_count;
		if (index >= V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES)
			break;
		config_object->attributes_count++;
		config_object->attributes[index].type = attributes[i].type;
		config_object->attributes[index].value =
			attributes[i].value;
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
	 *   AV1, VP8          - out of scope; AV1 lives on a different block
	 *   H264 Multiview /
	 *   Stereo High       - rkvdec decodes a single view
	 *   HEVC Main 10      - not wired through this VA backend
	 *   VP9 Profile 2     - kernel/NV15 support remains enabled, but the VA
	 *                       path stays hidden until Chrome preserves the
	 *                       physical DRM fourcc end to end
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

	found = v4l2_find_format_any(driver_data->video_fd,
				     V4L2_PIX_FMT_VP9_FRAME);
	if (found && index < V4L2_REQUEST_MAX_PROFILES)
		profiles[index++] = VAProfileVP9Profile0;
	*profiles_count = index;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQueryConfigEntrypoints(VADriverContextP context,
				       VAProfile profile,
				       VAEntrypoint *entrypoints,
				       int *entrypoints_count)
{
	struct request_data *driver_data = context->pDriverData;
	int index = 0;

	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
	case VAProfileH264Main:
	case VAProfileH264High:
		entrypoints[index++] = VAEntrypointVLD;
		/*
		 * The encoder is H.264 only; HEVC encode needs a second
		 * register path the kernel driver does not have yet.
		 */
		if (driver_data->has_encoder)
			entrypoints[index++] = VAEntrypointEncSlice;
		*entrypoints_count = index;
		break;

	case VAProfileHEVCMain:
	case VAProfileVP9Profile0:
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
	bool supported_decode = false;
	int i;

	if (entrypoint == VAEntrypointEncSlice && config_profile_is_h264(profile) &&
	    driver_data->has_encoder) {
		have_limits = config_encoder_limits(driver_data, &limits);

		for (i = 0; i < attributes_count; i++)
			config_get_encode_attribute(have_limits, &limits,
						    &attributes[i]);

		return VA_STATUS_SUCCESS;
	}

	supported_decode = entrypoint == VAEntrypointVLD &&
		profile_to_pixelformat(profile, &pixelformat) == 0;
	if (supported_decode)
		have_limits = v4l2_get_frame_sizes(driver_data->video_fd,
						   pixelformat, &limits) == 0;

	for (i = 0; i < attributes_count; i++) {
		switch (attributes[i].type) {
		case VAConfigAttribRTFormat:
			attributes[i].value = supported_decode ?
				VA_RT_FORMAT_YUV420 : VA_ATTRIB_NOT_SUPPORTED;
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
