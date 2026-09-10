/*
 * VPx range coder — minimal port of ffmpeg's libavcodec/vpx_rac.{c,h}
 * sufficient for parsing the VP9 compressed header (§ 6.3).
 *
 * Adapted from FFmpeg (LGPL-2.1+) © 2010 Fiona Glaser, 2006 Aurelien
 * Jacobs. Stripped of arch-specific paths; pure portable C.
 */

#ifndef _VPX_RAC_H_
#define _VPX_RAC_H_

#include <stdint.h>
#include <stddef.h>

struct vpx_rac {
	int		high;
	int		bits;	/* negated: negative = positive bits left */
	const uint8_t	*buffer;
	const uint8_t	*end;
	unsigned int	code_word;
	int		end_reached;
};

extern const uint8_t vpx_rac_norm_shift[256];

int vpx_rac_init(struct vpx_rac *c, const uint8_t *buf, size_t buf_size);

static inline int vpx_rac_is_end(struct vpx_rac *c)
{
	if (c->end <= c->buffer && c->bits >= 0)
		c->end_reached++;
	return c->end_reached > 10;
}

static inline unsigned int vpx_rac_renorm(struct vpx_rac *c)
{
	int shift = vpx_rac_norm_shift[c->high];
	int bits = c->bits;
	unsigned int code_word = c->code_word;

	c->high <<= shift;
	code_word <<= shift;
	bits += shift;
	if (bits >= 0 && c->buffer < c->end) {
		unsigned int v = (unsigned int)c->buffer[0] << 8;

		c->buffer++;
		if (c->buffer < c->end)
			v |= *c->buffer++;
		code_word |= v << bits;
		bits -= 16;
	}
	c->bits = bits;
	return code_word;
}

static inline int vpx_rac_get_prob(struct vpx_rac *c, uint8_t prob)
{
	unsigned int code_word = vpx_rac_renorm(c);
	unsigned int low = 1 + (((c->high - 1) * prob) >> 8);
	unsigned int low_shift = low << 16;
	int bit = code_word >= low_shift;

	c->high = bit ? c->high - low : low;
	c->code_word = bit ? code_word - low_shift : code_word;
	return bit;
}

static inline int vpx_rac_get_prob_branchy(struct vpx_rac *c, int prob)
{
	unsigned long code_word = vpx_rac_renorm(c);
	unsigned low = 1 + (((c->high - 1) * prob) >> 8);
	unsigned long low_shift = (unsigned long)low << 16;

	if (code_word >= low_shift) {
		c->high -= low;
		c->code_word = (unsigned int)(code_word - low_shift);
		return 1;
	}
	c->high = low;
	c->code_word = (unsigned int)code_word;
	return 0;
}

/* Equiprobable bit (50/50). */
static inline int vp89_rac_get(struct vpx_rac *c)
{
	unsigned int code_word = vpx_rac_renorm(c);
	int low = (c->high + 1) >> 1;
	unsigned int low_shift = low << 16;
	int bit = code_word >= low_shift;

	if (bit) {
		c->high -= low;
		code_word -= low_shift;
	} else {
		c->high = low;
	}
	c->code_word = code_word;
	return bit;
}

static inline unsigned int vp89_rac_get_uint(struct vpx_rac *c, int bits)
{
	unsigned int v = 0;
	while (bits-- > 0)
		v = (v << 1) | (unsigned int)vp89_rac_get(c);
	return v;
}

#endif /* _VPX_RAC_H_ */
