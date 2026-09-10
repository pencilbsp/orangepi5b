/*
 * VP9 uncompressed-header parser — see vp9_parser.h.
 *
 * Logic is a stripped-down port of the corresponding section of
 * libavcodec/vp9.c (decode_frame_header). Only fields needed to fill
 * V4L2_CID_STATELESS_VP9_FRAME are kept; everything else is skipped.
 *
 * Bit reader is MSB-first to match the VP9 specification.
 */

#include "vp9_parser.h"

#include <string.h>

#define VP9_SYNC_CODE			0x498342u
#define VP9_FRAME_MARKER		0x2u

struct vp9_br {
	const uint8_t	*buf;
	uint32_t	size;	/* bytes */
	uint32_t	pos;	/* bit offset */
	bool		oob;
};

static void br_init(struct vp9_br *br, const uint8_t *buf, uint32_t size)
{
	br->buf = buf;
	br->size = size;
	br->pos = 0;
	br->oob = false;
}

static uint32_t br_u(struct vp9_br *br, unsigned int n)
{
	uint32_t v = 0;
	unsigned int i;

	if (n > 32 || br->oob) {
		br->oob = true;
		return 0;
	}

	for (i = 0; i < n; i++) {
		uint32_t byte_idx = br->pos >> 3;
		unsigned int bit_idx = 7 - (br->pos & 7);

		if (byte_idx >= br->size) {
			br->oob = true;
			return 0;
		}

		v = (v << 1) | ((br->buf[byte_idx] >> bit_idx) & 1u);
		br->pos++;
	}

	return v;
}

static inline bool br_b(struct vp9_br *br)
{
	return br_u(br, 1) ? true : false;
}

/* Signed VP9 value: sign bit follows magnitude bits (su(n)). */
static int32_t br_su(struct vp9_br *br, unsigned int n)
{
	int32_t mag = (int32_t)br_u(br, n);
	bool neg = br_b(br);

	return neg ? -mag : mag;
}

/* read_delta_q(): 1-bit present, then 4-bit signed magnitude+sign (su(4)). */
static int8_t br_delta_q(struct vp9_br *br)
{
	if (!br_b(br))
		return 0;
	return (int8_t)br_su(br, 4);
}

static void skip_color_config(struct vp9_br *br, uint8_t profile)
{
	if (profile >= 2)
		(void)br_b(br); /* ten_or_twelve_bit_depth */

	uint8_t color_space = br_u(br, 3);
	if (color_space != 7 /* SRGB */) {
		(void)br_b(br); /* color_range */
		if (profile == 1 || profile == 3) {
			(void)br_b(br); /* subsampling_x */
			(void)br_b(br); /* subsampling_y */
			(void)br_b(br); /* reserved_zero */
		}
	} else if (profile == 1 || profile == 3) {
		(void)br_b(br); /* reserved_zero */
	}
}

static void skip_frame_size(struct vp9_br *br)
{
	(void)br_u(br, 16); /* frame_width_minus_1 */
	(void)br_u(br, 16); /* frame_height_minus_1 */
	if (br_b(br)) {
		(void)br_u(br, 16); /* render_width_minus_1 */
		(void)br_u(br, 16); /* render_height_minus_1 */
	}
}

static void skip_frame_size_with_refs(struct vp9_br *br)
{
	bool found_ref = false;
	unsigned int i;

	/* VP9 spec § 6.2.5: 1 bit per ref, BREAK on first found_ref==1. */
	for (i = 0; i < 3; i++) {
		if (br_b(br)) {
			found_ref = true;
			break;
		}
	}

	if (!found_ref) {
		(void)br_u(br, 16); /* width_minus_1 */
		(void)br_u(br, 16); /* height_minus_1 */
	}

	if (br_b(br)) {
		(void)br_u(br, 16); /* render_width_minus_1 */
		(void)br_u(br, 16); /* render_height_minus_1 */
	}
}

/* VP9 spec § 6.2.16 — compute min/max log2 tile cols given frame_width. */
static void compute_log2_tile_cols_range(uint32_t frame_width,
					 uint8_t *min_log2,
					 uint8_t *max_log2)
{
	uint32_t mi_cols = (frame_width + 7) >> 3;
	uint32_t sb64_cols = (mi_cols + 7) >> 3;
	uint8_t mn = 0, mx = 1;

	while ((64u << mn) < sb64_cols)
		mn++;
	while ((sb64_cols >> mx) >= 4u)
		mx++;
	if (mx > 0)
		mx -= 1;
	if (mx < mn)
		mx = mn;

	*min_log2 = mn;
	*max_log2 = mx;
}

int vp9_parse_uncompressed_header(const uint8_t *buffer, uint32_t size,
				  uint32_t frame_width,
				  struct vp9_parsed_header *out)
{
	struct vp9_br br;
	uint8_t profile;
	bool keyframe, show_frame, error_resilient, intra_only = false;
	unsigned int i;

	if (!buffer || size < 4 || !out)
		return -1;

	memset(out, 0, sizeof(*out));
	br_init(&br, buffer, size);

	/* frame_marker(2) */
	if (br_u(&br, 2) != VP9_FRAME_MARKER)
		return -1;

	/* profile_low_bit(1), profile_high_bit(1), [reserved_zero(1) if 3] */
	profile = (uint8_t)br_u(&br, 1);
	profile |= (uint8_t)(br_u(&br, 1) << 1);
	if (profile == 3)
		(void)br_b(&br); /* reserved_zero_bit */

	/* show_existing_frame(1) — if set the rest of the header is absent. */
	if (br_b(&br))
		return -1;

	keyframe = !br_b(&br);
	/* VP9 spec § 6.2: show_frame f(1) — 1=visible, 0=invisible
	 * (alt-ref / synthesized frame). NOT inverted. */
	show_frame = br_b(&br);
	error_resilient = br_b(&br);

	if (keyframe) {
		if (br_u(&br, 24) != VP9_SYNC_CODE)
			return -1;
		skip_color_config(&br, profile);
		skip_frame_size(&br);
	} else {
		intra_only = show_frame ? false : br_b(&br);
		if (!error_resilient)
			(void)br_u(&br, 2); /* reset_frame_context */

		if (intra_only) {
			if (br_u(&br, 24) != VP9_SYNC_CODE)
				return -1;
			if (profile > 0)
				skip_color_config(&br, profile);
			(void)br_u(&br, 8); /* refresh_frame_flags */
			skip_frame_size(&br);
		} else {
			(void)br_u(&br, 8); /* refresh_frame_flags */
			for (i = 0; i < 3; i++) {
				(void)br_u(&br, 3); /* ref_frame_idx */
				(void)br_b(&br);    /* ref_frame_sign_bias */
			}
			skip_frame_size_with_refs(&br);
			(void)br_b(&br); /* allow_high_precision_mv */
			if (br_b(&br) == false) {
				/* interpolation_filter: 2 bits when not switchable */
				(void)br_u(&br, 2);
			}
		}
	}

	if (!error_resilient) {
		(void)br_b(&br); /* refresh_frame_context */
		(void)br_b(&br); /* frame_parallel_decoding_mode */
	}
	(void)br_u(&br, 2); /* frame_context_idx */

	/* loop_filter_params() */
	(void)br_u(&br, 6); /* filter.level */
	(void)br_u(&br, 3); /* filter.sharpness */
	out->lf_delta_enabled = br_b(&br);
	if (out->lf_delta_enabled) {
		out->lf_delta_updated = br_b(&br);
		if (out->lf_delta_updated) {
			for (i = 0; i < 4; i++) {
				if (br_b(&br)) {
					out->lf_ref_delta_updated[i] = true;
					out->lf_ref_deltas[i] =
						(int8_t)br_su(&br, 6);
				}
			}
			for (i = 0; i < 2; i++) {
				if (br_b(&br)) {
					out->lf_mode_delta_updated[i] = true;
					out->lf_mode_deltas[i] =
						(int8_t)br_su(&br, 6);
				}
			}
		}
	}

	/* quantization_params() */
	out->base_q_idx = (uint8_t)br_u(&br, 8);
	out->delta_q_y_dc  = br_delta_q(&br);
	out->delta_q_uv_dc = br_delta_q(&br);
	out->delta_q_uv_ac = br_delta_q(&br);
	out->lossless = out->base_q_idx == 0 && out->delta_q_y_dc == 0 &&
			out->delta_q_uv_dc == 0 && out->delta_q_uv_ac == 0;

	/* segmentation_params() — § 6.2.11 */
	if (br_b(&br)) { /* segmentation_enabled */
		bool frame_is_intra_or_er = keyframe || intra_only ||
					    error_resilient;
		bool seg_update_data;

		if (!frame_is_intra_or_er) {
			if (br_b(&br)) { /* segmentation_update_map */
				for (i = 0; i < 7; i++)
					if (br_b(&br))
						(void)br_u(&br, 8); /* tree_probs[i] */
				if (br_b(&br)) /* temporal_update */
					for (i = 0; i < 3; i++)
						if (br_b(&br))
							(void)br_u(&br, 8);
			}
			seg_update_data = br_b(&br);
		} else {
			/* implicit: update_map=1, temporal_update=0,
			 * update_data=1. No bits consumed. */
			seg_update_data = true;
		}

		if (seg_update_data) {
			out->seg_update_data = true;
			out->seg_abs_or_delta = br_b(&br);
			for (i = 0; i < 8; i++) {
				out->seg_feat[i].q_enabled = br_b(&br);
				if (out->seg_feat[i].q_enabled)
					out->seg_feat[i].q_val =
						(int16_t)br_su(&br, 8);
				out->seg_feat[i].lf_enabled = br_b(&br);
				if (out->seg_feat[i].lf_enabled)
					out->seg_feat[i].lf_val =
						(int16_t)br_su(&br, 6);
				out->seg_feat[i].ref_enabled = br_b(&br);
				if (out->seg_feat[i].ref_enabled)
					out->seg_feat[i].ref_val =
						(uint8_t)br_u(&br, 2);
				out->seg_feat[i].skip_enabled = br_b(&br);
			}
		}
	}

	/* tile_info() — § 6.2.16 */
	{
		uint8_t min_log2, max_log2;
		uint8_t cols, rows;

		compute_log2_tile_cols_range(frame_width, &min_log2, &max_log2);
		cols = min_log2;
		while (cols < max_log2) {
			if (br_b(&br))
				cols++;
			else
				break;
		}
		rows = (uint8_t)br_u(&br, 1);
		if (rows)
			rows += (uint8_t)br_u(&br, 1);

		out->tile_cols_log2 = cols;
		out->tile_rows_log2 = rows;
	}

	/* header_size_in_bytes — § 6.2 last field before trailing_bits(). */
	out->first_partition_size = (uint16_t)br_u(&br, 16);

	return br.oob ? -1 : 0;
}
