/*
 * Bitmap fonts, drawn through picosdl's blitter.
 *
 * Two sizes. 5x7 is the classic GLCD table and is what fits on a 320x200 canvas
 * beside anything else; 9x14 comes from the X11 misc-fixed family and is for
 * places with room, such as the letterbox status bands, where 5x7 is legible but
 * small on a physical panel.
 *
 * Both are stored the same way - column-major, one Uint16 per column, bit 0 the
 * top row - so there is one draw loop rather than one per size. 5x7 only needs a
 * byte per column and pays 475 bytes of flash for the uniformity, which is the
 * cheaper half of that trade.
 *
 * Drawing goes through the ordinary keyed blitter rather than poking the
 * framebuffer, because the point of having a font here is partly to exercise that
 * path: each glyph is expanded into a scratch surface from the LIFO arena,
 * blitted, and released.
 */
#ifndef PSDL_FONT_INCLUDED
#define PSDL_FONT_INCLUDED

#include "SDL2/SDL.h"

typedef struct PSDL_Font {
	int width;      /* glyph box, in pixels                  */
	int height;
	int advance;    /* pen movement per character             */
	const Uint16 (*glyphs)[];   /* 95 glyphs, 0x20..0x7E, `width` columns each */
	int stride;     /* columns per glyph, i.e. == width       */
} PSDL_Font;

extern const PSDL_Font PSDL_Font5x7;    /* small: fits anywhere */
extern const PSDL_Font PSDL_Font9x14;   /* large: for status bands and titles */

/* Draw `text` at (x, y) in palette index `color`. Returns the x just past the
 * last glyph. Characters outside 0x20..0x7E are drawn as spaces. */
int PSDL_FontDraw(const PSDL_Font *font, SDL_Surface *dst, int x, int y,
                  Uint8 color, const char *text);

/* Width in pixels of `text` in `font`, as PSDL_FontDraw would lay it out. */
int PSDL_FontWidth(const PSDL_Font *font, const char *text);

#endif /* PSDL_FONT_INCLUDED */
