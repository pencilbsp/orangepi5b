/*
 * VP9 stateless backend for rk3588 rkvdec via V4L2 request API.
 *
 * Bridges VA-API VADecPictureParameterBufferVP9 + VASegmentParameterVP9 +
 * VASliceParameterBufferVP9 to the upstream V4L2 stateless VP9 controls
 * V4L2_CID_STATELESS_VP9_FRAME (struct v4l2_ctrl_vp9_frame) and, when
 * advertised by the driver, V4L2_CID_STATELESS_VP9_COMPRESSED_HDR.
 *
 * VA-API deliberately hides a handful of fields the kernel uapi needs
 * (base_q_idx + 4 delta_q, lf ref/mode deltas, per-segment feature
 * data); they're recovered from the bitstream by vp9_parser.c.
 *
 * Reference timestamps are looked up against active references stored
 * on this driver's object_surface objects — see ref_timestamp_for().
 *
 * VP9 probability updates are parsed from the compressed header and submitted
 * together with the frame control. The kernel consumes both as one state
 * transition; sending either one alone makes entropy state drift on inter
 * frames.
 */

#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include <va/va.h>
#include <va/va_dec_vp9.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "config.h"
#include "context.h"
#include "request.h"
#include "session.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "vp9.h"
#include "vp9_parser.h"
#include "vpx_rac.h"

/*
 * VP9 compressed-header parser — minimal port of
 * libavcodec/v4l2_request_vp9.c::fill_compressed_hdr (FFmpeg, LGPL-2.1+).
 * Adapted to consume VA-API VP9 state rather than FFmpeg's parser state.
 */

#define V4L2_VP9_TX_MODE_ALLOW_32X32_LOCAL	3

/* Differential forward probability update, VP9 spec § 6.3.5. */
static int vp9_read_prob_delta(struct vpx_rac *c)
{
	static const uint8_t inv_map_table[255] = {
		  7,  20,  33,  46,  59,  72,  85,  98, 111, 124, 137, 150,
		163, 176, 189, 202, 215, 228, 241, 254,   1,   2,   3,   4,
		  5,   6,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,
		 18,  19,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
		 31,  32,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
		 44,  45,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
		 57,  58,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
		 70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,
		 83,  84,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
		 96,  97,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108,
		109, 110, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
		122, 123, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
		135, 136, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147,
		148, 149, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160,
		161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173,
		174, 175, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186,
		187, 188, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199,
		200, 201, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212,
		213, 214, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225,
		226, 227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238,
		239, 240, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251,
		252, 253, 253,
	};
	int d;

	if (!vp89_rac_get(c)) {
		d = vp89_rac_get_uint(c, 4) + 0;
	} else if (!vp89_rac_get(c)) {
		d = vp89_rac_get_uint(c, 4) + 16;
	} else if (!vp89_rac_get(c)) {
		d = vp89_rac_get_uint(c, 5) + 32;
	} else {
		d = vp89_rac_get_uint(c, 7);
		if (d >= 65)
			d = (d << 1) - 65 + vp89_rac_get(c);
		d += 64;
		if (d >= (int)sizeof(inv_map_table))
			return 0;
	}
	return inv_map_table[d];
}

/*
 * Parse VP9 compressed_header() into struct v4l2_ctrl_vp9_compressed_hdr.
 * Returns 0 on success, -1 on parse failure (caller should still submit
 * the (mostly-zero) struct so the kernel doesn't WARN_ON).
 */
static int vp9_fill_compressed_hdr(
		struct v4l2_ctrl_vp9_compressed_hdr *ctrl,
		const uint8_t *frame_buffer, uint32_t frame_size,
		uint32_t uncompressed_header_size,
		uint32_t compressed_header_size,
		bool lossless, bool keyframe, bool intra_only,
		bool allow_comp_inter, bool high_precision_mvs,
		bool filter_switchable)
{
	struct vpx_rac c;
	int i, j, k, l, m, n;
	int comppredmode = 0; /* PRED_SINGLEREF */
	int rc;

	if (uncompressed_header_size + compressed_header_size > frame_size)
		return -1;

	rc = vpx_rac_init(&c, frame_buffer + uncompressed_header_size,
			  compressed_header_size);
	if (rc < 0)
		return -1;

	if (vpx_rac_get_prob_branchy(&c, 128)) /* marker bit */
		return -1;

	/* tx_mode */
	if (lossless) {
		ctrl->tx_mode = V4L2_VP9_TX_MODE_ONLY_4X4;
	} else {
		ctrl->tx_mode = (uint8_t)vp89_rac_get_uint(&c, 2);
		if (ctrl->tx_mode == V4L2_VP9_TX_MODE_ALLOW_32X32_LOCAL)
			ctrl->tx_mode += (uint8_t)vp89_rac_get(&c);

		if (ctrl->tx_mode == V4L2_VP9_TX_MODE_SELECT) {
			for (i = 0; i < 2; i++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->tx8[i][0] =
						(uint8_t)vp9_read_prob_delta(&c);
			for (i = 0; i < 2; i++)
				for (j = 0; j < 2; j++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->tx16[i][j] =
							(uint8_t)vp9_read_prob_delta(&c);
			for (i = 0; i < 2; i++)
				for (j = 0; j < 3; j++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->tx32[i][j] =
							(uint8_t)vp9_read_prob_delta(&c);
		}
	}

	/* coef updates — § 6.3.6 */
	for (i = 0; i < 4; i++) {
		if (vp89_rac_get(&c)) {
			for (j = 0; j < 2; j++)
				for (k = 0; k < 2; k++)
					for (l = 0; l < 6; l++)
						for (m = 0; m < 6; m++) {
							if (m >= 3 && l == 0)
								break;
							for (n = 0; n < 3; n++)
								if (vpx_rac_get_prob_branchy(&c, 252))
									ctrl->coef[i][j][k][l][m][n] =
										(uint8_t)vp9_read_prob_delta(&c);
						}
		}
		if (ctrl->tx_mode == i)
			break;
	}

	/* skip_prob */
	for (i = 0; i < 3; i++)
		if (vpx_rac_get_prob_branchy(&c, 252))
			ctrl->skip[i] = (uint8_t)vp9_read_prob_delta(&c);

	if (!keyframe && !intra_only) {
		for (i = 0; i < 7; i++)
			for (j = 0; j < 3; j++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->inter_mode[i][j] =
						(uint8_t)vp9_read_prob_delta(&c);

		/* interp_filter probs — spec § 6.3.10: read ONLY when
		 * interpolation_filter == SWITCHABLE (=4). Reading them
		 * unconditionally consumes bits from the range coder that
		 * spec-conformant parsers (FFmpeg) skip, shifting every
		 * subsequent prob_branchy decision by N bits and producing
		 * bogus y_mode / partition / mv updates that diverge kernel
		 * frame_context state from the encoder's. */
		if (filter_switchable) {
			for (i = 0; i < 4; i++)
				for (j = 0; j < 2; j++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->interp_filter[i][j] =
							(uint8_t)vp9_read_prob_delta(&c);
		}

		for (i = 0; i < 4; i++)
			if (vpx_rac_get_prob_branchy(&c, 252))
				ctrl->is_inter[i] =
					(uint8_t)vp9_read_prob_delta(&c);

		if (allow_comp_inter) {
			comppredmode = vp89_rac_get(&c);
			if (comppredmode)
				comppredmode += vp89_rac_get(&c);
			if (comppredmode == 2 /* PRED_SWITCHABLE */)
				for (i = 0; i < 5; i++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->comp_mode[i] =
							(uint8_t)vp9_read_prob_delta(&c);
		}

		if (comppredmode != 1 /* PRED_COMPREF */) {
			for (i = 0; i < 5; i++) {
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->single_ref[i][0] =
						(uint8_t)vp9_read_prob_delta(&c);
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->single_ref[i][1] =
						(uint8_t)vp9_read_prob_delta(&c);
			}
		}

		if (comppredmode != 0 /* PRED_SINGLEREF */) {
			for (i = 0; i < 5; i++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->comp_ref[i] =
						(uint8_t)vp9_read_prob_delta(&c);
		}

		for (i = 0; i < 4; i++)
			for (j = 0; j < 9; j++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->y_mode[i][j] =
						(uint8_t)vp9_read_prob_delta(&c);

		for (i = 0; i < 4; i++)
			for (j = 0; j < 4; j++)
				for (k = 0; k < 3; k++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->partition[(i * 4) + j][k] =
							(uint8_t)vp9_read_prob_delta(&c);

		/* mv probs */
		for (i = 0; i < 3; i++)
			if (vpx_rac_get_prob_branchy(&c, 252))
				ctrl->mv.joint[i] =
					(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);

		for (i = 0; i < 2; i++) {
			if (vpx_rac_get_prob_branchy(&c, 252))
				ctrl->mv.sign[i] =
					(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);

			for (j = 0; j < 10; j++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->mv.classes[i][j] =
						(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);

			if (vpx_rac_get_prob_branchy(&c, 252))
				ctrl->mv.class0_bit[i] =
					(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);

			for (j = 0; j < 10; j++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->mv.bits[i][j] =
						(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);
		}

		for (i = 0; i < 2; i++) {
			for (j = 0; j < 2; j++)
				for (k = 0; k < 3; k++)
					if (vpx_rac_get_prob_branchy(&c, 252))
						ctrl->mv.class0_fr[i][j][k] =
							(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);

			for (j = 0; j < 3; j++)
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->mv.fr[i][j] =
						(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);
		}

		if (high_precision_mvs) {
			for (i = 0; i < 2; i++) {
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->mv.class0_hp[i] =
						(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);
				if (vpx_rac_get_prob_branchy(&c, 252))
					ctrl->mv.hp[i] =
						(uint8_t)((vp89_rac_get_uint(&c, 7) << 1) | 1);
			}
		}
	}

	/* Caller can use comppredmode for v4l2_ctrl_vp9_frame.reference_mode. */
	return comppredmode;
}

static uint64_t timeval_to_ns(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000000ull +
	       (uint64_t)tv->tv_usec * 1000ull;
}

/*
 * Look up the V4L2 OUTPUT-buffer timestamp of the reference frame
 * stored at @ref_idx in the VA reference_frames[] array. Returns 0 if
 * the referenced surface is not currently tracked — drivers should
 * treat zero as "no reference", which matches kernel rkvdec behaviour.
 */
static uint64_t ref_timestamp_for(struct request_data *driver_data,
				  struct decoder_session *session,
				  const VADecPictureParameterBufferVP9 *pic,
				  unsigned int ref_idx)
{
	struct object_surface *ref_surface;
	VASurfaceID surface_id;

	if (ref_idx >= 8)
		return 0;

	surface_id = pic->reference_frames[ref_idx];
	if (surface_id == VA_INVALID_SURFACE)
		return 0;

	ref_surface = SURFACE(driver_data, surface_id);
	if (ref_surface == NULL || ref_surface->session != session)
		return 0;

	return timeval_to_ns(&ref_surface->timestamp);
}

static const int8_t vp9_default_ref_deltas[4] = { 1, 0, -1, -1 };
static const int8_t vp9_default_mode_deltas[2] = { 0, 0 };

static void vp9_state_reset(struct vp9_persistent_state *st)
{
	unsigned int i;

	memcpy(st->lf_ref_deltas, vp9_default_ref_deltas,
	       sizeof(st->lf_ref_deltas));
	memcpy(st->lf_mode_deltas, vp9_default_mode_deltas,
	       sizeof(st->lf_mode_deltas));
	st->seg_abs_or_delta = false;
	for (i = 0; i < 8; i++) {
		st->seg_feat[i].q_enabled = false;
		st->seg_feat[i].lf_enabled = false;
		st->seg_feat[i].ref_enabled = false;
		st->seg_feat[i].skip_enabled = false;
		st->seg_feat[i].q_val = 0;
		st->seg_feat[i].lf_val = 0;
		st->seg_feat[i].ref_val = 0;
	}
	st->initialised = true;
}

static void fill_loop_filter(struct v4l2_vp9_loop_filter *lf,
			     const VADecPictureParameterBufferVP9 *pic,
			     const struct vp9_parsed_header *parsed,
			     struct vp9_persistent_state *state)
{
	unsigned int i;

	lf->level = pic->filter_level;
	lf->sharpness = pic->sharpness_level;

	/* Per VP9 spec § 6.2.8, lf ref/mode deltas are updated in-place when
	 * delta_update is true and otherwise persist across frames. The
	 * parser only reports the *update*, so we keep the running state
	 * here. */
	if (parsed->lf_delta_updated) {
		for (i = 0; i < 4; i++)
			if (parsed->lf_ref_delta_updated[i])
				state->lf_ref_deltas[i] = parsed->lf_ref_deltas[i];
		for (i = 0; i < 2; i++)
			if (parsed->lf_mode_delta_updated[i])
				state->lf_mode_deltas[i] = parsed->lf_mode_deltas[i];
	}

	for (i = 0; i < 4; i++)
		lf->ref_deltas[i] = state->lf_ref_deltas[i];
	for (i = 0; i < 2; i++)
		lf->mode_deltas[i] = state->lf_mode_deltas[i];

	if (parsed->lf_delta_enabled)
		lf->flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;
	if (parsed->lf_delta_updated)
		lf->flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE;
}

static void fill_quantization(struct v4l2_vp9_quantization *q,
			      const struct vp9_parsed_header *parsed)
{
	q->base_q_idx = parsed->base_q_idx;
	q->delta_q_y_dc = parsed->delta_q_y_dc;
	q->delta_q_uv_dc = parsed->delta_q_uv_dc;
	q->delta_q_uv_ac = parsed->delta_q_uv_ac;
}

static void fill_segmentation(struct v4l2_vp9_segmentation *seg,
			      const VADecPictureParameterBufferVP9 *pic,
			      const struct vp9_parsed_header *parsed,
			      struct vp9_persistent_state *state)
{
	unsigned int i;

	memcpy(seg->tree_probs, pic->mb_segment_tree_probs,
	       sizeof(seg->tree_probs));

	if (pic->pic_fields.bits.segmentation_temporal_update) {
		memcpy(seg->pred_probs, pic->segment_pred_probs,
		       sizeof(seg->pred_probs));
	} else {
		memset(seg->pred_probs, 0xff, sizeof(seg->pred_probs));
	}

	if (pic->pic_fields.bits.segmentation_enabled)
		seg->flags |= V4L2_VP9_SEGMENTATION_FLAG_ENABLED;
	if (pic->pic_fields.bits.segmentation_update_map)
		seg->flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP;
	if (pic->pic_fields.bits.segmentation_temporal_update)
		seg->flags |= V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
	if (parsed->seg_update_data)
		seg->flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA;

	/* Per VP9 spec § 6.2.10, per-segment feature data persists across
	 * frames unless segmentation_update_data is set. Mirror that here
	 * — the parser reports only updates, so we keep the running state
	 * on object_context and emit it on every frame. */
	if (parsed->seg_update_data) {
		state->seg_abs_or_delta = parsed->seg_abs_or_delta;
		for (i = 0; i < 8; i++) {
			state->seg_feat[i].q_enabled =
				parsed->seg_feat[i].q_enabled;
			state->seg_feat[i].lf_enabled =
				parsed->seg_feat[i].lf_enabled;
			state->seg_feat[i].ref_enabled =
				parsed->seg_feat[i].ref_enabled;
			state->seg_feat[i].skip_enabled =
				parsed->seg_feat[i].skip_enabled;
			state->seg_feat[i].q_val = parsed->seg_feat[i].q_val;
			state->seg_feat[i].lf_val = parsed->seg_feat[i].lf_val;
			state->seg_feat[i].ref_val = parsed->seg_feat[i].ref_val;
		}
	}

	if (state->seg_abs_or_delta)
		seg->flags |= V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE;

	for (i = 0; i < 8; i++) {
		if (state->seg_feat[i].q_enabled) {
			seg->feature_enabled[i] |=
				1u << V4L2_VP9_SEG_LVL_ALT_Q;
			seg->feature_data[i][V4L2_VP9_SEG_LVL_ALT_Q] =
				state->seg_feat[i].q_val;
		}
		if (state->seg_feat[i].lf_enabled) {
			seg->feature_enabled[i] |=
				1u << V4L2_VP9_SEG_LVL_ALT_L;
			seg->feature_data[i][V4L2_VP9_SEG_LVL_ALT_L] =
				state->seg_feat[i].lf_val;
		}
		if (state->seg_feat[i].ref_enabled) {
			seg->feature_enabled[i] |=
				1u << V4L2_VP9_SEG_LVL_REF_FRAME;
			seg->feature_data[i][V4L2_VP9_SEG_LVL_REF_FRAME] =
				state->seg_feat[i].ref_val;
		}
		if (state->seg_feat[i].skip_enabled)
			seg->feature_enabled[i] |= 1u << V4L2_VP9_SEG_LVL_SKIP;
	}
}

static void fill_frame_flags(struct v4l2_ctrl_vp9_frame *frame,
			     const VADecPictureParameterBufferVP9 *pic)
{
	const __typeof__(pic->pic_fields.bits) *b = &pic->pic_fields.bits;

	if (b->frame_type == 0)
		frame->flags |= V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	if (b->show_frame)
		frame->flags |= V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
	if (b->error_resilient_mode)
		frame->flags |= V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT;
	if (b->intra_only)
		frame->flags |= V4L2_VP9_FRAME_FLAG_INTRA_ONLY;
	if (b->allow_high_precision_mv)
		frame->flags |= V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV;
	if (b->refresh_frame_context)
		frame->flags |= V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
	if (b->frame_parallel_decoding_mode)
		frame->flags |= V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
	if (b->subsampling_x)
		frame->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING;
	if (b->subsampling_y)
		frame->flags |= V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
	/* VA-API doesn't expose color_range explicitly for VP9; leave the
	 * FULL_SWING flag unset — drivers fall back to studio swing. */
}

static void fill_frame_refs(struct v4l2_ctrl_vp9_frame *frame,
			    struct request_data *driver_data,
			    struct decoder_session *session,
			    const VADecPictureParameterBufferVP9 *pic)
{
	const __typeof__(pic->pic_fields.bits) *b = &pic->pic_fields.bits;

	frame->last_frame_ts =
		ref_timestamp_for(driver_data, session, pic, b->last_ref_frame);
	frame->golden_frame_ts =
		ref_timestamp_for(driver_data, session, pic, b->golden_ref_frame);
	frame->alt_frame_ts =
		ref_timestamp_for(driver_data, session, pic, b->alt_ref_frame);

	if (b->last_ref_frame_sign_bias)
		frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_LAST;
	if (b->golden_ref_frame_sign_bias)
		frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_GOLDEN;
	if (b->alt_ref_frame_sign_bias)
		frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_ALT;
}

static int fill_vp9_frame(struct v4l2_ctrl_vp9_frame *frame,
			  struct request_data *driver_data,
			  struct decoder_session *session,
			  struct object_context *context,
			  struct object_surface *surface,
			  struct vp9_parsed_header *parsed_out)
{
	const VADecPictureParameterBufferVP9 *pic =
		&surface->params.vp9.picture;
	struct object_config *config = CONFIG(driver_data, context->config_id);
	struct vp9_persistent_state *state = &context->vp9_state;
	struct vp9_parsed_header parsed;
	bool profile_matches;
	int rc;

	profile_matches = config != NULL &&
		config->profile == VAProfileVP9Profile0 &&
		pic->profile == 0 && pic->bit_depth == 8;
	if (!profile_matches ||
	    !pic->pic_fields.bits.subsampling_x ||
	    !pic->pic_fields.bits.subsampling_y ||
	    pic->frame_width == 0 || pic->frame_height == 0) {
		request_log("VP9: profile/bit-depth/config mismatch (VA %d, stream %u/%u-bit)\n",
			    config != NULL ? config->profile : VAProfileNone,
			    pic->profile, pic->bit_depth);
		return -EINVAL;
	}

	rc = vp9_parse_uncompressed_header(surface->source_data,
					   surface->slices_size,
					   pic->frame_width,
					   &parsed);
	if (rc < 0) {
		request_log("VP9: uncompressed header parse failed (size %u)\n",
			    surface->slices_size);
		return -EINVAL;
	}

	/* Per spec § 6.2: keyframes and intra-only frames reset the
	 * frame-level persistent state. Error-resilient frames keep the
	 * state but make it independent — easiest is to also reset. */
	if (!state->initialised ||
	    pic->pic_fields.bits.frame_type == 0 ||
	    pic->pic_fields.bits.intra_only ||
	    pic->pic_fields.bits.error_resilient_mode)
		vp9_state_reset(state);

	memset(frame, 0, sizeof(*frame));

	frame->profile = pic->profile;
	frame->bit_depth = pic->bit_depth;
	frame->frame_width_minus_1 = pic->frame_width ? pic->frame_width - 1
						      : 0;
	frame->frame_height_minus_1 = pic->frame_height ? pic->frame_height - 1
							: 0;
	/* VA-API does not expose render size separately. Mirror frame size
	 * — the displayed area falls back to the full frame, matching the
	 * upstream FFmpeg v4l2_request_vp9 backend when no render override
	 * is present in the bitstream. */
	frame->render_width_minus_1 = frame->frame_width_minus_1;
	frame->render_height_minus_1 = frame->frame_height_minus_1;

	/* VP9 bitstream reset_frame_context ∈ {0,1,2,3} (spec § 6.2);
	 * V4L2_VP9_RESET_FRAME_CTX_* maps {0,1} → NONE and {2,3} → {SPEC,ALL}.
	 * Match ffmpeg v4l2_request_vp9.c::fill_frame:
	 *   .reset_frame_context = s->s.h.resetctx > 0 ? s->s.h.resetctx - 1 : 0
	 * VA exposes the raw bitstream value via the 2-bit pic_fields bitfield. */
	{
		unsigned int rc = pic->pic_fields.bits.reset_frame_context;
		frame->reset_frame_context = rc > 0 ? (uint8_t)(rc - 1) : 0;
	}
	/* Kernel rkvdec rewrites fctx_idx=0 internally on
	 * KEY/INTRA_ONLY/ERR_RESILIENT in v4l2_vp9_reset_frame_ctx, so
	 * passing the raw VA value here is safe — the kernel coerces
	 * what it needs. */
	frame->frame_context_idx =
		pic->pic_fields.bits.frame_context_idx;
	/* VA-API VP9 producers (ffmpeg vaapi_vp9.c) emit mcomp_filter_type
	 * already pre-XOR'd to the V4L2-canonical encoding via
	 * `filtermode ^ (filtermode <= 1)` — verified by strace-diff
	 * against ffmpeg's native v4l2_request_vp9 hwaccel. Forward the
	 * VA value unchanged; double-XOR'ing produces wrong filter
	 * coefficients and visible inter-frame artefacts. */
	frame->interpolation_filter =
		(uint8_t)pic->pic_fields.bits.mcomp_filter_type;
	frame->tile_cols_log2 = pic->log2_tile_columns;
	frame->tile_rows_log2 = pic->log2_tile_rows;
	/* reference_mode is the comppredmode value from the compressed
	 * header (VP9 spec § 6.3.13) — neither VA-API nor our uncompressed
	 * parser sees it. Default to SELECT (= per-block) which is the
	 * widest mode and works for compound-capable streams; intra-only
	 * frames must use SINGLE_REFERENCE. */
	frame->reference_mode = (pic->pic_fields.bits.frame_type == 0 ||
				 pic->pic_fields.bits.intra_only)
		? V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE
		: V4L2_VP9_REFERENCE_MODE_SELECT;

	frame->compressed_header_size = parsed.first_partition_size;
	frame->uncompressed_header_size = pic->frame_header_length_in_bytes;

	fill_loop_filter(&frame->lf, pic, &parsed, state);
	fill_quantization(&frame->quant, &parsed);
	fill_segmentation(&frame->seg, pic, &parsed, state);
	fill_frame_flags(frame, pic);
	fill_frame_refs(frame, driver_data, session, pic);

	if (parsed_out)
		*parsed_out = parsed;
	return 0;
}

int vp9_set_controls(struct request_data *driver_data,
		     struct decoder_session *session,
		     struct object_context *context_object,
		     struct object_surface *surface_object)
{
	struct v4l2_ctrl_vp9_frame frame;
	struct v4l2_ctrl_vp9_compressed_hdr compressed_hdr;
	const VADecPictureParameterBufferVP9 *pic =
		&surface_object->params.vp9.picture;
	bool keyframe = pic->pic_fields.bits.frame_type == 0;
	bool intra_only = pic->pic_fields.bits.intra_only;
	bool hp_mv = pic->pic_fields.bits.allow_high_precision_mv;
	bool allow_comp_inter =
		(pic->pic_fields.bits.last_ref_frame_sign_bias !=
		 pic->pic_fields.bits.golden_ref_frame_sign_bias) ||
		(pic->pic_fields.bits.last_ref_frame_sign_bias !=
		 pic->pic_fields.bits.alt_ref_frame_sign_bias);
	int comppredmode;
	int rc;
	struct vp9_parsed_header parsed;

	rc = fill_vp9_frame(&frame, driver_data, session, context_object,
			    surface_object, &parsed);
	if (rc < 0)
		return rc;

	/* Parse compressed header BEFORE emitting any control. The
	 * comppredmode value feeds frame->reference_mode; emitting
	 * V4L2_CID_STATELESS_VP9_FRAME twice in the same request_fd
	 * causes the kernel to apply updates against a partly-stale probability
	 * context. Emit exactly one frame control per request. */
	memset(&compressed_hdr, 0, sizeof(compressed_hdr));
	comppredmode = vp9_fill_compressed_hdr(
		&compressed_hdr,
		surface_object->source_data,
		surface_object->slices_size,
		pic->frame_header_length_in_bytes,
		parsed.first_partition_size,
		frame.quant.base_q_idx == 0 &&
			frame.quant.delta_q_y_dc == 0 &&
			frame.quant.delta_q_uv_dc == 0 &&
			frame.quant.delta_q_uv_ac == 0,
		keyframe, intra_only, allow_comp_inter, hp_mv,
		frame.interpolation_filter ==
			V4L2_VP9_INTERP_FILTER_SWITCHABLE);

	if (comppredmode < 0) {
		request_log("VP9: compressed_hdr parse failed (uhsz=%u chsz=%u sz=%u)\n",
			    pic->frame_header_length_in_bytes,
			    pic->first_partition_size,
			    surface_object->slices_size);
		/* Keep frame.reference_mode at fill_vp9_frame's default;
		 * keyframes will still decode because the kernel resets
		 * frame_context to defaults on KEY/INTRA_ONLY/ERR_RESILIENT. */
	} else if (!keyframe && !intra_only) {
		/* Map the VP9 compound-prediction mode to V4L2:
		 *   0 PRED_SINGLEREF   -> SINGLE_REFERENCE
		 *   1 PRED_COMPREF     -> COMPOUND_REFERENCE
		 *   2 PRED_SWITCHABLE  -> SELECT
		 */
		switch (comppredmode) {
		case 0:
			frame.reference_mode =
				V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE;
			break;
		case 1:
			frame.reference_mode =
				V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE;
			break;
		case 2:
		default:
			frame.reference_mode =
				V4L2_VP9_REFERENCE_MODE_SELECT;
			break;
		}
	}

	/* Atomic submission: emit FRAME + COMPRESSED_HDR in one
	 * VIDIOC_S_EXT_CTRLS. The V4L2 stateless contract requires the
	 * kernel to apply controls inside a request atomically; emitting
	 * them as two separate ioctls splits the state update and causes
	 * the rkvdec probability tables to advance against a partly-stale
	 * FRAME control. */
	{
		struct v4l2_ext_control ctrls[2] = { 0 };

		ctrls[0].id = V4L2_CID_STATELESS_VP9_FRAME;
		ctrls[0].ptr = &frame;
		ctrls[0].size = sizeof(frame);

		ctrls[1].id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR;
		ctrls[1].ptr = &compressed_hdr;
		ctrls[1].size = sizeof(compressed_hdr);

		rc = v4l2_set_controls(session->video_fd,
				       surface_object->request_fd,
				       ctrls, 2);
		if (rc < 0) {
			request_log("VP9: atomic FRAME+COMPRESSED_HDR set failed: %d\n",
				    rc);
			return rc;
		}
	}

	return 0;
}
