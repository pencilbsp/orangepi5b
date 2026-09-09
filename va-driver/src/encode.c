/*
 * Copyright (C) 2026
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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <va/va.h>

#include "buffer.h"
#include "encode.h"
#include "request.h"
#include "surface.h"
#include "utils.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define REQUEST_ENCODE_TIMEOUT_MS 2000

static bool encode_is_mplane(unsigned int type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	       type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

static unsigned int encode_nv12_size(unsigned int pitch, unsigned int height)
{
	return pitch * height + pitch * ((height + 1) / 2);
}

static unsigned int encode_profile_menu(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
		return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
	case VAProfileH264Main:
		return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
	case VAProfileH264High:
	default:
		return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	}
}

static unsigned int encode_level_menu(unsigned int level_idc)
{
	switch (level_idc) {
	case 9:
		return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
	case 10:
		return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
	case 11:
		return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
	case 12:
		return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
	case 13:
		return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
	case 20:
		return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
	case 21:
		return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
	case 22:
		return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
	case 30:
		return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
	case 31:
		return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
	case 32:
		return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
	case 40:
		return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
	case 41:
		return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
	case 42:
		return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
	case 50:
		return V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
	case 51:
		return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
	case 52:
		return V4L2_MPEG_VIDEO_H264_LEVEL_5_2;
	case 60:
		return V4L2_MPEG_VIDEO_H264_LEVEL_6_0;
	case 61:
		return V4L2_MPEG_VIDEO_H264_LEVEL_6_1;
	case 62:
		return V4L2_MPEG_VIDEO_H264_LEVEL_6_2;
	default:
		return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
	}
}

static void encode_try_ctrl(int video_fd, unsigned int id, int value)
{
	struct v4l2_ext_control control;
	struct v4l2_ext_controls controls;

	memset(&control, 0, sizeof(control));
	control.id = id;
	control.value = value;

	memset(&controls, 0, sizeof(controls));
	controls.controls = &control;
	controls.count = 1;

	ioctl(video_fd, VIDIOC_S_EXT_CTRLS, &controls);
}

static int encode_set_format(int video_fd, unsigned int type,
			     unsigned int pixelformat, unsigned int width,
			     unsigned int height, unsigned int bytesperline,
			     unsigned int sizeimage)
{
	struct v4l2_format format;

	memset(&format, 0, sizeof(format));
	format.type = type;

	if (encode_is_mplane(type)) {
		format.fmt.pix_mp.width = width;
		format.fmt.pix_mp.height = height;
		format.fmt.pix_mp.pixelformat = pixelformat;
		format.fmt.pix_mp.num_planes = 1;
		format.fmt.pix_mp.field = V4L2_FIELD_NONE;
		format.fmt.pix_mp.plane_fmt[0].bytesperline = bytesperline;
		format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
	} else {
		format.fmt.pix.width = width;
		format.fmt.pix.height = height;
		format.fmt.pix.pixelformat = pixelformat;
		format.fmt.pix.field = V4L2_FIELD_NONE;
		format.fmt.pix.bytesperline = bytesperline;
		format.fmt.pix.sizeimage = sizeimage;
	}

	return ioctl(video_fd, VIDIOC_S_FMT, &format);
}

static int encode_get_format(int video_fd, unsigned int type,
			     unsigned int *width, unsigned int *height,
			     unsigned int *bytesperline,
			     unsigned int *sizeimage)
{
	struct v4l2_format format;

	memset(&format, 0, sizeof(format));
	format.type = type;

	if (ioctl(video_fd, VIDIOC_G_FMT, &format) < 0)
		return -1;

	if (encode_is_mplane(type)) {
		if (width != NULL)
			*width = format.fmt.pix_mp.width;
		if (height != NULL)
			*height = format.fmt.pix_mp.height;
		if (bytesperline != NULL)
			*bytesperline =
				format.fmt.pix_mp.plane_fmt[0].bytesperline;
		if (sizeimage != NULL)
			*sizeimage = format.fmt.pix_mp.plane_fmt[0].sizeimage;
	} else {
		if (width != NULL)
			*width = format.fmt.pix.width;
		if (height != NULL)
			*height = format.fmt.pix.height;
		if (bytesperline != NULL)
			*bytesperline = format.fmt.pix.bytesperline;
		if (sizeimage != NULL)
			*sizeimage = format.fmt.pix.sizeimage;
	}

	return 0;
}

static int encode_request_buffers(int video_fd, unsigned int type,
				  unsigned int requested,
				  unsigned int *granted)
{
	struct v4l2_requestbuffers buffers;

	memset(&buffers, 0, sizeof(buffers));
	buffers.type = type;
	buffers.memory = V4L2_MEMORY_MMAP;
	buffers.count = requested;

	if (ioctl(video_fd, VIDIOC_REQBUFS, &buffers) < 0)
		return -1;

	if (granted != NULL)
		*granted = buffers.count;

	return buffers.count > 0 ? 0 : -1;
}

static int encode_query_buffer(int video_fd, unsigned int type,
			       unsigned int index, unsigned int *length,
			       unsigned int *offset)
{
	struct v4l2_plane plane;
	struct v4l2_buffer buffer;

	memset(&plane, 0, sizeof(plane));
	memset(&buffer, 0, sizeof(buffer));
	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;

	if (encode_is_mplane(type)) {
		buffer.length = 1;
		buffer.m.planes = &plane;
	}

	if (ioctl(video_fd, VIDIOC_QUERYBUF, &buffer) < 0)
		return -1;

	if (encode_is_mplane(type)) {
		*length = plane.length;
		*offset = plane.m.mem_offset;
	} else {
		*length = buffer.length;
		*offset = buffer.m.offset;
	}

	return 0;
}

static int encode_map_queue(int video_fd, unsigned int type,
			    unsigned int requested, unsigned int max_buffers,
			    void **data, unsigned int *lengths,
			    unsigned int *buffers_count)
{
	unsigned int granted;
	unsigned int i;

	if (encode_request_buffers(video_fd, type, requested, &granted) < 0)
		return -1;

	if (granted > max_buffers)
		granted = max_buffers;

	for (i = 0; i < granted; i++) {
		unsigned int length, offset;

		if (encode_query_buffer(video_fd, type, i, &length, &offset) < 0)
			return -1;

		data[i] = mmap(NULL, length, PROT_READ | PROT_WRITE,
			       MAP_SHARED, video_fd, offset);
		if (data[i] == MAP_FAILED) {
			data[i] = NULL;
			return -1;
		}

		lengths[i] = length;
	}

	*buffers_count = granted;
	return 0;
}

static void encode_unmap_queue(void **data, unsigned int *lengths,
			       unsigned int buffers_count)
{
	unsigned int i;

	for (i = 0; i < buffers_count; i++) {
		if (data[i] != NULL)
			munmap(data[i], lengths[i]);
		data[i] = NULL;
		lengths[i] = 0;
	}
}

static int encode_queue_buffer(int video_fd, unsigned int type,
			       unsigned int index, unsigned int bytesused)
{
	struct v4l2_plane plane;
	struct v4l2_buffer buffer;

	memset(&plane, 0, sizeof(plane));
	memset(&buffer, 0, sizeof(buffer));
	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.field = V4L2_FIELD_NONE;

	if (encode_is_mplane(type)) {
		buffer.length = 1;
		buffer.m.planes = &plane;
		plane.bytesused = bytesused;
	} else {
		buffer.bytesused = bytesused;
	}

	return ioctl(video_fd, VIDIOC_QBUF, &buffer);
}

static int encode_dequeue_buffer(int video_fd, unsigned int type,
				 unsigned int *index, unsigned int *bytesused)
{
	struct v4l2_plane plane;
	struct v4l2_buffer buffer;

	memset(&plane, 0, sizeof(plane));
	memset(&buffer, 0, sizeof(buffer));
	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;

	if (encode_is_mplane(type)) {
		buffer.length = 1;
		buffer.m.planes = &plane;
	}

	if (ioctl(video_fd, VIDIOC_DQBUF, &buffer) < 0)
		return -1;

	if (index != NULL)
		*index = buffer.index;
	if (bytesused != NULL)
		*bytesused = encode_is_mplane(type) ? plane.bytesused :
						   buffer.bytesused;

	return 0;
}

static int encode_wait_ready(int video_fd)
{
	struct pollfd pollfd;
	int rc;

	memset(&pollfd, 0, sizeof(pollfd));
	pollfd.fd = video_fd;
	pollfd.events = POLLIN | POLLERR | POLLPRI;

	do {
		rc = poll(&pollfd, 1, REQUEST_ENCODE_TIMEOUT_MS);
	} while (rc < 0 && errno == EINTR);

	if (rc <= 0)
		return -1;

	return (pollfd.revents & (POLLIN | POLLERR | POLLPRI)) ? 0 : -1;
}

static int encode_alloc_heap(unsigned int size)
{
	static const char *heap_paths[] = {
		"/dev/dma_heap/default_cma_region",
		"/dev/dma_heap/system",
	};
	struct dma_heap_allocation_data alloc;
	unsigned int i;
	int heap_fd;
	int rc;

	size = ALIGN_UP(size, 4096);

	for (i = 0; i < ARRAY_SIZE(heap_paths); i++) {
		heap_fd = open(heap_paths[i], O_RDONLY | O_CLOEXEC);
		if (heap_fd < 0)
			continue;

		memset(&alloc, 0, sizeof(alloc));
		alloc.len = size;
		alloc.fd_flags = O_RDWR | O_CLOEXEC;

		rc = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
		close(heap_fd);

		if (rc == 0)
			return alloc.fd;
	}

	return -1;
}

static void encode_set_crop(struct encode_context *encode,
			    struct encode_picture_params *params)
{
	struct v4l2_selection selection;
	unsigned int crop_width = encode->width;
	unsigned int crop_height = encode->height;

	if (params->have_sequence) {
		crop_width = params->sequence.picture_width_in_mbs * 16;
		crop_height = params->sequence.picture_height_in_mbs * 16;

		if (params->sequence.frame_cropping_flag) {
			crop_width -= params->sequence.frame_crop_left_offset * 2;
			crop_width -= params->sequence.frame_crop_right_offset * 2;
			crop_height -= params->sequence.frame_crop_top_offset * 2;
			crop_height -= params->sequence.frame_crop_bottom_offset * 2;
		}
	}

	if (crop_width == 0 || crop_height == 0)
		return;

	if (crop_width > encode->raw_width)
		crop_width = encode->raw_width;
	if (crop_height > encode->raw_height)
		crop_height = encode->raw_height;

	memset(&selection, 0, sizeof(selection));
	selection.type = encode->raw_type;
	selection.target = V4L2_SEL_TGT_CROP;
	selection.r.width = crop_width;
	selection.r.height = crop_height;

	ioctl(encode->video_fd, VIDIOC_S_SELECTION, &selection);
}

static unsigned int encode_clamp_qp(int qp)
{
	if (qp < 0)
		return 0;
	if (qp > 51)
		return 51;
	return qp;
}

static void encode_apply_params(struct encode_context *encode,
				struct encode_picture_params *params)
{
	unsigned int level;
	unsigned int qp_i;
	unsigned int qp_p;
	bool idr = false;

	if (params->have_sequence) {
		if (params->sequence.bits_per_second != 0)
			params->bitrate = params->sequence.bits_per_second;

		level = encode_level_menu(params->sequence.level_idc);
		if (!encode->sequence_seen || encode->applied_level != level) {
			encode_try_ctrl(encode->video_fd,
					V4L2_CID_MPEG_VIDEO_H264_LEVEL,
					level);
			encode->applied_level = level;
		}

		encode->sequence_seen = true;
	}

	if (params->bitrate != 0 &&
	    encode->applied_bitrate != params->bitrate) {
		encode_try_ctrl(encode->video_fd, V4L2_CID_MPEG_VIDEO_BITRATE,
				params->bitrate);
		encode->applied_bitrate = params->bitrate;
	}

	if (params->framerate_num != 0 &&
	    (encode->applied_framerate_num != params->framerate_num ||
	     encode->applied_framerate_den != params->framerate_den)) {
		struct v4l2_streamparm parm;

		memset(&parm, 0, sizeof(parm));
		parm.type = encode->raw_type;
		parm.parm.output.timeperframe.numerator =
			params->framerate_den ?: 1;
		parm.parm.output.timeperframe.denominator =
			params->framerate_num;
		ioctl(encode->video_fd, VIDIOC_S_PARM, &parm);

		encode->applied_framerate_num = params->framerate_num;
		encode->applied_framerate_den = params->framerate_den;
	}

	if (params->qp_min != 0 &&
	    encode->applied_qp_min != params->qp_min) {
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
				params->qp_min);
		encode->applied_qp_min = params->qp_min;
	}

	if (params->qp_max != 0 &&
	    encode->applied_qp_max != params->qp_max) {
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
				params->qp_max);
		encode->applied_qp_max = params->qp_max;
	}

	if (!params->have_picture)
		return;

	if (params->picture.pic_fields.bits.entropy_coding_mode_flag)
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
				V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC);
	else
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
				V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC);

	encode_try_ctrl(encode->video_fd,
			V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM,
			params->picture.pic_fields.bits.transform_8x8_mode_flag);

	qp_i = encode_clamp_qp(params->picture.pic_init_qp);
	qp_p = qp_i;
	if (params->have_slice)
		qp_p = encode_clamp_qp((int)params->picture.pic_init_qp +
				       params->slice.slice_qp_delta);

	if (encode->applied_qp_i != qp_i) {
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp_i);
		encode->applied_qp_i = qp_i;
	}

	if (encode->applied_qp_p != qp_p) {
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp_p);
		encode->applied_qp_p = qp_p;
	}

	if (params->picture.pic_fields.bits.idr_pic_flag)
		idr = true;
	if (params->have_slice &&
	    (params->slice.slice_type == 2 || params->slice.slice_type == 7))
		idr = true;

	if (idr)
		encode_try_ctrl(encode->video_fd,
				V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0);
}

static void encode_fill_raw(struct encode_context *encode, unsigned int index,
			    struct object_surface *surface_object)
{
	unsigned int dst_pitch = encode->raw_pitch ?: encode->raw_width;
	unsigned int src_pitch = surface_object->encode_pitch;
	unsigned int dst_height = encode->raw_height;
	unsigned int src_height = surface_object->height;
	unsigned int copy = dst_pitch < src_pitch ? dst_pitch : src_pitch;
	unsigned int luma_rows;
	unsigned int chroma_rows;
	unsigned int dst_chroma_rows;
	unsigned int src_chroma_rows;
	unsigned int i;
	unsigned char *dst = encode->raw_data[index];
	const unsigned char *src = surface_object->encode_data;

	if (dst == NULL || src == NULL || src_pitch == 0 ||
	    dst_pitch == 0 || dst_height == 0)
		return;

	memset(dst, 0, encode->raw_size);

	luma_rows = src_height < dst_height ? src_height : dst_height;
	for (i = 0; i < luma_rows; i++)
		memcpy(dst + dst_pitch * i, src + src_pitch * i, copy);

	for (; i < dst_height; i++)
		memcpy(dst + dst_pitch * i,
		       src + src_pitch * (luma_rows ? luma_rows - 1 : 0),
		       copy);

	dst += dst_pitch * dst_height;
	src += src_pitch * src_height;
	src_chroma_rows = (src_height + 1) / 2;
	dst_chroma_rows = (dst_height + 1) / 2;
	chroma_rows = src_chroma_rows < dst_chroma_rows ?
		      src_chroma_rows : dst_chroma_rows;

	for (i = 0; i < chroma_rows; i++)
		memcpy(dst + dst_pitch * i, src + src_pitch * i, copy);

	for (; i < dst_chroma_rows; i++)
		memcpy(dst + dst_pitch * i,
		       src + src_pitch * (chroma_rows ? chroma_rows - 1 : 0),
		       copy);
}

static VAStatus encode_append_packed(struct encode_picture_params *params,
				     VACodedBufferSegment *segment)
{
	unsigned int i;
	unsigned int offset = 0;
	unsigned char *dst = segment->buf;

	for (i = 0; i < params->num_packed; i++) {
		unsigned int bytes = (params->packed_bits[i] + 7) / 8;

		if (params->packed_type[i] != VAEncPackedHeaderRawData)
			continue;

		if (offset + bytes > segment->size)
			return VA_STATUS_ERROR_OPERATION_FAILED;

		memcpy(dst + offset, params->packed_data[i], bytes);
		offset += bytes;
	}

	segment->size = offset;
	return VA_STATUS_SUCCESS;
}

int encode_context_create(struct request_data *driver_data,
			  struct encode_context *encode, VAProfile profile,
			  unsigned int rc_mode, unsigned int width,
			  unsigned int height)
{
	unsigned int raw_pitch = ALIGN_UP(width, 16);
	unsigned int raw_size = encode_nv12_size(raw_pitch, height);
	unsigned int coded_size = width * height;
	int rc;

	memset(encode, 0, sizeof(*encode));
	encode->video_fd = -1;
	encode->raw_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	encode->coded_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	encode->raw_pixelformat = V4L2_PIX_FMT_NV12;
	encode->coded_pixelformat = V4L2_PIX_FMT_H264;
	encode->width = width;
	encode->height = height;

	if (!driver_data->has_encoder)
		return -1;

	encode->video_fd = open(driver_data->encoder_video_path,
				O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (encode->video_fd < 0)
		return -1;

	encode_try_ctrl(encode->video_fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			encode_profile_menu(profile));
	encode_try_ctrl(encode->video_fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			(rc_mode & VA_RC_CQP) ? V4L2_MPEG_VIDEO_BITRATE_MODE_CQ :
			(rc_mode & VA_RC_CBR) ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR :
						V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	encode->applied_constant_quality = !!(rc_mode & VA_RC_CQP);

	coded_size = coded_size < (1024 * 1024) ? 1024 * 1024 : coded_size;
	rc = encode_set_format(encode->video_fd, encode->coded_type,
			       encode->coded_pixelformat, width, height, 0,
			       coded_size);
	if (rc < 0)
		goto error;

	rc = encode_set_format(encode->video_fd, encode->raw_type,
			       encode->raw_pixelformat, width, height,
			       raw_pitch, raw_size);
	if (rc < 0)
		goto error;

	rc = encode_get_format(encode->video_fd, encode->raw_type,
			       &encode->raw_width, &encode->raw_height,
			       &encode->raw_pitch, &encode->raw_size);
	if (rc < 0)
		goto error;

	if (encode->raw_pitch == 0)
		encode->raw_pitch = raw_pitch;
	if (encode->raw_size == 0)
		encode->raw_size = encode_nv12_size(encode->raw_pitch,
						    encode->raw_height);

	rc = encode_get_format(encode->video_fd, encode->coded_type, NULL,
			       NULL, NULL, &encode->coded_size);
	if (rc < 0)
		goto error;

	if (encode->coded_size == 0)
		encode->coded_size = coded_size;

	rc = encode_map_queue(encode->video_fd, encode->raw_type,
			      REQUEST_ENCODE_RAW_BUFFERS,
			      REQUEST_ENCODE_RAW_BUFFERS, encode->raw_data,
			      encode->raw_length, &encode->num_raw_buffers);
	if (rc < 0)
		goto error;

	rc = encode_map_queue(encode->video_fd, encode->coded_type,
			      REQUEST_ENCODE_CODED_BUFFERS,
			      REQUEST_ENCODE_CODED_BUFFERS,
			      encode->coded_data, encode->coded_length,
			      &encode->num_coded_buffers);
	if (rc < 0)
		goto error;

	if (ioctl(encode->video_fd, VIDIOC_STREAMON, &encode->coded_type) < 0)
		goto error;
	if (ioctl(encode->video_fd, VIDIOC_STREAMON, &encode->raw_type) < 0)
		goto error;

	request_log("encode: %s %ux%u, %u raw buffers, %u coded buffers\n",
		    driver_data->encoder_video_path, encode->raw_width,
		    encode->raw_height, encode->num_raw_buffers,
		    encode->num_coded_buffers);

	return 0;

error:
	request_log("encode: cannot configure %s: %s\n",
		    driver_data->encoder_video_path, strerror(errno));
	encode_context_destroy(encode);
	return -1;
}

void encode_context_destroy(struct encode_context *encode)
{
	if (encode->video_fd < 0)
		return;

	ioctl(encode->video_fd, VIDIOC_STREAMOFF, &encode->raw_type);
	ioctl(encode->video_fd, VIDIOC_STREAMOFF, &encode->coded_type);

	encode_unmap_queue(encode->raw_data, encode->raw_length,
			   encode->num_raw_buffers);
	encode_unmap_queue(encode->coded_data, encode->coded_length,
			   encode->num_coded_buffers);

	encode_request_buffers(encode->video_fd, encode->raw_type, 0, NULL);
	encode_request_buffers(encode->video_fd, encode->coded_type, 0, NULL);

	close(encode->video_fd);
	encode->video_fd = -1;
	encode->num_raw_buffers = 0;
	encode->num_coded_buffers = 0;
}

void encode_params_reset(struct encode_picture_params *params)
{
	memset(params, 0, sizeof(*params));
}

static void encode_collect_misc(struct encode_picture_params *params,
				struct object_buffer *buffer_object)
{
	VAEncMiscParameterBuffer *misc = buffer_object->data;

	if (buffer_object->size * buffer_object->count < sizeof(*misc) ||
	    misc == NULL)
		return;

	switch (misc->type) {
	case VAEncMiscParameterTypeRateControl: {
		VAEncMiscParameterRateControl *rc =
			(VAEncMiscParameterRateControl *)misc->data;

		if (buffer_object->size * buffer_object->count <
		    sizeof(*misc) + sizeof(*rc))
			return;

		params->bitrate = rc->bits_per_second;
		params->qp_min = rc->min_qp;
		params->qp_max = rc->max_qp;
		break;
	}
	case VAEncMiscParameterTypeFrameRate: {
		VAEncMiscParameterFrameRate *fr =
			(VAEncMiscParameterFrameRate *)misc->data;

		if (buffer_object->size * buffer_object->count <
		    sizeof(*misc) + sizeof(*fr))
			return;

		params->framerate_num = fr->framerate & 0xffff;
		params->framerate_den = fr->framerate >> 16;
		if (params->framerate_den == 0)
			params->framerate_den = 1;
		break;
	}
	default:
		break;
	}
}

static void encode_collect_packed(struct encode_picture_params *params,
				  struct object_buffer *buffer_object)
{
	if (buffer_object->type == VAEncPackedHeaderParameterBufferType) {
		VAEncPackedHeaderParameterBuffer *packed =
			buffer_object->data;

		if (buffer_object->size * buffer_object->count <
		    sizeof(*packed))
			return;

		params->packed_pending = true;
		params->packed_type_pending = packed->type;
		params->packed_bits_pending = packed->bit_length;
		return;
	}

	if (buffer_object->type != VAEncPackedHeaderDataBufferType ||
	    !params->packed_pending)
		return;

	if (params->num_packed < REQUEST_ENCODE_MAX_PACKED_HEADERS) {
		unsigned int i = params->num_packed++;

		params->packed_type[i] = params->packed_type_pending;
		params->packed_bits[i] = params->packed_bits_pending;
		params->packed_data[i] = buffer_object->data;
	}

	params->packed_pending = false;
}

VAStatus encode_params_collect(struct encode_picture_params *params,
			       struct object_buffer *buffer_object)
{
	switch (buffer_object->type) {
	case VAEncSequenceParameterBufferType:
		if (buffer_object->size * buffer_object->count <
		    sizeof(params->sequence))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&params->sequence, buffer_object->data,
		       sizeof(params->sequence));
		params->have_sequence = true;
		break;

	case VAEncPictureParameterBufferType:
		if (buffer_object->size * buffer_object->count <
		    sizeof(params->picture))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&params->picture, buffer_object->data,
		       sizeof(params->picture));
		params->have_picture = true;
		break;

	case VAEncSliceParameterBufferType:
		if (buffer_object->size * buffer_object->count <
		    sizeof(params->slice))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		memcpy(&params->slice, buffer_object->data,
		       sizeof(params->slice));
		params->have_slice = true;
		break;

	case VAEncMiscParameterBufferType:
		encode_collect_misc(params, buffer_object);
		break;

	case VAEncPackedHeaderParameterBufferType:
	case VAEncPackedHeaderDataBufferType:
		encode_collect_packed(params, buffer_object);
		break;

	default:
		break;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus encode_surface_storage(struct object_surface *surface_object)
{
	unsigned int pitch = ALIGN_UP((unsigned int)surface_object->width, 16);
	unsigned int size = encode_nv12_size(pitch,
					     (unsigned int)surface_object->height);
	int fd;
	void *map;

	if (surface_object->encode_data != NULL)
		return VA_STATUS_SUCCESS;

	fd = encode_alloc_heap(size);
	if (fd >= 0) {
		map = mmap(NULL, ALIGN_UP(size, 4096), PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			close(fd);
			fd = -1;
		} else {
			memset(map, 0, size);
			surface_object->encode_data = map;
			surface_object->encode_fd = fd;
		}
	}

	if (surface_object->encode_data == NULL) {
		surface_object->encode_data = calloc(1, size);
		if (surface_object->encode_data == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		surface_object->encode_fd = -1;
	}

	surface_object->encode_size = size;
	surface_object->encode_pitch = pitch;

	return VA_STATUS_SUCCESS;
}

void encode_surface_release(struct object_surface *surface_object)
{
	if (surface_object->encode_data == NULL)
		return;

	if (surface_object->encode_fd >= 0) {
		munmap(surface_object->encode_data,
		       ALIGN_UP(surface_object->encode_size, 4096));
		close(surface_object->encode_fd);
	} else {
		free(surface_object->encode_data);
	}

	surface_object->encode_data = NULL;
	surface_object->encode_size = 0;
	surface_object->encode_pitch = 0;
	surface_object->encode_fd = -1;
}

VAStatus encode_picture_submit(struct encode_context *encode,
			       struct object_surface *surface_object,
			       struct encode_picture_params *params,
			       struct object_buffer *coded_buffer)
{
	VACodedBufferSegment *segment;
	unsigned int raw_index;
	unsigned int coded_index;
	unsigned int returned_raw;
	unsigned int returned_coded;
	unsigned int bytesused = 0;
	VAStatus status;

	if (!params->have_picture || !coded_buffer->coded_segment)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	if (encode->num_raw_buffers == 0 || encode->num_coded_buffers == 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	status = encode_surface_storage(surface_object);
	if (status != VA_STATUS_SUCCESS)
		return status;

	segment = coded_buffer->data;
	memset(segment, 0, sizeof(*segment));
	segment->buf = (unsigned char *)coded_buffer->data + sizeof(*segment);
	segment->next = NULL;
	segment->status = 0;
	segment->bit_offset = 0;
	segment->size = coded_buffer->coded_capacity;

	status = encode_append_packed(params, segment);
	if (status != VA_STATUS_SUCCESS)
		return status;

	raw_index = encode->next_raw++ % encode->num_raw_buffers;
	coded_index = encode->next_coded++ % encode->num_coded_buffers;

	encode_fill_raw(encode, raw_index, surface_object);
	encode_set_crop(encode, params);
	encode_apply_params(encode, params);

	if (encode_queue_buffer(encode->video_fd, encode->coded_type,
				coded_index, 0) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (encode_queue_buffer(encode->video_fd, encode->raw_type, raw_index,
				encode->raw_size) < 0) {
		encode_dequeue_buffer(encode->video_fd, encode->coded_type,
				      &returned_coded, &bytesused);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	if (encode_wait_ready(encode->video_fd) < 0)
		return VA_STATUS_ERROR_TIMEDOUT;

	if (encode_dequeue_buffer(encode->video_fd, encode->raw_type,
				  &returned_raw, NULL) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (encode_dequeue_buffer(encode->video_fd, encode->coded_type,
				  &returned_coded, &bytesused) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	if (segment->size + bytesused > coded_buffer->coded_capacity)
		bytesused = coded_buffer->coded_capacity - segment->size;

	memcpy((unsigned char *)segment->buf + segment->size,
	       encode->coded_data[returned_coded], bytesused);
	segment->size += bytesused;

	return VA_STATUS_SUCCESS;
}
