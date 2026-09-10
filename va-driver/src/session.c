/*
 * Copyright (C) 2026 Orange Pi 5B image builders
 *
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "request.h"
#include "session.h"
#include "utils.h"

int decoder_session_open(struct request_data *driver_data,
			 struct decoder_session *session)
{
	memset(session, 0, sizeof(*session));
	session->video_fd = -1;
	session->media_fd = -1;

	/*
	 * The device was chosen once, at driver init; a session only reopens
	 * the same nodes so that it gets file handles of its own.
	 */
	session->video_fd = open(driver_data->video_path, O_RDWR | O_NONBLOCK);
	if (session->video_fd < 0) {
		request_log("session: cannot open %s\n", driver_data->video_path);
		return -1;
	}

	session->media_fd = open(driver_data->media_path, O_RDWR | O_NONBLOCK);
	if (session->media_fd < 0) {
		request_log("session: cannot open %s\n", driver_data->media_path);
		close(session->video_fd);
		session->video_fd = -1;
		return -1;
	}

	return 0;
}

void decoder_session_close(struct decoder_session *session)
{
	if (session->media_fd >= 0)
		close(session->media_fd);
	if (session->video_fd >= 0)
		close(session->video_fd);

	session->media_fd = -1;
	session->video_fd = -1;
	session->video_format = NULL;
	session->num_output_buffers = 0;
	session->num_capture_buffers = 0;
	session->next_output_buf = 0;
	session->next_capture_buf = 0;
	session->free_output_count = 0;
	session->free_capture_count = 0;
	session->streaming = false;
}
