/*
 * A 5x7 bitmap font, drawn through picosdl's blitter.
 *
 * The glyphs live in flash as a const array and each one is wrapped in a
 * flash-resident SDL_Surface on demand - the same "sprites are const data in
 * XIP, nothing is copied to RAM" arrangement the game's real sprites will use.
 * So this is a small working example of the sprite path, not just a debug aid.
 */
#ifndef PICOSDL_DEMO_FONT5X7_H
#define PICOSDL_DEMO_FONT5X7_H

#include "SDL2/SDL.h"

#define FONT5X7_W 5
#define FONT5X7_H 7
#define FONT5X7_ADVANCE 6

/* Draw `text` at (x, y) in palette index `color`. Returns the x position just
 * past the last glyph. Characters outside 0x20..0x7E are drawn as spaces. */
int font5x7_draw(SDL_Surface *dst, int x, int y, Uint8 color, const char *text);

/* Width in pixels of `text` when drawn. */
int font5x7_width(const char *text);

#endif /* PICOSDL_DEMO_FONT5X7_H */
