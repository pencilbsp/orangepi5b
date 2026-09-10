/*
 * VP9 uncompressed-header parser — fields not exposed by VA-API.
 *
 * VA-API VADecPictureParameterBufferVP9 deliberately omits the
 * quantization base + deltas and the loop-filter / segmentation deltas
 * that drivers need to populate V4L2_CID_STATELESS_VP9_FRAME. This
 * minimal parser walks the bitstream from the start of the frame to
 * fill in the gaps. See VP9 bitstream specification § 6.2
 * (uncompressed_header).
 *
 * Only the fields required for V4L2 stateless decode are extracted;
 * resolution, refs, and colour space — already present in the VA
 * params — are skipped.
 */

#ifndef _VP9_PARSER_H_
#define _VP9_PARSER_H_

#include <stdbool.h>
#include <stdint.h>

struct vp9_parsed_header {
	/* Loop filter (VA exposes filter_level + sharpness_level only). */
	bool		lf_delta_enabled;
	bool		lf_delta_updated;
	bool		lf_ref_delta_updated[4];
	bool		lf_mode_delta_updated[2];
	int8_t		lf_ref_deltas[4];
	int8_t		lf_mode_deltas[2];

	/* Quantization — VA does not expose any of these. */
	uint8_t		base_q_idx;
	int8_t		delta_q_y_dc;
	int8_t		delta_q_uv_dc;
	int8_t		delta_q_uv_ac;
	bool		lossless;

	/* Segmentation per-feature delta/abs values (VA only gives final
	 * per-segment quant scales). */
	bool		seg_update_data;
	bool		seg_abs_or_delta;
	struct {
		bool	q_enabled;
		bool	lf_enabled;
		bool	ref_enabled;
		bool	skip_enabled;
		int16_t	q_val;
		int16_t	lf_val;
		uint8_t	ref_val;
	} seg_feat[8];

	/* tile_info() — VA exposes log2_tile_columns/rows but not the
	 * header_size_in_bytes that immediately follows. */
	uint8_t		tile_cols_log2;
	uint8_t		tile_rows_log2;

	/* header_size_in_bytes — VP9 spec § 6.2 uncompressed_header() final
	 * field, immediately before trailing_bits(). VA-API VADecPicture-
	 * ParameterBufferVP9 exposes the same value as `first_partition_size`
	 * but the field is brittle across VA-API producers — parse it here so
	 * the driver does not depend on VA reporting it. Used as
	 * compressed_header_size in V4L2_CID_STATELESS_VP9_COMPRESSED_HDR. */
	uint16_t	first_partition_size;
};

/*
 * Parse the VP9 uncompressed header at @buffer (@size bytes available).
 * On success returns 0 and populates @out; on failure returns -1 and
 * leaves @out untouched.
 *
 * The parser is robust against truncated input — it returns -1 if any
 * required field would run past the end of the buffer.
 */
int vp9_parse_uncompressed_header(const uint8_t *buffer, uint32_t size,
				  uint32_t frame_width,
				  struct vp9_parsed_header *out);

#endif /* _VP9_PARSER_H_ */
