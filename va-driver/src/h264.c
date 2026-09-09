/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 * Copyright (C) 2026 Orange Pi 5B image builders
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * H.264 VA-API to V4L2 stateless mapping for rkvdec (VDPU381).
 *
 * Frame-based decoding with Annex-B start codes, against the mainline
 * V4L2_CID_STATELESS_H264_* ABI. The out-of-tree V4L2_CID_MPEG_VIDEO_H264_*
 * identifiers this file once used never landed and are absent from current
 * kernels.
 */

#include <string.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <va/va.h>

#include "context.h"
#include "h264.h"
#include "request.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

/*
 * The kernel identifies a reference frame by the timestamp of the CAPTURE
 * buffer it was decoded into, converted the way v4l2-common does it.
 */
static __u64 timeval_to_ns(const struct timeval *tv)
{
	return (__u64)tv->tv_sec * 1000000000ULL + (__u64)tv->tv_usec * 1000ULL;
}

/*
 * VA-API exposes only a boolean "is a reference picture", which is not enough
 * to fill nal_ref_idc, and nothing at all for the IDR flag. Both are in the
 * first VCL NAL of the access unit, so read them from the bitstream we are
 * about to hand the hardware.
 *
 * Returns the NAL header byte, or 0 when no VCL NAL is found.
 */
static __u8 first_vcl_nal_header(const __u8 *data, unsigned int size)
{
	unsigned int i;

	if (data == NULL || size < 4)
		return 0;

	for (i = 0; i + 3 < size; i++) {
		__u8 header, type;

		/* Annex-B start code, three or four bytes. */
		if (data[i] != 0x00 || data[i + 1] != 0x00)
			continue;
		if (data[i + 2] == 0x01)
			header = data[i + 3];
		else if (data[i + 2] == 0x00 && i + 4 < size &&
			 data[i + 3] == 0x01)
			header = data[i + 4];
		else
			continue;

		/* VCL NAL unit types are 1..5; anything else is metadata. */
		type = header & 0x1f;
		if (type >= 1 && type <= 5)
			return header;
	}

	return 0;
}

static void fill_sps(struct v4l2_ctrl_h264_sps *sps,
		     const VAPictureParameterBufferH264 *picture)
{
	memset(sps, 0, sizeof(*sps));

	/*
	 * VA-API carries no profile_idc, level_idc or constraint set flags:
	 * a client is not required to keep the raw SPS around. VDPU381 does
	 * not read them, so they stay zero rather than being invented.
	 */
	sps->chroma_format_idc = picture->seq_fields.bits.chroma_format_idc;
	sps->bit_depth_luma_minus8 =
		picture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 =
		picture->bit_depth_chroma_minus8;
	sps->log2_max_frame_num_minus4 =
		picture->seq_fields.bits.log2_max_frame_num_minus4;
	sps->pic_order_cnt_type =
		picture->seq_fields.bits.pic_order_cnt_type;
	sps->log2_max_pic_order_cnt_lsb_minus4 =
		picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
	sps->max_num_ref_frames = picture->num_ref_frames;
	sps->pic_width_in_mbs_minus1 = picture->picture_width_in_mbs_minus1;
	sps->pic_height_in_map_units_minus1 =
		picture->picture_height_in_mbs_minus1;

	if (picture->seq_fields.bits.residual_colour_transform_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	if (picture->seq_fields.bits.delta_pic_order_always_zero_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
	if (picture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
		sps->flags |=
			V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	if (picture->seq_fields.bits.frame_mbs_only_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (picture->seq_fields.bits.mb_adaptive_frame_field_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (picture->seq_fields.bits.direct_8x8_inference_flag)
		sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
}

static void fill_pps(struct v4l2_ctrl_h264_pps *pps,
		     const VAPictureParameterBufferH264 *picture,
		     const VASliceParameterBufferH264 *slice,
		     bool scaling_matrix_present)
{
	memset(pps, 0, sizeof(*pps));

	pps->pic_init_qp_minus26 = picture->pic_init_qp_minus26;
	pps->chroma_qp_index_offset = picture->chroma_qp_index_offset;
	pps->second_chroma_qp_index_offset =
		picture->second_chroma_qp_index_offset;
	/*
	 * VA-API does not carry the PPS defaults. It reports the values in
	 * force for the slice instead -- already resolved against
	 * num_ref_idx_active_override_flag -- and frame-based decoding sees
	 * one slice header per frame, so those are what the frame decodes
	 * with.
	 */
	if (slice != NULL) {
		pps->num_ref_idx_l0_default_active_minus1 =
			slice->num_ref_idx_l0_active_minus1;
		pps->num_ref_idx_l1_default_active_minus1 =
			slice->num_ref_idx_l1_active_minus1;
	}
	pps->weighted_bipred_idc = picture->pic_fields.bits.weighted_bipred_idc;
	/*
	 * Slice groups (ASO/FMO) exist only in unconstrained Baseline, which
	 * rkvdec does not decode and this driver does not advertise. VA-API
	 * has deprecated the field; leave it at zero rather than read it.
	 */
	pps->num_slice_groups_minus1 = 0;

	if (picture->pic_fields.bits.entropy_coding_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (picture->pic_fields.bits.pic_order_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	if (picture->pic_fields.bits.weighted_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	if (picture->pic_fields.bits.deblocking_filter_control_present_flag)
		pps->flags |=
			V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (picture->pic_fields.bits.constrained_intra_pred_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (picture->pic_fields.bits.redundant_pic_cnt_present_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	if (picture->pic_fields.bits.transform_8x8_mode_flag)
		pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	/*
	 * The kernel reads the scaling matrix control only when this flag is
	 * set; a client that sent no VAIQMatrixBuffer means "use flat lists".
	 */
	if (scaling_matrix_present)
		pps->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
}

static void fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *matrix,
				const VAIQMatrixBufferH264 *va_matrix)
{
	unsigned int i;

	memset(matrix, 0, sizeof(*matrix));

	for (i = 0; i < 6; i++)
		memcpy(matrix->scaling_list_4x4[i], va_matrix->ScalingList4x4[i],
		       sizeof(matrix->scaling_list_4x4[i]));

	/*
	 * VA-API hands over two 8x8 lists in the standardised order: index 0
	 * is Intra Y and index 1 is Inter Y. They map to V4L2 slots 0 and 1.
	 *
	 * Writing the second one to slot 3 -- as the legacy ABI's layout
	 * invited -- silently gives inter-coded blocks a flat matrix.
	 */
	for (i = 0; i < 2; i++)
		memcpy(matrix->scaling_list_8x8[i], va_matrix->ScalingList8x8[i],
		       sizeof(matrix->scaling_list_8x8[i]));
}

/*
 * Write the reference frames into the V4L2 DPB.
 *
 * The entries must be packed consecutively from slot zero. rkvdec programs one
 * hardware reference register per slot index, so an array with holes in it
 * describes a different reference set than the one intended: every picture
 * predicted from a reference past the first hole then decodes against
 * registers that were never written. The corruption starts only once a surface
 * has been released and its slot re-used, which is why the opening GOP looks
 * correct and the picture falls apart after the second IDR.
 */
static void fill_dpb(struct v4l2_ctrl_h264_decode_params *decode,
		     struct request_data *driver_data,
		     const VAPictureParameterBufferH264 *picture)
{
	unsigned int slot = 0;
	unsigned int i;

	memset(decode->dpb, 0, sizeof(decode->dpb));

	for (i = 0; i < 16 && slot < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const VAPictureH264 *pic = &picture->ReferenceFrames[i];
		struct v4l2_h264_dpb_entry *entry = &decode->dpb[slot];
		struct object_surface *surface;

		if (pic->picture_id == VA_INVALID_SURFACE ||
		    (pic->flags & VA_PICTURE_H264_INVALID))
			continue;

		surface = SURFACE(driver_data, pic->picture_id);
		if (surface == NULL)
			continue;

		entry->reference_ts = timeval_to_ns(&surface->timestamp);
		entry->frame_num = pic->frame_idx;
		/*
		 * For frame decoding PicNum is FrameNumWrap, which is what
		 * VA-API reports in frame_idx for a short-term reference.
		 */
		entry->pic_num = pic->frame_idx;
		entry->top_field_order_cnt = pic->TopFieldOrderCnt;
		entry->bottom_field_order_cnt = pic->BottomFieldOrderCnt;
		entry->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
			       V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;

		if (pic->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
			entry->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;

		/*
		 * Which halves of the entry may be referenced. Leaving this at
		 * zero says "neither field", which makes the entry useless as a
		 * reference without being invalid: intra frames still decode
		 * correctly and every inter frame comes out wrong.
		 */
		if (pic->flags & VA_PICTURE_H264_TOP_FIELD)
			entry->fields = V4L2_H264_TOP_FIELD_REF;
		else if (pic->flags & VA_PICTURE_H264_BOTTOM_FIELD)
			entry->fields = V4L2_H264_BOTTOM_FIELD_REF;
		else
			entry->fields = V4L2_H264_FRAME_REF;

		if (entry->fields != V4L2_H264_FRAME_REF)
			entry->flags |= V4L2_H264_DPB_ENTRY_FLAG_FIELD;

		slot++;
	}
}

static void fill_decode_params(struct v4l2_ctrl_h264_decode_params *decode,
			       const VAPictureParameterBufferH264 *picture,
			       const VASliceParameterBufferH264 *slice,
			       __u8 nal_header)
{
	decode->nal_ref_idc = (nal_header >> 5) & 0x3;
	decode->frame_num = picture->frame_num;
	decode->top_field_order_cnt = picture->CurrPic.TopFieldOrderCnt;
	decode->bottom_field_order_cnt = picture->CurrPic.BottomFieldOrderCnt;
	decode->idr_pic_id = 0;
	decode->pic_order_cnt_lsb = 0;
	decode->delta_pic_order_cnt_bottom = 0;
	decode->delta_pic_order_cnt0 = 0;
	decode->delta_pic_order_cnt1 = 0;
	decode->dec_ref_pic_marking_bit_size = 0;
	decode->pic_order_cnt_bit_size = 0;
	decode->slice_group_change_cycle = 0;
	decode->flags = 0;

	/* NAL unit type 5 is a slice of an IDR picture. */
	if ((nal_header & 0x1f) == 5)
		decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;

	if (slice != NULL) {
		switch (slice->slice_type % 5) {
		case 0:
			decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
			break;
		case 1:
			decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
			break;
		default:
			break;
		}
	}
}

/*
 * Set the sequence header on the device rather than on a request.
 *
 * A stateless decoder validates the sequence when streaming starts, and a
 * control attached to a request is not applied until that request is queued --
 * which cannot happen before STREAMON. So the first frame's SPS goes down
 * directly, and the per-frame path below repeats it inside each request.
 */
int h264_set_device_sps(struct decoder_session *session,
			struct object_surface *surface)
{
	struct v4l2_ctrl_h264_sps sps;

	fill_sps(&sps, &surface->params.h264.picture);

	return v4l2_set_control(session->video_fd, -1,
				V4L2_CID_STATELESS_H264_SPS, &sps, sizeof(sps));
}

int h264_set_controls(struct request_data *driver_data,
		      struct decoder_session *session,
		      struct object_context *context,
		      struct object_surface *surface)
{
	struct v4l2_ctrl_h264_scaling_matrix matrix;
	struct v4l2_ctrl_h264_decode_params decode;
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ext_control controls[4];
	bool scaling_matrix_present;
	__u8 nal_header;

	(void)context;


	nal_header = first_vcl_nal_header(surface->source_data,
					  surface->slices_size);
	if (nal_header == 0) {
		request_log("h264: no VCL NAL in access unit\n");
		return -1;
	}

	/*
	 * A client that sends no VAIQMatrixBuffer is asking for flat scaling
	 * lists, which is not the same as sending zeroed ones.
	 */
	scaling_matrix_present = surface->params.h264.matrix.ScalingList4x4[0][0] != 0;

	fill_sps(&sps, &surface->params.h264.picture);
	fill_pps(&pps, &surface->params.h264.picture,
		 &surface->params.h264.slice, scaling_matrix_present);
	fill_scaling_matrix(&matrix, &surface->params.h264.matrix);

	memset(&decode, 0, sizeof(decode));
	fill_dpb(&decode, driver_data, &surface->params.h264.picture);
	fill_decode_params(&decode, &surface->params.h264.picture,
			   &surface->params.h264.slice, nal_header);

	memset(controls, 0, sizeof(controls));
	controls[0].id = V4L2_CID_STATELESS_H264_SPS;
	controls[0].ptr = &sps;
	controls[0].size = sizeof(sps);
	controls[1].id = V4L2_CID_STATELESS_H264_PPS;
	controls[1].ptr = &pps;
	controls[1].size = sizeof(pps);
	controls[2].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
	controls[2].ptr = &matrix;
	controls[2].size = sizeof(matrix);
	controls[3].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
	controls[3].ptr = &decode;
	controls[3].size = sizeof(decode);

	/*
	 * All four go down in one ioctl against the request: a stateless
	 * decoder needs the whole frame's state to arrive atomically with the
	 * bitstream it describes.
	 */
	return v4l2_set_controls(session->video_fd, surface->request_fd,
				 controls, 4);
}
