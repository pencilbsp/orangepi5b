/* SPDX-License-Identifier: MIT */
/* Query the Mesa/GBM dma-buf formats used by Chrome's EGL import path. */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct format_probe {
	uint32_t fourcc;
	const char *name;
};

static const struct format_probe probes[] = {
	{ DRM_FORMAT_NV12, "NV12" },
	{ DRM_FORMAT_NV15, "NV15" },
	{ DRM_FORMAT_P010, "P010" },
};

static bool list_has_format(const EGLint *formats, EGLint count,
			    uint32_t fourcc)
{
	EGLint i;

	for (i = 0; i < count; i++)
		if ((uint32_t)formats[i] == fourcc)
			return true;
	return false;
}

static void probe_gbm(struct gbm_device *gbm, const struct format_probe *probe)
{
	static const struct {
		uint32_t flags;
		const char *name;
	} usages[] = {
		{ GBM_BO_USE_LINEAR, "linear" },
		{ GBM_BO_USE_RENDERING, "rendering" },
		{ GBM_BO_USE_SCANOUT, "scanout" },
	};
	struct gbm_bo *bo;
	unsigned int i;

	printf("GBM %-4s:", probe->name);
	for (i = 0; i < sizeof(usages) / sizeof(usages[0]); i++)
		printf(" %s=%s", usages[i].name,
		       gbm_device_is_format_supported(gbm, probe->fourcc,
					      usages[i].flags) ? "yes" : "no");

	errno = 0;
	bo = gbm_bo_create(gbm, 128, 64, probe->fourcc,
			   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (bo != NULL) {
		printf(" create=yes planes=%d", gbm_bo_get_plane_count(bo));
		gbm_bo_destroy(bo);
	} else {
		printf(" create=no errno=%d(%s)", errno, strerror(errno));
	}
	putchar('\n');
}

static int allocate_dmabuf(size_t size)
{
	struct dma_heap_allocation_data allocation = {
		.len = size,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	int heap;

	heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	if (heap < 0)
		return -1;
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
		close(heap);
		return -1;
	}
	close(heap);
	return allocation.fd;
}

static int dmabuf_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };
	int rc;

	do {
		rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (rc < 0 && (errno == EINTR || errno == EAGAIN));
	return rc;
}

static bool fill_constant_image(int fd, size_t size, uint32_t fourcc,
				EGLint width, EGLint height, EGLint pitch)
{
	uint8_t *data;
	EGLint row;

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
		return false;
	if (dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) {
		munmap(data, size);
		return false;
	}

	memset(data, 0, size);
	if (fourcc == DRM_FORMAT_NV12) {
		memset(data, 128, size);
	} else if (fourcc == DRM_FORMAT_P010) {
		uint16_t *pixels = (uint16_t *)data;
		size_t count = size / sizeof(*pixels);
		size_t i;

		for (i = 0; i < count; i++)
			pixels[i] = 512u << 6;
	} else if (fourcc == DRM_FORMAT_NV15) {
		/* Four identical 10-bit samples packed little-endian into 5 bytes. */
		static const uint8_t mid_gray[5] = { 0x00, 0x02, 0x08, 0x20, 0x80 };
		const EGLint chroma_offset = pitch * height;
		const EGLint visible_bytes = ((width + 3) / 4) * 5;
		EGLint plane;

		for (plane = 0; plane < 2; plane++) {
			uint8_t *base = data + (plane == 0 ? 0 : chroma_offset);
			EGLint rows = plane == 0 ? height : height / 2;

			for (row = 0; row < rows; row++) {
				EGLint byte;

				for (byte = 0; byte < visible_bytes; byte += 5)
					memcpy(base + (size_t)row * pitch + byte,
					       mid_gray, sizeof(mid_gray));
			}
		}
	}

	(void)dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
	munmap(data, size);
	return true;
}

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = GL_FALSE;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		GLsizei length = 0;

		glGetShaderInfoLog(shader, sizeof(log), &length, log);
		fprintf(stderr, "shader compile failed: %.*s\n", (int)length, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool sample_external_image(EGLImageKHR image, uint8_t rgba[4])
{
	static const char vertex_source[] =
		"attribute vec2 position;\n"
		"varying vec2 texcoord;\n"
		"void main() {\n"
		"  gl_Position = vec4(position, 0.0, 1.0);\n"
		"  texcoord = position * 0.5 + 0.5;\n"
		"}\n";
	static const char fragment_source[] =
		"#extension GL_OES_EGL_image_external : require\n"
		"precision mediump float;\n"
		"uniform samplerExternalOES image;\n"
		"varying vec2 texcoord;\n"
		"void main() { gl_FragColor = texture2D(image, texcoord); }\n";
	static const GLfloat vertices[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
	};
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
	GLuint vertex_shader = 0;
	GLuint fragment_shader = 0;
	GLuint program = 0;
	GLuint texture = 0;
	GLuint output_texture = 0;
	GLuint framebuffer = 0;
	GLint position;
	GLint linked = GL_FALSE;
	bool ok = false;

	image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (image_target == NULL)
		goto cleanup;

	vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
	fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
	if (vertex_shader == 0 || fragment_shader == 0)
		goto cleanup;
	program = glCreateProgram();
	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
		goto cleanup;

	glGenTextures(1, &texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	image_target(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
	if (glGetError() != GL_NO_ERROR)
		goto cleanup;

	glGenTextures(1, &output_texture);
	glBindTexture(GL_TEXTURE_2D, output_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, output_texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE)
		goto cleanup;

	glViewport(0, 0, 1, 1);
	glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "image"), 0);
	position = glGetAttribLocation(program, "position");
	glEnableVertexAttribArray((GLuint)position);
	glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0,
			      vertices);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	ok = glGetError() == GL_NO_ERROR;

cleanup:
	if (framebuffer != 0)
		glDeleteFramebuffers(1, &framebuffer);
	if (output_texture != 0)
		glDeleteTextures(1, &output_texture);
	if (texture != 0)
		glDeleteTextures(1, &texture);
	if (program != 0)
		glDeleteProgram(program);
	if (vertex_shader != 0)
		glDeleteShader(vertex_shader);
	if (fragment_shader != 0)
		glDeleteShader(fragment_shader);
	return ok;
}

static void probe_actual_import(EGLDisplay display,
				const struct format_probe *probe)
{
	PFNEGLCREATEIMAGEKHRPROC create_image =
		(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNEGLDESTROYIMAGEKHRPROC destroy_image =
		(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	const EGLint width = 128;
	const EGLint height = 64;
	const EGLint pitch = probe->fourcc == DRM_FORMAT_NV15 ? 192 : 256;
	const EGLint chroma_offset = pitch * height;
	const size_t size = (size_t)chroma_offset * 3 / 2;
	EGLImageKHR image;
	uint8_t rgba[4] = { 0, 0, 0, 0 };
	EGLint attributes[32];
	unsigned int n = 0;
	int fd;

	if (create_image == NULL || destroy_image == NULL) {
		printf("EGL %-4s: actual_import=unavailable\n", probe->name);
		return;
	}
	fd = allocate_dmabuf(size);
	if (fd < 0) {
		printf("EGL %-4s: actual_import=no dma_heap=%s\n", probe->name,
		       strerror(errno));
		return;
	}
	if (!fill_constant_image(fd, size, probe->fourcc, width, height, pitch)) {
		printf("EGL %-4s: actual_import=no fill=%s\n", probe->name,
		       strerror(errno));
		close(fd);
		return;
	}

#define ADD_ATTRIBUTE(name, value) do { \
	attributes[n++] = (name); \
	attributes[n++] = (value); \
} while (0)
	ADD_ATTRIBUTE(EGL_WIDTH, width);
	ADD_ATTRIBUTE(EGL_HEIGHT, height);
	ADD_ATTRIBUTE(EGL_LINUX_DRM_FOURCC_EXT, (EGLint)probe->fourcc);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE0_FD_EXT, fd);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE1_FD_EXT, fd);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE1_OFFSET_EXT, chroma_offset);
	ADD_ATTRIBUTE(EGL_DMA_BUF_PLANE1_PITCH_EXT, pitch);
	attributes[n] = EGL_NONE;
#undef ADD_ATTRIBUTE

	image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
			     NULL, attributes);
	if (image == EGL_NO_IMAGE_KHR) {
		printf("EGL %-4s: actual_import=no error=0x%x\n", probe->name,
		       eglGetError());
	} else {
		printf("EGL %-4s: actual_import=yes pitch=%d offset=%d",
		       probe->name, pitch, chroma_offset);
		if (sample_external_image(image, rgba))
			printf(" external_sample=yes rgba=%u,%u,%u,%u",
			       rgba[0], rgba[1], rgba[2], rgba[3]);
		else
			printf(" external_sample=no gl_error=0x%x", glGetError());
		putchar('\n');
		destroy_image(display, image);
	}
	close(fd);
}

int main(int argc, char **argv)
{
	PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	struct gbm_device *gbm = NULL;
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext egl_context = EGL_NO_CONTEXT;
	EGLConfig egl_config;
	EGLint configs = 0;
	const EGLint config_attributes[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};
	const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};
	EGLint *formats = NULL;
	EGLint format_count = 0;
	EGLint major = 0;
	EGLint minor = 0;
	unsigned int i;
	int fd = -1;
	int status = EXIT_FAILURE;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		goto cleanup;
	}
	gbm = gbm_create_device(fd);
	if (gbm == NULL) {
		fprintf(stderr, "gbm_create_device failed\n");
		goto cleanup;
	}
	display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		fprintf(stderr, "EGL GBM initialization failed: 0x%x\n",
			eglGetError());
		goto cleanup;
	}

	printf("EGL %d.%d vendor=%s\n", major, minor,
	       eglQueryString(display, EGL_VENDOR));
	printf("GBM backend=%s\n", gbm_device_get_backend_name(gbm));
	if (!eglBindAPI(EGL_OPENGL_ES_API) ||
	    !eglChooseConfig(display, config_attributes, &egl_config, 1, &configs) ||
	    configs != 1) {
		fprintf(stderr, "EGL ES2 config unavailable: 0x%x\n", eglGetError());
		goto cleanup;
	}
	egl_context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT,
				       context_attributes);
	if (egl_context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    egl_context)) {
		fprintf(stderr, "EGL ES2 context failed: 0x%x\n", eglGetError());
		goto cleanup;
	}
	query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)
		eglGetProcAddress("eglQueryDmaBufFormatsEXT");
	query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
		eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	if (query_formats == NULL ||
	    !query_formats(display, 0, NULL, &format_count) || format_count <= 0) {
		fprintf(stderr, "EGL dma-buf format query unavailable\n");
		goto cleanup;
	}

	formats = calloc((size_t)format_count, sizeof(*formats));
	if (formats == NULL ||
	    !query_formats(display, format_count, formats, &format_count)) {
		fprintf(stderr, "eglQueryDmaBufFormatsEXT failed: 0x%x\n",
			eglGetError());
		goto cleanup;
	}

	printf("EGL dma-buf formats=%d\n", format_count);
	for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		EGLint modifier_count = 0;
		EGLuint64KHR *modifiers = NULL;
		EGLBoolean *external_only = NULL;
		EGLint modifier;

		printf("EGL %-4s: import=%s", probes[i].name,
		       list_has_format(formats, format_count, probes[i].fourcc) ?
		       "yes" : "no");
		if (query_modifiers != NULL &&
		    query_modifiers(display, (EGLint)probes[i].fourcc, 0,
				    NULL, NULL, &modifier_count))
			printf(" modifiers=%d", modifier_count);
		putchar('\n');
		if (modifier_count > 0) {
			modifiers = calloc((size_t)modifier_count, sizeof(*modifiers));
			external_only = calloc((size_t)modifier_count,
					       sizeof(*external_only));
			if (modifiers != NULL && external_only != NULL &&
			    query_modifiers(display, (EGLint)probes[i].fourcc,
					    modifier_count, modifiers, external_only,
					    &modifier_count)) {
				printf("EGL %-4s modifiers:", probes[i].name);
				for (modifier = 0; modifier < modifier_count; modifier++)
					printf(" 0x%016" PRIx64 "(external=%s)",
					       (uint64_t)modifiers[modifier],
					       external_only[modifier] ? "yes" : "no");
				putchar('\n');
			}
		}
		free(modifiers);
		free(external_only);
		probe_gbm(gbm, &probes[i]);
		probe_actual_import(display, &probes[i]);
	}
	status = EXIT_SUCCESS;

cleanup:
	free(formats);
	if (display != EGL_NO_DISPLAY) {
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			       EGL_NO_CONTEXT);
		if (egl_context != EGL_NO_CONTEXT)
			eglDestroyContext(display, egl_context);
		eglTerminate(display);
	}
	if (gbm != NULL)
		gbm_device_destroy(gbm);
	if (fd >= 0)
		close(fd);
	return status;
}
