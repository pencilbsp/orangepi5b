/*
 * v4l2-formats -- what does each /dev/videoN actually advertise?
 *
 * The image ships no v4l-utils, and node numbers move between boots, so this
 * walks every node and prints the card name with its OUTPUT and CAPTURE
 * formats. Never key anything off the node number; key off the card name.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void list_formats(int fd, enum v4l2_buf_type type, const char *label)
{
	struct v4l2_fmtdesc f = { .type = type };
	int n = 0;

	printf("    %-8s", label);
	for (f.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++) {
		printf(" %.4s", (char *)&f.pixelformat);
		n++;
	}
	if (!n)
		printf(" (none)");
	printf("\n");
}

int main(void)
{
	char path[64];

	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/video%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		struct v4l2_capability cap = {};
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			printf("%s  card=\"%s\"  driver=\"%s\"\n",
			       path, cap.card, cap.driver);
			__u32 caps = cap.device_caps ? cap.device_caps : cap.capabilities;
			if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
				list_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT");
				list_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE");
			} else if (caps & V4L2_CAP_VIDEO_M2M) {
				list_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "OUTPUT");
				list_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "CAPTURE");
			}
		}
		close(fd);
	}
	return 0;
}
