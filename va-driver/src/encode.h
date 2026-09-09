#ifndef REQUEST_ENCODE_H
#define REQUEST_ENCODE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_enc_h264.h>

struct request_data;
struct object_surface;
struct object_buffer;

#define REQUEST_ENCODE_RAW_BUFFERS 4
#define REQUEST_ENCODE_CODED_BUFFERS 4
#define REQUEST_ENCODE_MAX_PACKED_HEADERS 8

struct encode_picture_params {
	bool have_sequence;
	bool have_picture;
	bool have_slice;
	VAEncSequenceParameterBufferH264 sequence;
	VAEncPictureParameterBufferH264 picture;
	VAEncSliceParameterBufferH264 slice;

	unsigned int bitrate;
	unsigned int framerate_num;
	unsigned int framerate_den;
	unsigned int qp_min;
	unsigned int qp_max;

	bool packed_pending;
	uint32_t packed_type_pending;
	unsigned int packed_bits_pending;
	unsigned int num_packed;
	uint32_t packed_type[REQUEST_ENCODE_MAX_PACKED_HEADERS];
	const void *packed_data[REQUEST_ENCODE_MAX_PACKED_HEADERS];
	unsigned int packed_bits[REQUEST_ENCODE_MAX_PACKED_HEADERS];
};

struct encode_context {
	int video_fd;
	unsigned int width;
	unsigned int height;
	unsigned int raw_width;
	unsigned int raw_height;
	unsigned int raw_pitch;
	unsigned int raw_size;
	unsigned int coded_size;
	unsigned int raw_pixelformat;
	unsigned int coded_pixelformat;
	unsigned int raw_type;
	unsigned int coded_type;

	void *raw_data[REQUEST_ENCODE_RAW_BUFFERS];
	unsigned int raw_length[REQUEST_ENCODE_RAW_BUFFERS];
	unsigned int num_raw_buffers;
	unsigned int next_raw;

	void *coded_data[REQUEST_ENCODE_CODED_BUFFERS];
	unsigned int coded_length[REQUEST_ENCODE_CODED_BUFFERS];
	unsigned int num_coded_buffers;
	unsigned int next_coded;

	unsigned int applied_bitrate;
	unsigned int applied_framerate_num;
	unsigned int applied_framerate_den;
	unsigned int applied_qp_i;
	unsigned int applied_qp_p;
	unsigned int applied_qp_min;
	unsigned int applied_qp_max;
	unsigned int applied_level;
	bool applied_constant_quality;
	bool sequence_seen;
};

int encode_context_create(struct request_data *driver_data,
			  struct encode_context *encode,
			  VAProfile profile,
			  unsigned int rc_mode,
			  unsigned int width,
			  unsigned int height);
void encode_context_destroy(struct encode_context *encode);

void encode_params_reset(struct encode_picture_params *params);
VAStatus encode_params_collect(struct encode_picture_params *params,
			       struct object_buffer *buffer_object);

VAStatus encode_surface_storage(struct object_surface *surface_object);
void encode_surface_release(struct object_surface *surface_object);

VAStatus encode_picture_submit(struct encode_context *encode,
			       struct object_surface *surface_object,
			       struct encode_picture_params *params,
			       struct object_buffer *coded_buffer);

#endif
