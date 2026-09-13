/*
 * va-info -- what does the VA driver actually advertise, and does a context
 * come up? The image ships no vainfo, and node numbers move between boots.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_str.h>

static const char *entrypoint_name(VAEntrypoint e)
{
	switch (e) {
	case VAEntrypointVLD:		return "VLD";
	case VAEntrypointEncSlice:	return "EncSlice";
	case VAEntrypointEncPicture:	return "EncPicture";
	case VAEntrypointEncSliceLP:	return "EncSliceLP";
	case VAEntrypointVideoProc:	return "VideoProc";
	default:			return "other";
	}
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd = open(node, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed\n", node);
		return 1;
	}

	VADisplay dpy = vaGetDisplayDRM(fd);
	if (dpy == NULL) {
		fprintf(stderr, "vaGetDisplayDRM failed\n");
		return 1;
	}

	int major, minor;
	VAStatus st = vaInitialize(dpy, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize: %s\n", vaErrorStr(st));
		return 2;
	}
	printf("VA-API %d.%d, vendor: %s\n", major, minor, vaQueryVendorString(dpy));

	int max_profiles = vaMaxNumProfiles(dpy);
	VAProfile *profiles = calloc(max_profiles, sizeof(*profiles));
	int n_profiles = 0;
	vaQueryConfigProfiles(dpy, profiles, &n_profiles);

	int max_ep = vaMaxNumEntrypoints(dpy);
	VAEntrypoint *eps = calloc(max_ep, sizeof(*eps));

	for (int i = 0; i < n_profiles; i++) {
		int n_ep = 0;
		vaQueryConfigEntrypoints(dpy, profiles[i], eps, &n_ep);
		for (int j = 0; j < n_ep; j++)
			printf("  %-38s %s\n", vaProfileStr(profiles[i]),
			       entrypoint_name(eps[j]));
	}

	/* Creating a config and a context is where a driver usually falls over. */
	for (int i = 0; i < n_profiles; i++) {
		VAConfigID cfg;
		VAContextID ctx;
		VASurfaceID surfaces[4];

		st = vaCreateConfig(dpy, profiles[i], VAEntrypointVLD, NULL, 0, &cfg);
		if (st != VA_STATUS_SUCCESS) {
			printf("  %-38s config: %s\n", vaProfileStr(profiles[i]),
			       vaErrorStr(st));
			continue;
		}
		st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080,
				      surfaces, 4, NULL, 0);
		if (st != VA_STATUS_SUCCESS) {
			printf("  %-38s surfaces: %s\n", vaProfileStr(profiles[i]),
			       vaErrorStr(st));
			vaDestroyConfig(dpy, cfg);
			continue;
		}
		st = vaCreateContext(dpy, cfg, 1920, 1080, VA_PROGRESSIVE,
				     surfaces, 4, &ctx);
		printf("  %-38s context 1920x1080: %s\n", vaProfileStr(profiles[i]),
		       st == VA_STATUS_SUCCESS ? "OK" : vaErrorStr(st));
		if (st == VA_STATUS_SUCCESS)
			vaDestroyContext(dpy, ctx);
		vaDestroySurfaces(dpy, surfaces, 4);
		vaDestroyConfig(dpy, cfg);
	}

	vaTerminate(dpy);
	close(fd);
	return 0;
}
