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

#endif
