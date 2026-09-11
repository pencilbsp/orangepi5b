/*
 * Copyright (C) 2026 Orange Pi 5B image builders
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <stddef.h>

/*
 * Pick the V4L2 stateless decoder to drive and open its video and media nodes.
 * Returns 0 and fills both descriptors, or -1.
 */
int decoder_device_open(int *video_fd_out, int *media_fd_out,
			char *video_path_out, char *media_path_out,
			size_t path_len);

/*
 * The RK3588 AV1 decoder is a separate Hantro VPU981 mem2mem device rather
 * than another backend of rkvdec. Discover it independently so an AV1 VA
 * context opens the node that actually owns V4L2_PIX_FMT_AV1_FRAME.
 */
int av1_decoder_device_open(int *video_fd_out, int *media_fd_out,
			    char *video_path_out, char *media_path_out,
			    size_t path_len);

/*
 * Find the stateful H.264 encoder, if the kernel has one. Unlike the decoder
 * this is optional: a build without the RKVENC driver still decodes, and the
 * driver simply does not advertise an encode entrypoint.
 *
 * Returns 0 and fills @video_path_out, or -1 when no encoder is present.
 */
int encoder_device_find(char *video_path_out, size_t path_len);

#endif
