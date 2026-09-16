/*
 * A 5x7 bitmap font, drawn through picosdl's blitter.
 *
 * The glyphs live in flash as a const array and each one is wrapped in a
 * flash-resident SDL_Surface on demand - the same "sprites are const data in XIP,
 * nothing is copied to RAM" arrangement a game's real sprites use. So it is a small
 * working example of the sprite path as well as a way to put words on a screen.
 *
 * It was the demo's until the status bands needed it (PSDL_StatusBands), and a
 * library that can draw its own frame rate onto a panel should not need a client to
 * lend it a font.
 */
#ifndef PSDL_FONT5X7_INCLUDED
#define PSDL_FONT5X7_INCLUDED

#include "SDL2/SDL.h"

#define PSDL_FONT5X7_WIDTH   5
#define PSDL_FONT5X7_HEIGHT  7
#define PSDL_FONT5X7_ADVANCE 6

/* Draw `text` at (x, y) in palette index `color`. Returns the x position just
 * past the last glyph. Characters outside 0x20..0x7E are drawn as spaces. */
int PSDL_Font5x7Draw(SDL_Surface *dst, int x, int y, Uint8 color, const char *text);

/* Width in pixels of `text` when drawn. */
int PSDL_Font5x7Width(const char *text);

#endif /* PSDL_FONT5X7_INCLUDED */
