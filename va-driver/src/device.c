/*
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
 * Finding the decoder.
 *
 * Never key off a node number. Probe order moves /dev/videoN between boots,
 * and on RK3588S two devices advertise H264_SLICE: rkvdec, and the legacy
 * Hantro VDPU2 block registered as "rockchip,rk3568-vpu-dec". Only rkvdec
 * decodes it correctly -- VDPU2 accepts the stream, returns frames, and every
 * one of them is garbage. Taking the first match worked until the numbering
 * changed, at which point H.264 silently became garbage with no code having
 * been touched. So the devices are ranked, not raced.
 */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/media.h>
#include <linux/videodev2.h>

#include "device.h"
#include "utils.h"
#include "v4l2.h"

/* Higher wins. */
#define RANK_NONE		0
#define RANK_OTHER		1
#define RANK_RKVDEC		2
#define RANK_ENVIRONMENT	3

static int rank_device(int fd, const char *path, const char *wanted_path)
{
	struct v4l2_capability capability;
	bool has_h264, has_hevc;

	if (wanted_path != NULL && strcmp(path, wanted_path) == 0)
		return RANK_ENVIRONMENT;

	memset(&capability, 0, sizeof(capability));
	if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
		return RANK_NONE;

	if (!(capability.capabilities & V4L2_CAP_STREAMING))
		return RANK_NONE;

	has_h264 = v4l2_find_format_any(fd, V4L2_PIX_FMT_H264_SLICE);
	has_hevc = v4l2_find_format_any(fd, V4L2_PIX_FMT_HEVC_SLICE);
	if (!has_h264 && !has_hevc)
		return RANK_NONE;

	if (strstr((const char *)capability.card, "rkvdec") != NULL ||
	    strstr((const char *)capability.driver, "rkvdec") != NULL)
		return RANK_RKVDEC;

	return RANK_OTHER;
}

/*
 * The video and media nodes of one decoder are two views of the same platform
 * device, and bus_info is what says so: both report "platform:fdc38000.video-codec".
 *
 * Matching on that rather than walking sysfs keeps this independent of where
 * the media class happens to be exposed -- /sys/class/media does not exist on
 * this kernel -- and of the numbering, which is assigned independently for the
 * two node types.
 */
static int media_node_for_bus(const char *bus_info, char *out, size_t len)
{
	struct dirent *entry;
	DIR *dir;
	int found = -1;

	dir = opendir("/dev");
	if (dir == NULL)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		struct media_device_info info;
		char path[PATH_MAX];
		int fd;

		if (strncmp(entry->d_name, "media", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		memset(&info, 0, sizeof(info));
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0 &&
		    strcmp(info.bus_info, bus_info) == 0) {
			snprintf(out, len, "%s", path);
			found = 0;
		}
		close(fd);

		if (found == 0)
			break;
	}

	closedir(dir);
	return found;
}

int decoder_device_open(int *video_fd_out, int *media_fd_out,
			char *video_path_out, char *media_path_out,
			size_t path_len)
{
	const char *wanted_video = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	const char *wanted_media = getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH");
	char best_video[PATH_MAX] = "";
	char best_bus[sizeof(((struct v4l2_capability *)0)->bus_info)] = "";
	char media_path[PATH_MAX];
	int best_rank = RANK_NONE;
	struct dirent *entry;
	DIR *dir;
	int video_fd, media_fd;

	dir = opendir("/dev");
	if (dir == NULL)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		int fd, rank;

		if (strncmp(entry->d_name, "video", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		rank = rank_device(fd, path, wanted_video);
		if (getenv("LIBVA_V4L2_REQUEST_DEBUG") != NULL) {
			struct v4l2_capability cap;

			memset(&cap, 0, sizeof(cap));
			ioctl(fd, VIDIOC_QUERYCAP, &cap);
			request_log("device: %s card=\"%s\" rank=%d\n", path,
				    cap.card, rank);
		}
		close(fd);

		if (rank > best_rank) {
			struct v4l2_capability cap;
			int probe = open(path, O_RDWR | O_NONBLOCK);

			memset(&cap, 0, sizeof(cap));
			if (probe >= 0) {
				ioctl(probe, VIDIOC_QUERYCAP, &cap);
				close(probe);
			}

			best_rank = rank;
			snprintf(best_video, sizeof(best_video), "%s", path);
			snprintf(best_bus, sizeof(best_bus), "%s",
				 (const char *)cap.bus_info);
		}
	}
	closedir(dir);

	if (best_rank == RANK_NONE) {
		request_log("device: no V4L2 stateless H.264/HEVC decoder found\n");
		return -1;
	}

	video_fd = open(best_video, O_RDWR | O_NONBLOCK);
	if (video_fd < 0)
		return -1;

	if (wanted_media != NULL) {
		snprintf(media_path, sizeof(media_path), "%s", wanted_media);
	} else if (media_node_for_bus(best_bus, media_path,
				      sizeof(media_path)) < 0) {
		request_log("device: no media node for %s\n", best_video);
		close(video_fd);
		return -1;
	}

	media_fd = open(media_path, O_RDWR | O_NONBLOCK);
	if (media_fd < 0) {
		request_log("device: cannot open %s\n", media_path);
		close(video_fd);
		return -1;
	}

	request_log("device: using %s with %s\n", best_video, media_path);

	*video_fd_out = video_fd;
	*media_fd_out = media_fd;
	snprintf(video_path_out, path_len, "%s", best_video);
	snprintf(media_path_out, path_len, "%s", media_path);
	return 0;
}
