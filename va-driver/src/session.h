/*
 * Copyright (C) 2026 Orange Pi 5B image builders
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _SESSION_H_
#define _SESSION_H_

#include <stdbool.h>

struct request_data;
struct video_format;

/* Upper bound of the decoder REQBUFS pool and its recycled-index stacks. */
#define DECODER_SESSION_MAX_BUFFERS	64u

/*
 * Everything describing one decode in progress.
 *
 * v4l2-mem2mem keys a decode session off the open file handle: REQBUFS, the
 * queues and every job belong to whichever handle asked for them. Contexts
 * sharing one handle therefore reprogram each other's queues -- the second
 * stream to start pulls the first one's buffers out from under it, which shows
 * up as "CAPTURE pool exhausted at 0 buffers" on a stream that was working.
 *
 * So each context opens the decoder itself and owns everything below.
 */
struct decoder_session {
	int video_fd;
	int media_fd;

	struct video_format *video_format;

	unsigned int num_output_buffers;
	unsigned int num_capture_buffers;
	unsigned int next_output_buf;
	unsigned int next_capture_buf;

	/*
	 * vaDestroySurfaces returns the V4L2 indices owned by that surface.
	 * Chrome creates and destroys surfaces continuously, so monotonic-only
	 * next_* counters would exhaust a finite pool even when few surfaces are
	 * alive at once.
	 */
	unsigned int free_output[DECODER_SESSION_MAX_BUFFERS];
	unsigned int free_capture[DECODER_SESSION_MAX_BUFFERS];
	unsigned int free_output_count;
	unsigned int free_capture_count;

	/*
	 * What the queues are currently programmed for. On rkvdec the OUTPUT
	 * pixelformat is what selects the hardware codec backend, so a context
	 * created for a different codec -- or at a different resolution, which
	 * is how adaptive streaming changes quality -- must reprogram them.
	 */
	unsigned int programmed_pixelformat;
	int programmed_profile;
	int programmed_width;
	int programmed_height;

	/*
	 * STREAMON is deferred until the first frame's sequence header is
	 * known: rkvdec validates the SPS in its start hook, and for HEVC
	 * rkvdec_hevc_start() rejects chroma_format_idc == 0.
	 */
	bool streaming;
	bool av1_device;
};

int decoder_session_open(struct request_data *driver_data,
			 struct decoder_session *session, int profile);
void decoder_session_close(struct decoder_session *session);

#endif
