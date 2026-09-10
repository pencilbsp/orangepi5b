/*
 * VP9 stateless backend for rk3588 rkvdec via V4L2 request API.
 *
 * Bridges VA-API VADecPictureParameterBufferVP9 + VASliceParameterBufferVP9
 * to V4L2_CID_STATELESS_VP9_FRAME (struct v4l2_ctrl_vp9_frame).
 * V4L2_CID_STATELESS_VP9_COMPRESSED_HDR is also emitted because rkvdec
 * does not parse the compressed header itself.
 */

#ifndef _VP9_H_
#define _VP9_H_

#include <stdbool.h>
#include <stdint.h>

struct object_context;
struct object_surface;
struct request_data;
struct decoder_session;

/* Fields which VP9 inherits when an uncompressed header omits an update. */
struct vp9_persistent_state {
	bool initialised;
	int8_t lf_ref_deltas[4];
	int8_t lf_mode_deltas[2];
	bool seg_abs_or_delta;
	struct {
		bool q_enabled;
		bool lf_enabled;
		bool ref_enabled;
		bool skip_enabled;
		int16_t q_val;
		int16_t lf_val;
		uint8_t ref_val;
	} seg_feat[8];
};

int vp9_set_controls(struct request_data *driver_data,
		     struct decoder_session *session,
		     struct object_context *context_object,
		     struct object_surface *surface_object);

#endif /* _VP9_H_ */
