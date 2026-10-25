/* rfonts.h -- the reading view's fonts: DejaVu Sans drawn ahead of time
   (fonts/mkfonts.py), with 2-bit coverage, so they can be blended onto any
   background (the text faces are crisp: only 0 and 3; the headings are
   smoothed). */
#ifndef DIAMOND_RFONTS_H
#define DIAMOND_RFONTS_H

#include <stdint.h>

typedef struct {
	uint16_t adv4;		/* how far the pen moves on, in quarter pixels */
	uint8_t w, h;		/* the ink's size */
	int8_t x, y;		/* where the ink starts: from the pen, and down from the top of the line */
	uint16_t off;		/* into the bits: four pixels to a byte, the first in the top two bits */
} rglyph_t;

typedef struct {
	uint8_t height;		/* a line, top to bottom */
	uint8_t ascent;		/* the top of the line to the baseline */
	uint8_t size;		/* the size it was drawn at, in pixels */
	const rglyph_t *glyph;
	const uint8_t *bits;
} rfont_t;

extern const rfont_t rfont_body, rfont_bold, rfont_italic, rfont_bolditalic, rfont_mono,
	rfont_h1, rfont_h2, rfont_h3;

/* the glyph for a Unicode character, or NULL if the fonts don't have it
   (a glyph that moves the pen nowhere isn't in this face: try another) */
static inline const rglyph_t *rfont_glyph(const rfont_t *f, unsigned cp)
{
	unsigned i;
	if (cp >= 32 && cp < 127) i = cp - 32;
	else if (cp >= 160 && cp < 256) i = 95 + cp - 160;
	else {
		static const uint16_t extra[] = { 0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2026, 0x2022 };
		for (i = 0; i < 8 && extra[i] != cp; i++) ;
		if (i == 8) return 0;
		i += 95 + 96;
	}
	return &f->glyph[i];
}

#endif
