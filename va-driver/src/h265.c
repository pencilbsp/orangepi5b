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
 * HEVC VA-API to V4L2 stateless mapping for rkvdec (VDPU381).
 *
 * Frame-based with Annex-B start codes, so the kernel neither advertises nor
 * wants V4L2_CID_STATELESS_HEVC_SLICE_PARAMS or ENTRY_POINT_OFFSETS: a correct
 * client submits exactly four controls per request -- SPS, PPS, scaling matrix
 * and decode params.
 *
 * Of the decode params, VDPU381 reads only pic_order_cnt_val, flags,
 * num_active_dpb_entries and the DPB itself, and of each DPB entry only
 * pic_order_cnt_val -- the timestamp is what identifies the buffer. The
 * poc_st_curr_* index arrays go unread by this hardware; they are filled
 * correctly anyway, because being right does not depend on who is looking.
 */

#include <string.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <va/va.h>

#include "context.h"
#include "h265.h"
#include "request.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

static __u64 timeval_to_ns(const struct timeval *tv)
{
	return (__u64)tv->tv_sec * 1000000000ULL + (__u64)tv->tv_usec * 1000ULL;
}

/*
 * Returns the NAL header's unit type, or 0xff when no slice NAL is found.
 * HEVC puts the type in bits 6..1 of the first header byte.
 */
static __u8 first_slice_nal_type(const __u8 *data, unsigned int size)
{
	unsigned int i;

	if (data == NULL || size < 5)
		return 0xff;

	for (i = 0; i + 4 < size; i++) {
		__u8 type;

		if (data[i] != 0x00 || data[i + 1] != 0x00)
			continue;
		if (data[i + 2] == 0x01)
			type = (data[i + 3] >> 1) & 0x3f;
		else if (data[i + 2] == 0x00 && data[i + 3] == 0x01)
			type = (data[i + 4] >> 1) & 0x3f;
		else
			continue;

		/* VCL NAL unit types are 0..31; the rest are parameter sets. */
		if (type <= 31)
			return type;
	}

	return 0xff;
}

static void fill_sps(struct v4l2_ctrl_hevc_sps *sps,
		     const VAPictureParameterBufferHEVC *picture)
{
	memset(sps, 0, sizeof(*sps));

	sps->pic_width_in_luma_samples = picture->pic_width_in_luma_samples;
	sps->pic_height_in_luma_samples = picture->pic_height_in_luma_samples;
	sps->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
	sps->log2_max_pic_order_cnt_lsb_minus4 =
		picture->log2_max_pic_order_cnt_lsb_minus4;
	sps->sps_max_dec_pic_buffering_minus1 =
		picture->sps_max_dec_pic_buffering_minus1;
	sps->log2_min_luma_coding_block_size_minus3 =
		picture->log2_min_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_luma_coding_block_size =
		picture->log2_diff_max_min_luma_coding_block_size;
	sps->log2_min_luma_transform_block_size_minus2 =
		picture->log2_min_transform_block_size_minus2;
	sps->log2_diff_max_min_luma_transform_block_size =
		picture->log2_diff_max_min_transform_block_size;
	sps->max_transform_hierarchy_depth_inter =
		picture->max_transform_hierarchy_depth_inter;
	sps->max_transform_hierarchy_depth_intra =
		picture->max_transform_hierarchy_depth_intra;
	sps->pcm_sample_bit_depth_luma_minus1 =
		picture->pcm_sample_bit_depth_luma_minus1;
	sps->pcm_sample_bit_depth_chroma_minus1 =
		picture->pcm_sample_bit_depth_chroma_minus1;
	sps->log2_min_pcm_luma_coding_block_size_minus3 =
		picture->log2_min_pcm_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_pcm_luma_coding_block_size =
		picture->log2_diff_max_min_pcm_luma_coding_block_size;
	sps->num_short_term_ref_pic_sets = picture->num_short_term_ref_pic_sets;
	sps->num_long_term_ref_pics_sps = picture->num_long_term_ref_pic_sps;
	sps->chroma_format_idc = picture->pic_fields.bits.chroma_format_idc;

	if (picture->pic_fields.bits.separate_colour_plane_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	if (picture->pic_fields.bits.scaling_list_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
	if (picture->pic_fields.bits.amp_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
	if (picture->pic_fields.bits.pcm_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
	if (picture->pic_fields.bits.pcm_loop_filter_disabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
	if (picture->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
	if (picture->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
	if (picture->pic_fields.bits.strong_intra_smoothing_enabled_flag)
		sps->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;

	/*
	 * VA-API has no sample_adaptive_offset_enabled_flag in the picture
	 * parameters; it is carried per slice. The kernel wants the SPS-level
	 * value, and every slice of a picture agrees with it, so the caller
	 * fills this in from the slice parameters.
	 */
}

static void fill_pps(struct v4l2_ctrl_hevc_pps *pps,
		     const VAPictureParameterBufferHEVC *picture)
{
	unsigned int i;

	memset(pps, 0, sizeof(*pps));

	pps->num_extra_slice_header_bits = picture->num_extra_slice_header_bits;
	pps->num_ref_idx_l0_default_active_minus1 =
		picture->num_ref_idx_l0_default_active_minus1;
	pps->num_ref_idx_l1_default_active_minus1 =
		picture->num_ref_idx_l1_default_active_minus1;
	pps->init_qp_minus26 = picture->init_qp_minus26;
	pps->diff_cu_qp_delta_depth = picture->diff_cu_qp_delta_depth;
	pps->pps_cb_qp_offset = picture->pps_cb_qp_offset;
	pps->pps_cr_qp_offset = picture->pps_cr_qp_offset;
	pps->pps_beta_offset_div2 = picture->pps_beta_offset_div2;
	pps->pps_tc_offset_div2 = picture->pps_tc_offset_div2;
	pps->log2_parallel_merge_level_minus2 =
		picture->log2_parallel_merge_level_minus2;

	if (picture->pic_fields.bits.tiles_enabled_flag) {
		pps->num_tile_columns_minus1 = picture->num_tile_columns_minus1;
		pps->num_tile_rows_minus1 = picture->num_tile_rows_minus1;

		/*
		 * VA-API always sends the explicit column and row geometry,
		 * even when the tiles are uniformly spaced, and never reports
		 * uniform_spacing_flag. Passing the geometry through is
		 * therefore always correct; claiming uniform spacing without
		 * the flag would not be.
		 */
		for (i = 0; i < 20 && i <= picture->num_tile_columns_minus1; i++)
			pps->column_width_minus1[i] =
				picture->column_width_minus1[i];
		for (i = 0; i < 22 && i <= picture->num_tile_rows_minus1; i++)
			pps->row_height_minus1[i] =
				picture->row_height_minus1[i];

		pps->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
		if (picture->pic_fields.bits.loop_filter_across_tiles_enabled_flag)
			pps->flags |=
				V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
	}

	if (picture->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
	if (picture->slice_parsing_fields.bits.output_flag_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
	if (picture->pic_fields.bits.sign_data_hiding_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
	if (picture->slice_parsing_fields.bits.cabac_init_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
	if (picture->pic_fields.bits.constrained_intra_pred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (picture->pic_fields.bits.transform_skip_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
	if (picture->pic_fields.bits.cu_qp_delta_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
	if (picture->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag)
		pps->flags |=
			V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
	if (picture->pic_fields.bits.weighted_pred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
	if (picture->pic_fields.bits.weighted_bipred_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
	if (picture->pic_fields.bits.transquant_bypass_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
	if (picture->pic_fields.bits.entropy_coding_sync_enabled_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
	if (picture->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag)
		pps->flags |=
			V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
	if (picture->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag)
		pps->flags |=
			V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
	if (picture->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
	if (picture->slice_parsing_fields.bits.lists_modification_present_flag)
		pps->flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
	if (picture->slice_parsing_fields.bits.slice_segment_header_extension_present_flag)
		pps->flags |=
			V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT;
}

static void fill_scaling_matrix(struct v4l2_ctrl_hevc_scaling_matrix *matrix,
				const VAIQMatrixBufferHEVC *va_matrix)
{
	unsigned int i;

	memset(matrix, 0, sizeof(*matrix));

	for (i = 0; i < 6; i++)
		memcpy(matrix->scaling_list_4x4[i],
		       va_matrix->ScalingList4x4[i],
		       sizeof(matrix->scaling_list_4x4[i]));
	for (i = 0; i < 6; i++)
		memcpy(matrix->scaling_list_8x8[i],
		       va_matrix->ScalingList8x8[i],
		       sizeof(matrix->scaling_list_8x8[i]));
	for (i = 0; i < 6; i++)
		memcpy(matrix->scaling_list_16x16[i],
		       va_matrix->ScalingList16x16[i],
		       sizeof(matrix->scaling_list_16x16[i]));
	/*
	 * VA-API carries two 32x32 lists, the kernel six: HEVC only codes
	 * 32x32 lists for the luma component of intra and inter prediction.
	 */
	for (i = 0; i < 2; i++)
		memcpy(matrix->scaling_list_32x32[i],
		       va_matrix->ScalingList32x32[i],
		       sizeof(matrix->scaling_list_32x32[i]));

	for (i = 0; i < 6; i++) {
		matrix->scaling_list_dc_coef_16x16[i] =
			va_matrix->ScalingListDC16x16[i];
		if (i < 2)
			matrix->scaling_list_dc_coef_32x32[i] =
				va_matrix->ScalingListDC32x32[i];
	}
}

/*
 * Pack the reference frames into the V4L2 DPB, consecutively from slot zero,
 * and classify each one into the short/long term index lists while we are
 * walking them.
 *
 * The same rule as H.264 applies: rkvdec maps a slot index onto a hardware
 * reference register, so an array with holes describes a different set of
 * references than the one intended.
 */
static void fill_dpb(struct v4l2_ctrl_hevc_decode_params *decode,
		     struct request_data *driver_data,
		     const VAPictureParameterBufferHEVC *picture)
{
	unsigned int slot = 0;
	unsigned int i;

	memset(decode->dpb, 0, sizeof(decode->dpb));
	decode->num_active_dpb_entries = 0;
	decode->num_poc_st_curr_before = 0;
	decode->num_poc_st_curr_after = 0;
	decode->num_poc_lt_curr = 0;

	for (i = 0; i < 15 && slot < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		const VAPictureHEVC *pic = &picture->ReferenceFrames[i];
		struct v4l2_hevc_dpb_entry *entry = &decode->dpb[slot];
		struct object_surface *surface;

		if (pic->picture_id == VA_INVALID_SURFACE ||
		    (pic->flags & VA_PICTURE_HEVC_INVALID))
			continue;

		surface = SURFACE(driver_data, pic->picture_id);
		if (surface == NULL)
			continue;

		entry->timestamp = timeval_to_ns(&surface->timestamp);
		entry->pic_order_cnt_val = pic->pic_order_cnt;
		entry->field_pic = 0;
		entry->flags = 0;
		if (pic->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE)
			entry->flags |= V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;

		/*
		 * The index lists refer to DPB slots, so they have to be built
		 * from the packed position rather than from VA's array index.
		 */
		if (pic->flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE)
			decode->poc_st_curr_before[decode->num_poc_st_curr_before++] =
				slot;
		else if (pic->flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)
			decode->poc_st_curr_after[decode->num_poc_st_curr_after++] =
				slot;
		else if (pic->flags & VA_PICTURE_HEVC_RPS_LT_CURR)
			decode->poc_lt_curr[decode->num_poc_lt_curr++] = slot;

		slot++;
	}

	decode->num_active_dpb_entries = slot;
}

/*
 * See the note on h264_set_device_sps(). For HEVC this is not optional:
 * rkvdec_hevc_start() runs at STREAMON and rejects an unset SPS outright,
 * because chroma_format_idc == 0 is not 4:2:0.
 */
int h265_set_device_sps(struct decoder_session *session,
			struct object_surface *surface)
{
	struct v4l2_ctrl_hevc_sps sps;

	fill_sps(&sps, &surface->params.h265.picture);

	return v4l2_set_control(session->video_fd, -1,
				V4L2_CID_STATELESS_HEVC_SPS, &sps, sizeof(sps));
}

int h265_set_controls(struct request_data *driver_data,
		      struct decoder_session *session,
		      struct object_context *context,
		      struct object_surface *surface)
{
	const VAPictureParameterBufferHEVC *picture;
	const VASliceParameterBufferHEVC *slice;
	struct v4l2_ctrl_hevc_scaling_matrix matrix;
	struct v4l2_ctrl_hevc_decode_params decode;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps;
	struct v4l2_ext_control controls[4];
	__u8 nal_type;

	(void)context;


	picture = &surface->params.h265.picture;
	slice = &surface->params.h265.slice;

	nal_type = first_slice_nal_type(surface->source_data,
					surface->slices_size);
	if (nal_type == 0xff) {
		request_log("h265: no slice NAL in access unit\n");
		return -1;
	}

	fill_sps(&sps, picture);
	/*
	 * SAO is an SPS-level switch that VA-API only reports per slice. Every
	 * slice of a picture carries the same value, so the first one answers
	 * for the sequence.
	 */
	if (slice->LongSliceFlags.fields.slice_sao_luma_flag ||
	    slice->LongSliceFlags.fields.slice_sao_chroma_flag)
		sps.flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;

	fill_pps(&pps, picture);
	fill_scaling_matrix(&matrix, &surface->params.h265.iqmatrix);

	memset(&decode, 0, sizeof(decode));
	fill_dpb(&decode, driver_data, picture);
	decode.pic_order_cnt_val = picture->CurrPic.pic_order_cnt;
	decode.short_term_ref_pic_set_size = picture->st_rps_bits;

	/*
	 * NAL unit types 16..23 are IRAP pictures; 19 and 20 (IDR_W_RADL and
	 * IDR_N_LP) are the IDRs among them.
	 */
	if (nal_type >= 16 && nal_type <= 23)
		decode.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
	if (nal_type == 19 || nal_type == 20)
		decode.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;

	memset(controls, 0, sizeof(controls));
	controls[0].id = V4L2_CID_STATELESS_HEVC_SPS;
	controls[0].ptr = &sps;
	controls[0].size = sizeof(sps);
	controls[1].id = V4L2_CID_STATELESS_HEVC_PPS;
	controls[1].ptr = &pps;
	controls[1].size = sizeof(pps);
	controls[2].id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX;
	controls[2].ptr = &matrix;
	controls[2].size = sizeof(matrix);
	controls[3].id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS;
	controls[3].ptr = &decode;
	controls[3].size = sizeof(decode);

	return v4l2_set_controls(session->video_fd, surface->request_fd,
				 controls, 4);
}
