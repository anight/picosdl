/*
 * The palette.
 *
 * There is exactly one, it is shared by every surface, and it *is* the display
 * hardware's CLUT - the panel's PIO/DMA chain expands each 8bpp index through
 * it on the way out. So SDL_SetPaletteColors writes straight to the hardware,
 * and a palette fade costs 256 register writes instead of touching 64000 pixels.
 * That is why USE_FADE and USE_FLASH are affordable on this target at all.
 */
#include "psdl_internal.h"

static SDL_Color  s_colors[256];
static SDL_Palette s_palette;

void psdl_palette_init(void)
{
	for (int i = 0; i < 256; ++i) {
		s_colors[i].r = 0;
		s_colors[i].g = 0;
		s_colors[i].b = 0;
		s_colors[i].a = SDL_ALPHA_OPAQUE;
	}
	s_palette.ncolors  = 256;
	s_palette.colors   = s_colors;
	s_palette.version  = 1;
	s_palette.refcount = 1;
}

SDL_Palette *PSDL_GlobalPalette(void)
{
	if (s_palette.colors == NULL)
		psdl_palette_init();
	return &s_palette;
}

int SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                         int firstcolor, int ncolors)
{
	(void)palette;   /* there is only one */

	if (colors == NULL || ncolors <= 0)
		return 0;
	if (firstcolor < 0) {
		ncolors += firstcolor;
		colors  -= firstcolor;
		firstcolor = 0;
	}
	if (firstcolor + ncolors > 256)
		ncolors = 256 - firstcolor;
	if (ncolors <= 0)
		return 0;

	for (int i = 0; i < ncolors; ++i) {
		s_colors[firstcolor + i]   = colors[i];
		s_colors[firstcolor + i].a = SDL_ALPHA_OPAQUE;
	}
	s_palette.version++;

	psdl_backend_palette_set(firstcolor, ncolors, &s_colors[firstcolor]);
	return 0;
}

/*
 * On an indexed target "map RGB" means "find the closest index". Callers use
 * this to name a colour for SDL_FillRect, which happens rarely enough that a
 * linear search over 256 entries is not worth optimising.
 */
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
	(void)format;

	int best = 0;
	int best_dist = 1 << 30;
	for (int i = 0; i < 256; ++i) {
		int dr = (int)s_colors[i].r - (int)r;
		int dg = (int)s_colors[i].g - (int)g;
		int db = (int)s_colors[i].b - (int)b;
		int dist = dr * dr + dg * dg + db * db;
		if (dist < best_dist) {
			best_dist = dist;
			best = i;
			if (dist == 0)
				break;
		}
	}
	return (Uint32)best;
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	(void)a;
	return SDL_MapRGB(format, r, g, b);
}

void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *format, Uint8 *r, Uint8 *g, Uint8 *b)
{
	(void)format;
	Uint8 index = (Uint8)pixel;
	if (r) *r = s_colors[index].r;
	if (g) *g = s_colors[index].g;
	if (b) *b = s_colors[index].b;
}

const char *SDL_GetPixelFormatName(Uint32 format)
{
	(void)format;
	return "SDL_PIXELFORMAT_INDEX8";
}
