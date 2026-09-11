/*
 * Copyright (C) 2026 pencilbsp <pencil.bsp@gmail.com>
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

/*
 * AV1 stateless backend.
 *
 * The VA-API AV1 interface hands over an already-parsed frame header, so
 * most of this file is a transcription into the four V4L2 controls the
 * kernel expects: SEQUENCE, FRAME, TILE_GROUP_ENTRY and FILM_GRAIN.
 *
 * Two things it has to work out for itself, because VA does not carry
 * them and the hardware reads them:
 *
 *   - order_hints[], the order hint of each reference this frame uses.
 *     VA only gives the current frame's hint, so the hint is remembered
 *     on the surface as each frame is decoded and looked back up through
 *     ref_frame_map[].
 *
 *   - skip_mode_frame[], which the AV1 specification derives from those
 *     same order hints (section 7.20, "skip mode params").
 */

#include "av1.h"

#include "context.h"
#include "request.h"
#include "session.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "video.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <linux/videodev2.h>
#include <va/va.h>

#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#endif

/* VA numbers references LAST..ALTREF as 0..6; V4L2 keeps the AV1
 * numbering where slot 0 is INTRA_FRAME, so the two differ by one. */
#define VA_REF_TO_V4L2(ref)	((ref) + V4L2_AV1_REF_LAST_FRAME)

static uint64_t timeval_to_ns(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000000ull +
	       (uint64_t)tv->tv_usec * 1000ull;
}

static struct object_surface *ref_surface(struct request_data *driver_data,
					  const VADecPictureParameterBufferAV1 *pic,
					  unsigned int slot)
{
	VASurfaceID surface_id;

	if (slot >= 8)
		return NULL;

	surface_id = pic->ref_frame_map[slot];
	if (surface_id == VA_INVALID_SURFACE)
		return NULL;

	return SURFACE(driver_data, surface_id);
}

/*
 * AV1 order hints wrap inside a field of order_hint_bits, so "earlier"
 * and "later" are decided on the signed distance within that field
 * rather than by comparing the raw values (specification 5.9.3).
 */
static int relative_dist(unsigned int order_hint_bits, int a, int b)
{
	int diff, m;

	if (order_hint_bits == 0)
		return 0;

	diff = a - b;
	m = 1 << (order_hint_bits - 1);

	return (diff & (m - 1)) - (diff & m);
}

/*
 * Specification 7.20. Picks the two references skip mode blocks are
 * predicted from: the nearest past frame paired with either the nearest
 * future frame or, failing that, the second nearest past frame.
 */
static void derive_skip_mode_frame(struct v4l2_ctrl_av1_frame *frame,
				   const uint32_t *order_hints,
				   unsigned int order_hint_bits,
				   int order_hint)
{
	int forward_idx = -1, backward_idx = -1, second_forward_idx = -1;
	int forward_hint = 0, backward_hint = 0, second_forward_hint = 0;
	unsigned int i;

	frame->skip_mode_frame[0] = 0;
	frame->skip_mode_frame[1] = 0;

	if (!(frame->flags & V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT))
		return;

	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		int ref_hint = (int)order_hints[VA_REF_TO_V4L2(i)];

		if (relative_dist(order_hint_bits, ref_hint, order_hint) < 0) {
			if (forward_idx < 0 ||
			    relative_dist(order_hint_bits, ref_hint,
					  forward_hint) > 0) {
				forward_idx = (int)i;
				forward_hint = ref_hint;
			}
		} else if (relative_dist(order_hint_bits, ref_hint,
					 order_hint) > 0) {
			if (backward_idx < 0 ||
			    relative_dist(order_hint_bits, ref_hint,
					  backward_hint) < 0) {
				backward_idx = (int)i;
				backward_hint = ref_hint;
			}
		}
	}

	if (forward_idx < 0)
		return;

	if (backward_idx >= 0) {
		frame->skip_mode_frame[0] = VA_REF_TO_V4L2(
			forward_idx < backward_idx ? forward_idx : backward_idx);
		frame->skip_mode_frame[1] = VA_REF_TO_V4L2(
			forward_idx < backward_idx ? backward_idx : forward_idx);
		return;
	}

	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		int ref_hint = (int)order_hints[VA_REF_TO_V4L2(i)];

		if (relative_dist(order_hint_bits, ref_hint, forward_hint) < 0) {
			if (second_forward_idx < 0 ||
			    relative_dist(order_hint_bits, ref_hint,
					  second_forward_hint) > 0) {
				second_forward_idx = (int)i;
				second_forward_hint = ref_hint;
			}
		}
	}

	if (second_forward_idx < 0)
		return;

	frame->skip_mode_frame[0] = VA_REF_TO_V4L2(
		forward_idx < second_forward_idx ? forward_idx :
						   second_forward_idx);
	frame->skip_mode_frame[1] = VA_REF_TO_V4L2(
		forward_idx < second_forward_idx ? second_forward_idx :
						   forward_idx);
}

/*
 * Reference slots use VA-API's own numbering: ref_frame_idx[] is passed
 * through untouched and reference_frame_ts[] is the DPB in ref_frame_map[]
 * order. The kernel resolves a reference by looking ref_frame_idx[] up in
 * reference_frame_ts[] and matching the timestamp, so the numbering only
 * has to be self-consistent — and it retires a frame as soon as no slot
 * still names it, which makes publishing the *whole* map, not just the
 * slots this frame reads, a correctness requirement.
 *
 * Slots are keyed by the OUTPUT-buffer timestamp of the frame that wrote
 * them, not by surface: a VA client recycles its surfaces, so the same
 * VASurfaceID stands for different frames over time.
 *
 * refresh_frame_flags is left zero. VA-API does not carry it, and since
 * the hantro driver keys entropy contexts by frame rather than by refresh
 * slot (kernel patch 0013) nothing reads it.
 */
static void fill_sequence(struct v4l2_ctrl_av1_sequence *seq,
			  const VADecPictureParameterBufferAV1 *pic)
{
	memset(seq, 0, sizeof(*seq));

	seq->seq_profile = pic->profile;
	seq->order_hint_bits = pic->order_hint_bits_minus_1 + 1;
	seq->bit_depth = 8 + 2 * pic->bit_depth_idx;
	seq->max_frame_width_minus_1 = pic->frame_width_minus1;
	seq->max_frame_height_minus_1 = pic->frame_height_minus1;

	if (pic->seq_info_fields.fields.still_picture)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_STILL_PICTURE;
	if (pic->seq_info_fields.fields.use_128x128_superblock)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK;
	if (pic->seq_info_fields.fields.enable_filter_intra)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA;
	if (pic->seq_info_fields.fields.enable_intra_edge_filter)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER;
	if (pic->seq_info_fields.fields.enable_interintra_compound)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND;
	if (pic->seq_info_fields.fields.enable_masked_compound)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND;
	if (pic->seq_info_fields.fields.enable_dual_filter)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER;
	if (pic->seq_info_fields.fields.enable_order_hint)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT;
	if (pic->seq_info_fields.fields.enable_jnt_comp)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP;
	if (pic->seq_info_fields.fields.enable_cdef)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF;
	if (pic->seq_info_fields.fields.mono_chrome)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME;
	if (pic->seq_info_fields.fields.color_range)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_COLOR_RANGE;
	if (pic->seq_info_fields.fields.subsampling_x)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X;
	if (pic->seq_info_fields.fields.subsampling_y)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
	if (pic->seq_info_fields.fields.film_grain_params_present)
		seq->flags |= V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT;
}

static unsigned int tile_log2(unsigned int n)
{
	unsigned int log2 = 0;

	while ((1u << log2) < n)
		log2++;

	return log2;
}

/*
 * With uniform tile spacing the specification derives every tile's size
 * from the tile counts alone (5.11.1), so VA-API leaves
 * width_in_sbs_minus_1[] and height_in_sbs_minus_1[] unset. FFmpeg fills
 * them in regardless and this driver used to just copy them, which hid
 * the omission: Chrome leaves them zero, the hardware is then handed a
 * frame with no tile geometry, and the decode comes back with the error
 * flag. Derive them instead of trusting the client.
 */
static void derive_uniform_tiles(struct v4l2_av1_tile_info *tile,
				 const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int shift =
		pic->seq_info_fields.fields.use_128x128_superblock ? 5 : 4;
	unsigned int mi_cols = 2 * ((pic->frame_width_minus1 + 1 + 7) >> 3);
	unsigned int mi_rows = 2 * ((pic->frame_height_minus1 + 1 + 7) >> 3);
	unsigned int sb_cols = (mi_cols + (1u << shift) - 1) >> shift;
	unsigned int sb_rows = (mi_rows + (1u << shift) - 1) >> shift;
	unsigned int step, i, k;

	if (pic->tile_cols == 0 || pic->tile_rows == 0)
		return;

	step = (sb_cols + (1u << tile_log2(pic->tile_cols)) - 1) >>
	       tile_log2(pic->tile_cols);
	for (i = 0, k = 0; i < sb_cols && k < V4L2_AV1_MAX_TILE_COLS;
	     i += step, k++)
		tile->width_in_sbs_minus_1[k] =
			(sb_cols - i < step ? sb_cols - i : step) - 1;

	step = (sb_rows + (1u << tile_log2(pic->tile_rows)) - 1) >>
	       tile_log2(pic->tile_rows);
	for (i = 0, k = 0; i < sb_rows && k < V4L2_AV1_MAX_TILE_ROWS;
	     i += step, k++)
		tile->height_in_sbs_minus_1[k] =
			(sb_rows - i < step ? sb_rows - i : step) - 1;
}

static void fill_tile_info(struct v4l2_av1_tile_info *tile,
			   const VADecPictureParameterBufferAV1 *pic,
			   const VASliceParameterBufferAV1 *tiles,
			   unsigned int num_tiles)
{
	unsigned int i;

	tile->tile_cols = pic->tile_cols;
	tile->tile_rows = pic->tile_rows;
	tile->context_update_tile_id = pic->context_update_tile_id;

	if (pic->pic_info_fields.bits.uniform_tile_spacing_flag) {
		tile->flags |= V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING;
		derive_uniform_tiles(tile, pic);
	} else {
		for (i = 0; i < pic->tile_cols && i < V4L2_AV1_MAX_TILE_COLS; i++)
			tile->width_in_sbs_minus_1[i] =
				pic->width_in_sbs_minus_1[i];
		for (i = 0; i < pic->tile_rows && i < V4L2_AV1_MAX_TILE_ROWS; i++)
			tile->height_in_sbs_minus_1[i] =
				pic->height_in_sbs_minus_1[i];
	}

	/*
	 * VA does not report tile_size_bytes, and the kernel needs it to
	 * walk a multi-tile frame. Every tile but the last is preceded by a
	 * size field of exactly that width, so the gap between where one
	 * tile ends and the next begins is the value itself. A 4K stream
	 * carrying 32 tiles gets this wrong-by-one and decodes to garbage
	 * from the very first frame, which no single-tile clip can show.
	 */
	tile->tile_size_bytes = 4;

	for (i = 0; i + 1 < num_tiles; i++) {
		uint32_t end = tiles[i].slice_data_offset +
			       tiles[i].slice_data_size;
		uint32_t gap;

		/* Only consecutive tiles of one tile group are laid out
		 * this way; a new group restarts the offsets. */
		if (tiles[i + 1].slice_data_offset <= end)
			continue;

		gap = tiles[i + 1].slice_data_offset - end;
		if (gap >= 1 && gap <= 4) {
			tile->tile_size_bytes = (uint8_t)gap;
			break;
		}
	}
}

static void fill_quantization(struct v4l2_av1_quantization *quant,
			      const VADecPictureParameterBufferAV1 *pic)
{
	quant->base_q_idx = pic->base_qindex;
	quant->delta_q_y_dc = pic->y_dc_delta_q;
	quant->delta_q_u_dc = pic->u_dc_delta_q;
	quant->delta_q_u_ac = pic->u_ac_delta_q;
	quant->delta_q_v_dc = pic->v_dc_delta_q;
	quant->delta_q_v_ac = pic->v_ac_delta_q;
	quant->qm_y = pic->qmatrix_fields.bits.qm_y;
	quant->qm_u = pic->qmatrix_fields.bits.qm_u;
	quant->qm_v = pic->qmatrix_fields.bits.qm_v;
	quant->delta_q_res = pic->mode_control_fields.bits.log2_delta_q_res;

	if (pic->qmatrix_fields.bits.using_qmatrix)
		quant->flags |= V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX;
	if (pic->mode_control_fields.bits.delta_q_present_flag)
		quant->flags |= V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT;
	/* The u and v planes carry separate deltas only when they differ. */
	if (pic->u_dc_delta_q != pic->v_dc_delta_q ||
	    pic->u_ac_delta_q != pic->v_ac_delta_q)
		quant->flags |= V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA;
}

static void fill_segmentation(struct v4l2_av1_segmentation *seg,
			      const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int i, j;

	if (pic->seg_info.segment_info_fields.bits.enabled)
		seg->flags |= V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
	if (pic->seg_info.segment_info_fields.bits.update_map)
		seg->flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
	if (pic->seg_info.segment_info_fields.bits.temporal_update)
		seg->flags |= V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
	if (pic->seg_info.segment_info_fields.bits.update_data)
		seg->flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA;

	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		seg->feature_enabled[i] = pic->seg_info.feature_mask[i];
		for (j = 0; j < V4L2_AV1_SEG_LVL_MAX; j++)
			seg->feature_data[i][j] =
				pic->seg_info.feature_data[i][j];

		if (seg->feature_enabled[i])
			seg->last_active_seg_id = i;
	}

	if (seg->feature_enabled[0] &
	    ((1 << V4L2_AV1_SEG_LVL_REF_FRAME) |
	     (1 << V4L2_AV1_SEG_LVL_REF_SKIP) |
	     (1 << V4L2_AV1_SEG_LVL_REF_GLOBALMV)))
		seg->flags |= V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP;
}

static void fill_loop_filter(struct v4l2_av1_loop_filter *lf,
			     const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int i;

	lf->level[0] = pic->filter_level[0];
	lf->level[1] = pic->filter_level[1];
	lf->level[2] = pic->filter_level_u;
	lf->level[3] = pic->filter_level_v;
	lf->sharpness = pic->loop_filter_info_fields.bits.sharpness_level;
	lf->delta_lf_res = pic->mode_control_fields.bits.log2_delta_lf_res;

	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
		lf->ref_deltas[i] = pic->ref_deltas[i];
	for (i = 0; i < 2; i++)
		lf->mode_deltas[i] = pic->mode_deltas[i];

	if (pic->loop_filter_info_fields.bits.mode_ref_delta_enabled)
		lf->flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED;
	if (pic->loop_filter_info_fields.bits.mode_ref_delta_update)
		lf->flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE;
	if (pic->mode_control_fields.bits.delta_lf_present_flag)
		lf->flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT;
	if (pic->mode_control_fields.bits.delta_lf_multi)
		lf->flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI;
}

/*
 * VA packs each CDEF strength into one byte as (primary << 2) | secondary,
 * where a secondary of 3 means 4 (specification 5.9.19).
 */
static void fill_cdef(struct v4l2_av1_cdef *cdef,
		      const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int i, n;

	cdef->damping_minus_3 = pic->cdef_damping_minus_3;
	cdef->bits = pic->cdef_bits;

	n = 1u << pic->cdef_bits;
	if (n > V4L2_AV1_CDEF_MAX)
		n = V4L2_AV1_CDEF_MAX;

	for (i = 0; i < n; i++) {
		uint8_t y = pic->cdef_y_strengths[i];
		uint8_t uv = pic->cdef_uv_strengths[i];

		cdef->y_pri_strength[i] = y >> 2;
		cdef->y_sec_strength[i] = (y & 0x03) == 3 ? 4 : (y & 0x03);
		cdef->uv_pri_strength[i] = uv >> 2;
		cdef->uv_sec_strength[i] = (uv & 0x03) == 3 ? 4 : (uv & 0x03);
	}
}

static void fill_loop_restoration(struct v4l2_av1_loop_restoration *lr,
				  const VADecPictureParameterBufferAV1 *pic)
{
	static const enum v4l2_av1_frame_restoration_type types[4] = {
		V4L2_AV1_FRAME_RESTORE_NONE,
		V4L2_AV1_FRAME_RESTORE_WIENER,
		V4L2_AV1_FRAME_RESTORE_SGRPROJ,
		V4L2_AV1_FRAME_RESTORE_SWITCHABLE,
	};

	lr->frame_restoration_type[0] =
		types[pic->loop_restoration_fields.bits.yframe_restoration_type & 3];
	lr->frame_restoration_type[1] =
		types[pic->loop_restoration_fields.bits.cbframe_restoration_type & 3];
	lr->frame_restoration_type[2] =
		types[pic->loop_restoration_fields.bits.crframe_restoration_type & 3];
	lr->lr_unit_shift = pic->loop_restoration_fields.bits.lr_unit_shift;
	lr->lr_uv_shift = pic->loop_restoration_fields.bits.lr_uv_shift;

	if (lr->frame_restoration_type[0] != V4L2_AV1_FRAME_RESTORE_NONE ||
	    lr->frame_restoration_type[1] != V4L2_AV1_FRAME_RESTORE_NONE ||
	    lr->frame_restoration_type[2] != V4L2_AV1_FRAME_RESTORE_NONE)
		lr->flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR;
	if (lr->frame_restoration_type[1] != V4L2_AV1_FRAME_RESTORE_NONE ||
	    lr->frame_restoration_type[2] != V4L2_AV1_FRAME_RESTORE_NONE)
		lr->flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR;
}

static void fill_global_motion(struct v4l2_av1_global_motion *gm,
			       const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int i, j;

	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		unsigned int ref = VA_REF_TO_V4L2(i);
		uint32_t type = pic->wm[i].wmtype;

		if (type > V4L2_AV1_WARP_MODEL_AFFINE)
			type = V4L2_AV1_WARP_MODEL_AFFINE;

		gm->type[ref] = type;
		for (j = 0; j < 6; j++)
			gm->params[ref][j] = pic->wm[i].wmmat[j];

		if (type != V4L2_AV1_WARP_MODEL_IDENTITY)
			gm->flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_GLOBAL;
		if (type == V4L2_AV1_WARP_MODEL_ROTZOOM)
			gm->flags[ref] |=
				V4L2_AV1_GLOBAL_MOTION_FLAG_IS_ROT_ZOOM;
		if (type == V4L2_AV1_WARP_MODEL_TRANSLATION)
			gm->flags[ref] |=
				V4L2_AV1_GLOBAL_MOTION_FLAG_IS_TRANSLATION;
	}
}

static void fill_film_grain(struct v4l2_ctrl_av1_film_grain *grain,
			    const VADecPictureParameterBufferAV1 *pic)
{
	const VAFilmGrainStructAV1 *fg = &pic->film_grain_info;
	unsigned int i;

	memset(grain, 0, sizeof(*grain));

	if (fg->film_grain_info_fields.bits.apply_grain)
		grain->flags |= V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
	if (fg->film_grain_info_fields.bits.chroma_scaling_from_luma)
		grain->flags |=
			V4L2_AV1_FILM_GRAIN_FLAG_CHROMA_SCALING_FROM_LUMA;
	if (fg->film_grain_info_fields.bits.overlap_flag)
		grain->flags |= V4L2_AV1_FILM_GRAIN_FLAG_OVERLAP;
	if (fg->film_grain_info_fields.bits.clip_to_restricted_range)
		grain->flags |=
			V4L2_AV1_FILM_GRAIN_FLAG_CLIP_TO_RESTRICTED_RANGE;

	grain->grain_seed = fg->grain_seed;
	grain->grain_scaling_minus_8 =
		fg->film_grain_info_fields.bits.grain_scaling_minus_8;
	grain->ar_coeff_lag = fg->film_grain_info_fields.bits.ar_coeff_lag;
	grain->ar_coeff_shift_minus_6 =
		fg->film_grain_info_fields.bits.ar_coeff_shift_minus_6;
	grain->grain_scale_shift =
		fg->film_grain_info_fields.bits.grain_scale_shift;

	grain->num_y_points = fg->num_y_points;
	for (i = 0; i < fg->num_y_points && i < V4L2_AV1_MAX_NUM_Y_POINTS; i++) {
		grain->point_y_value[i] = fg->point_y_value[i];
		grain->point_y_scaling[i] = fg->point_y_scaling[i];
	}

	grain->num_cb_points = fg->num_cb_points;
	for (i = 0; i < fg->num_cb_points && i < V4L2_AV1_MAX_NUM_CB_POINTS;
	     i++) {
		grain->point_cb_value[i] = fg->point_cb_value[i];
		grain->point_cb_scaling[i] = fg->point_cb_scaling[i];
	}

	grain->num_cr_points = fg->num_cr_points;
	for (i = 0; i < fg->num_cr_points && i < V4L2_AV1_MAX_NUM_CR_POINTS;
	     i++) {
		grain->point_cr_value[i] = fg->point_cr_value[i];
		grain->point_cr_scaling[i] = fg->point_cr_scaling[i];
	}

	/* V4L2 stores the AR coefficients biased by 128 so they fit in u8. */
	for (i = 0; i < V4L2_AV1_AR_COEFFS_SIZE; i++) {
		if (i < sizeof(fg->ar_coeffs_y) / sizeof(fg->ar_coeffs_y[0]))
			grain->ar_coeffs_y_plus_128[i] =
				(uint8_t)(fg->ar_coeffs_y[i] + 128);
		if (i < sizeof(fg->ar_coeffs_cb) / sizeof(fg->ar_coeffs_cb[0]))
			grain->ar_coeffs_cb_plus_128[i] =
				(uint8_t)(fg->ar_coeffs_cb[i] + 128);
		if (i < sizeof(fg->ar_coeffs_cr) / sizeof(fg->ar_coeffs_cr[0]))
			grain->ar_coeffs_cr_plus_128[i] =
				(uint8_t)(fg->ar_coeffs_cr[i] + 128);
	}

	grain->cb_mult = fg->cb_mult;
	grain->cb_luma_mult = fg->cb_luma_mult;
	grain->cb_offset = fg->cb_offset;
	grain->cr_mult = fg->cr_mult;
	grain->cr_luma_mult = fg->cr_luma_mult;
	grain->cr_offset = fg->cr_offset;
}

static void fill_frame(struct request_data *driver_data,
		       struct v4l2_ctrl_av1_frame *frame,
		       const VADecPictureParameterBufferAV1 *pic,
		       const VASliceParameterBufferAV1 *tiles,
		       unsigned int num_tiles,
		       unsigned int order_hint_bits)
{
	unsigned int i;

	memset(frame, 0, sizeof(*frame));

	frame->frame_type = pic->pic_info_fields.bits.frame_type;
	frame->order_hint = pic->order_hint;
	frame->primary_ref_frame = pic->primary_ref_frame;
	frame->refresh_frame_flags = 0;
	frame->frame_width_minus_1 = pic->frame_width_minus1;
	frame->frame_height_minus_1 = pic->frame_height_minus1;
	frame->render_width_minus_1 = pic->frame_width_minus1;
	frame->render_height_minus_1 = pic->frame_height_minus1;
	frame->interpolation_filter = pic->interp_filter;
	frame->tx_mode = pic->mode_control_fields.bits.tx_mode;
	frame->superres_denom = pic->pic_info_fields.bits.use_superres ?
				pic->superres_scale_denominator : 8;

	/*
	 * frame_width_minus1 is already the upscaled width: VA documents it
	 * as the resolution after SuperRes, which is what the hardware
	 * scans out.
	 */
	frame->upscaled_width = pic->frame_width_minus1 + 1;

	if (pic->pic_info_fields.bits.show_frame)
		frame->flags |= V4L2_AV1_FRAME_FLAG_SHOW_FRAME;
	if (pic->pic_info_fields.bits.showable_frame)
		frame->flags |= V4L2_AV1_FRAME_FLAG_SHOWABLE_FRAME;
	if (pic->pic_info_fields.bits.error_resilient_mode)
		frame->flags |= V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE;
	if (pic->pic_info_fields.bits.disable_cdf_update)
		frame->flags |= V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE;
	if (pic->pic_info_fields.bits.allow_screen_content_tools)
		frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_SCREEN_CONTENT_TOOLS;
	/*
	 * force_integer_mv is 1 on any intra frame whatever the bitstream
	 * signalled (specification 5.9.2). VA-API passes the signalled value
	 * straight through, so the derivation has to be applied here.
	 */
	if (pic->pic_info_fields.bits.force_integer_mv ||
	    frame->frame_type == V4L2_AV1_KEY_FRAME ||
	    frame->frame_type == V4L2_AV1_INTRA_ONLY_FRAME)
		frame->flags |= V4L2_AV1_FRAME_FLAG_FORCE_INTEGER_MV;
	if (pic->pic_info_fields.bits.allow_intrabc)
		frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC;
	if (pic->pic_info_fields.bits.use_superres)
		frame->flags |= V4L2_AV1_FRAME_FLAG_USE_SUPERRES;
	if (pic->pic_info_fields.bits.allow_high_precision_mv)
		frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV;
	if (pic->pic_info_fields.bits.is_motion_mode_switchable)
		frame->flags |= V4L2_AV1_FRAME_FLAG_IS_MOTION_MODE_SWITCHABLE;
	if (pic->pic_info_fields.bits.use_ref_frame_mvs)
		frame->flags |= V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS;
	if (pic->pic_info_fields.bits.disable_frame_end_update_cdf)
		frame->flags |=
			V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
	if (pic->pic_info_fields.bits.allow_warped_motion)
		frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_WARPED_MOTION;
	if (pic->mode_control_fields.bits.reference_select)
		frame->flags |= V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT;
	if (pic->mode_control_fields.bits.reduced_tx_set_used)
		frame->flags |= V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET;
	if (pic->mode_control_fields.bits.skip_mode_present)
		frame->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT;

	fill_tile_info(&frame->tile_info, pic, tiles, num_tiles);
	fill_quantization(&frame->quantization, pic);
	fill_segmentation(&frame->segmentation, pic);
	fill_loop_filter(&frame->loop_filter, pic);
	fill_cdef(&frame->cdef, pic);
	fill_loop_restoration(&frame->loop_restoration, pic);
	fill_global_motion(&frame->global_motion, pic);

	/*
	 * References are resolved by the OUTPUT-buffer timestamp of the
	 * frame that produced them, indexed through ref_frame_idx[]. Publish
	 * every slot of the DPB, not only the ones this frame reads: the
	 * kernel retires a decoded frame the moment no slot names it.
	 */
	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
		struct object_surface *ref = ref_surface(driver_data, pic, i);

		frame->reference_frame_ts[i] =
			ref != NULL ? timeval_to_ns(&ref->timestamp) : 0;
	}

	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		struct object_surface *ref;
		int va_slot = pic->ref_frame_idx[i];

		frame->ref_frame_idx[i] = pic->ref_frame_idx[i];

		/* VA carries no per-reference order hint, so recover it from
		 * the hint each surface was decoded with. */
		ref = (va_slot >= 0 && va_slot < 8) ?
			ref_surface(driver_data, pic, (unsigned int)va_slot) :
			NULL;
		frame->order_hints[VA_REF_TO_V4L2(i)] =
			ref != NULL ? ref->params.av1.order_hint : 0;
	}

	derive_skip_mode_frame(frame, frame->order_hints, order_hint_bits,
			       pic->order_hint);
}

int av1_set_device_sequence(struct decoder_session *session,
			    struct object_surface *surface_object)
{
	struct v4l2_ctrl_av1_sequence seq;

	if (!surface_object->params.av1.picture_set)
		return -1;

	fill_sequence(&seq, &surface_object->params.av1.picture);
	return v4l2_set_control(session->video_fd, -1,
				V4L2_CID_STATELESS_AV1_SEQUENCE,
				&seq, sizeof(seq));
}

int av1_set_controls(struct request_data *driver_data,
		     struct decoder_session *session,
		     struct object_context *context_object,
		     struct object_surface *surface_object)
{
	const VADecPictureParameterBufferAV1 *pic =
		&surface_object->params.av1.picture;
	struct v4l2_ctrl_av1_tile_group_entry entries[V4L2_AV1_MAX_TILE_COUNT];
	struct v4l2_ctrl_av1_film_grain grain;
	struct v4l2_ctrl_av1_sequence seq;
	struct v4l2_ctrl_av1_frame frame;
	struct v4l2_ext_control ctrls[4];
	unsigned int count, tiles, i, rebase;
	int rc;

	if (!surface_object->params.av1.picture_set)
		return -1;

	tiles = surface_object->params.av1.num_tiles;
	if (tiles == 0) {
		request_log("AV1: frame carried no tile group entry\n");
		return -1;
	}

	/*
	 * A film grain frame is decoded into two surfaces by the VA
	 * contract: the clean frame later frames predict from, and a second
	 * grainy one that is what actually gets displayed. This backend
	 * produces a single output, so the grainy surface would go back to
	 * the caller untouched. Refuse the frame and let the caller fall
	 * back to software rather than return a picture quietly missing its
	 * grain.
	 */
	/*
	 * Intra block copy makes a frame its own reference, and this
	 * hardware does not come back from it: the request is accepted and
	 * then never completes, which times out the decode and leaves the
	 * queues wedged for every frame after it. Refuse the frame instead,
	 * so the caller falls back to software and the decoder stays
	 * usable. Encoders turn it on for screen-like content, which is why
	 * it appears at some resolutions and not others.
	 */
	if (pic->pic_info_fields.bits.allow_intrabc) {
		request_log("AV1: intra block copy is not supported by this decoder\n");
		return -1;
	}

	if (pic->bit_depth_idx > 1) {
		request_log("AV1: %u-bit streams are not supported by this decoder\n",
			    8 + 2 * pic->bit_depth_idx);
		return -1;
	}

	if (pic->bit_depth_idx == 1 &&
	    session->video_format->v4l2_format != V4L2_PIX_FMT_P010) {
		request_log("AV1: 10-bit streams need P010 CAPTURE buffers\n");
		return -1;
	}

	if (pic->bit_depth_idx == 0 &&
	    session->video_format->v4l2_format == V4L2_PIX_FMT_P010) {
		request_log("AV1: 8-bit streams need NV12 CAPTURE buffers\n");
		return -1;
	}

	if (pic->film_grain_info.film_grain_info_fields.bits.apply_grain &&
	    pic->current_display_picture != VA_INVALID_SURFACE &&
	    pic->current_display_picture != pic->current_frame) {
		request_log("AV1: film grain needs a second display surface, unsupported\n");
		return -1;
	}

	fill_sequence(&seq, pic);
	fill_frame(driver_data, &frame, pic,
		   surface_object->params.av1.tiles, tiles,
		   seq.order_hint_bits);
	fill_film_grain(&grain, pic);

	/*
	 * Rebase the coded buffer so the first tile starts within the first
	 * 16 bytes.
	 *
	 * The kernel points the hardware at tile_offset[0] rounded down to
	 * 16 and passes the byte remainder as a start-bit offset, but the
	 * stream length it programmes is the whole payload, not what is
	 * left after that rounding. A client that hands over the frame OBU
	 * rather than the tile group alone therefore has the hardware
	 * reading past the end of its own data: FFmpeg's tile_offset[0] is
	 * 1 and nothing happens, Chrome's is 30 and the decode comes back
	 * with the error flag. Dropping the rounded-down prefix here makes
	 * the offset zero, which takes that arithmetic out of the picture
	 * for every client.
	 */
	rebase = surface_object->params.av1.tiles[0].slice_data_offset & ~0xfu;
	if (rebase > 0 && rebase <= surface_object->slices_size) {
		memmove(surface_object->source_data,
			(unsigned char *)surface_object->source_data + rebase,
			surface_object->slices_size - rebase);
		surface_object->slices_size -= rebase;
	} else {
		rebase = 0;
	}

	memset(entries, 0,
	       tiles * sizeof(struct v4l2_ctrl_av1_tile_group_entry));

	for (i = 0; i < tiles; i++) {
		const VASliceParameterBufferAV1 *tile =
			&surface_object->params.av1.tiles[i];

		entries[i].tile_offset = tile->slice_data_offset - rebase;
		entries[i].tile_size = tile->slice_data_size;
		entries[i].tile_row = tile->tile_row;
		entries[i].tile_col = tile->tile_column;
	}

	/* The frame's state has to land in the request as one update, the
	 * same contract the VP9 backend follows. */
	memset(ctrls, 0, sizeof(ctrls));
	count = 0;

	/*
	 * The sequence header describes the whole stream, so send it only
	 * when it actually changes rather than with every frame, which is
	 * what a stateless client is expected to do.
	 */
	if (memcmp(&seq, &context_object->av1_sequence, sizeof(seq)) != 0 ||
	    !context_object->av1_sequence_sent) {
		/* Once per stream, so a decode that goes wrong later can be
		 * tied to what was actually being asked of the hardware. */
		const VASliceParameterBufferAV1 *first =
			&surface_object->params.av1.tiles[0];
		const VASliceParameterBufferAV1 *last =
			&surface_object->params.av1.tiles[tiles - 1];

		request_log("AV1: sequence profile %u, %u-bit, %ux%u, %u tile%s%s%s\n",
			    seq.seq_profile, seq.bit_depth,
			    frame.frame_width_minus_1 + 1,
			    frame.frame_height_minus_1 + 1,
			    tiles, tiles == 1 ? "" : "s",
			    pic->seq_info_fields.fields.film_grain_params_present ?
				    ", film grain" : "",
			    pic->pic_info_fields.bits.use_superres ?
				    ", superres" : "");
		/* The coded buffer is assembled by appending every slice data
		 * buffer the client sends, while tile offsets are taken from
		 * the client unchanged. If a client splits a frame over more
		 * than one buffer those two disagree, so record both. */
		request_log("AV1: %u data buffer%s, %u bytes; tile[0] off %u len %u, tile[%u] off %u len %u, end %u\n",
			    surface_object->slices_count,
			    surface_object->slices_count == 1 ? "" : "s",
			    surface_object->slices_size,
			    first->slice_data_offset, first->slice_data_size,
			    tiles - 1,
			    last->slice_data_offset, last->slice_data_size,
			    last->slice_data_offset + last->slice_data_size);
		context_object->av1_sequence = seq;
		context_object->av1_sequence_sent = true;
		ctrls[count].id = V4L2_CID_STATELESS_AV1_SEQUENCE;
		ctrls[count].ptr = &seq;
		ctrls[count].size = sizeof(seq);
		count++;
	}

	ctrls[count].id = V4L2_CID_STATELESS_AV1_FRAME;
	ctrls[count].ptr = &frame;
	ctrls[count].size = sizeof(frame);
	count++;

	ctrls[count].id = V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY;
	ctrls[count].ptr = entries;
	ctrls[count].size =
		tiles * sizeof(struct v4l2_ctrl_av1_tile_group_entry);
	count++;

	/*
	 * Send the film grain control on every frame, even when no grain is
	 * applied. The kernel keeps the last value it was given, so leaving
	 * it out does not mean "no grain" — it means "whatever was set
	 * before", which is not the same thing once anything has set it.
	 */
	ctrls[count].id = V4L2_CID_STATELESS_AV1_FILM_GRAIN;
	ctrls[count].ptr = &grain;
	ctrls[count].size = sizeof(grain);
	count++;

	rc = v4l2_set_controls(session->video_fd,
			       surface_object->request_fd, ctrls, count);
	if (rc < 0) {
		request_log("AV1: control submission failed: %d\n", rc);
		return rc;
	}

	/* Remember this frame's hint for whoever references it later. */
	surface_object->params.av1.order_hint = pic->order_hint;

	return 0;
}
