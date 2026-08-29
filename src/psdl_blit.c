/*
 * The blitters. Everything is 8bpp indexed, so a blit is a byte copy with an
 * optional colour-key test - no format conversion, no per-pixel palette lookup.
 *
 * This is the only hot path in picosdl, so the inner loops live in RAM on the
 * Pico rather than being fetched over XIP.
 */
#include "psdl_internal.h"

#ifdef PICO_ON_DEVICE
#include "pico.h"
#define PSDL_HOT __not_in_flash_func
#else
#define PSDL_HOT(f) f
#endif

/* ------------------------------------------------------------- clipping */

/*
 * Resolve a blit request into the source and destination rectangles actually
 * touched, after clipping to both surfaces and to the destination's clip rect.
 * `orig_src` receives the source rectangle before clipping, which the mirrored
 * blit needs in order to work out which source column a clipped destination
 * column corresponds to.
 *
 * Returns 0 if the blit is entirely clipped away.
 */
static int clip_blit(SDL_Surface *src, const SDL_Rect *srcrect,
                     SDL_Surface *dst, const SDL_Rect *dstrect,
                     SDL_Rect *out_src, SDL_Rect *out_dst, SDL_Rect *orig_src)
{
	SDL_Rect sr;
	if (srcrect != NULL) {
		sr = *srcrect;
	} else {
		sr.x = 0; sr.y = 0; sr.w = src->w; sr.h = src->h;
	}

	int dx = dstrect ? dstrect->x : 0;
	int dy = dstrect ? dstrect->y : 0;

	/* Clamp the source rectangle to the source surface. */
	if (sr.x < 0) { sr.w += sr.x; dx -= sr.x; sr.x = 0; }
	if (sr.y < 0) { sr.h += sr.y; dy -= sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	if (sr.w <= 0 || sr.h <= 0) return 0;

	*orig_src = sr;

	/* Clip against the destination's clip rect. */
	const SDL_Rect *c = &dst->clip_rect;
	int cl = c->x, ct = c->y, cr = c->x + c->w, cb = c->y + c->h;
	if (cl < 0) cl = 0;
	if (ct < 0) ct = 0;
	if (cr > dst->w) cr = dst->w;
	if (cb > dst->h) cb = dst->h;

	if (dx < cl)          { int d = cl - dx; sr.x += d; sr.w -= d; dx = cl; }
	if (dy < ct)          { int d = ct - dy; sr.y += d; sr.h -= d; dy = ct; }
	if (dx + sr.w > cr)   { sr.w = cr - dx; }
	if (dy + sr.h > cb)   { sr.h = cb - dy; }
	if (sr.w <= 0 || sr.h <= 0) return 0;

	*out_src = sr;
	out_dst->x = dx;
	out_dst->y = dy;
	out_dst->w = sr.w;
	out_dst->h = sr.h;
	return 1;
}

static inline Uint8 *row_of(SDL_Surface *s, int y, int x)
{
	return (Uint8 *)s->pixels + (size_t)y * (size_t)s->pitch + (size_t)x;
}

static inline const Uint8 *crow_of(const SDL_Surface *s, int y, int x)
{
	return (const Uint8 *)s->pixels + (size_t)y * (size_t)s->pitch + (size_t)x;
}

/* --------------------------------------------------------------- fill */

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
	if (dst == NULL)
		return -1;

	SDL_Rect r;
	if (rect != NULL) {
		r = *rect;
	} else {
		r.x = 0; r.y = 0; r.w = dst->w; r.h = dst->h;
	}

	const SDL_Rect *c = &dst->clip_rect;
	int x0 = r.x < c->x ? c->x : r.x;
	int y0 = r.y < c->y ? c->y : r.y;
	int x1 = r.x + r.w, y1 = r.y + r.h;
	if (x1 > c->x + c->w) x1 = c->x + c->w;
	if (y1 > c->y + c->h) y1 = c->y + c->h;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > dst->w) x1 = dst->w;
	if (y1 > dst->h) y1 = dst->h;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	Uint8 value = (Uint8)color;
	for (int y = y0; y < y1; ++y)
		memset(row_of(dst, y, x0), value, (size_t)(x1 - x0));
	return 0;
}

/* -------------------------------------------------------------- blits */

static void PSDL_HOT(blit_rows_opaque)(const Uint8 *sp, int spitch,
                                       Uint8 *dp, int dpitch, int w, int h)
{
	for (int y = 0; y < h; ++y) {
		memcpy(dp, sp, (size_t)w);
		sp += spitch;
		dp += dpitch;
	}
}

static void PSDL_HOT(blit_rows_keyed)(const Uint8 *sp, int spitch,
                                      Uint8 *dp, int dpitch, int w, int h, Uint8 key)
{
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			Uint8 v = sp[x];
			if (v != key)
				dp[x] = v;
		}
		sp += spitch;
		dp += dpitch;
	}
}

static void PSDL_HOT(blit_rows_keyed_offset)(const Uint8 *sp, int spitch,
                                             Uint8 *dp, int dpitch, int w, int h,
                                             Uint8 key, Uint8 offset)
{
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			Uint8 v = sp[x];
			if (v != key)
				dp[x] = (Uint8)(v + offset);
		}
		sp += spitch;
		dp += dpitch;
	}
}

int SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	const Uint8 *sp = crow_of(src, s.y, s.x);
	Uint8       *dp = row_of(dst, d.y, d.x);

	if (src->has_colorkey)
		blit_rows_keyed(sp, src->pitch, dp, dst->pitch, s.w, s.h, (Uint8)src->colorkey);
	else
		blit_rows_opaque(sp, src->pitch, dp, dst->pitch, s.w, s.h);

	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

/*
 * Transparency chosen per call rather than per surface.
 *
 * SDLPoP toggles transparency by calling SDL_SetColorKey on the sprite before
 * each blit. Sprites are const objects in flash here, so that write is not
 * available; the key is baked in and this picks whether to honour it.
 */
int PSDL_BlitTransp(SDL_Surface *src, const SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect, int transparent)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	const Uint8 *sp = crow_of(src, s.y, s.x);
	Uint8       *dp = row_of(dst, d.y, d.x);

	if (transparent && src->has_colorkey)
		blit_rows_keyed(sp, src->pitch, dp, dst->pitch, s.w, s.h, (Uint8)src->colorkey);
	else
		blit_rows_opaque(sp, src->pitch, dp, dst->pitch, s.w, s.h);

	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

/*
 * Draw every non-transparent pixel as one colour, ignoring the source index.
 *
 * This is SDLPoP's method_3_blit_mono, which built a whole ARGB8888 conversion
 * of the sprite to achieve it. On an indexed target it is a keyed blit that
 * writes a constant, and it is how the depth-1 images and the built-in font get
 * drawn in whatever colour the caller wants.
 */
static void PSDL_HOT(blit_rows_mono)(const Uint8 *sp, int spitch,
                                     Uint8 *dp, int dpitch, int w, int h,
                                     Uint8 key, Uint8 color)
{
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			if (sp[x] != key)
				dp[x] = color;
		}
		sp += spitch;
		dp += dpitch;
	}
}

int PSDL_BlitMono(SDL_Surface *src, const SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect, Uint8 color)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	blit_rows_mono(crow_of(src, s.y, s.x), src->pitch,
	               row_of(dst, d.y, d.x), dst->pitch, s.w, s.h,
	               src->has_colorkey ? (Uint8)src->colorkey : 0, color);
	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

int PSDL_BlitOffset(SDL_Surface *src, const SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect, Uint8 index_offset)
{
	if (src == NULL || dst == NULL)
		return -1;
	if (index_offset == 0)
		return SDL_UpperBlit(src, srcrect, dst, dstrect);

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	blit_rows_keyed_offset(crow_of(src, s.y, s.x), src->pitch,
	                       row_of(dst, d.y, d.x), dst->pitch, s.w, s.h,
	                       src->has_colorkey ? (Uint8)src->colorkey : 0xFF,
	                       index_offset);
	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

/*
 * Mirrored blit. This replaces SDLPoP's hflip(), which allocated a whole
 * surface and blitted into it every time a sprite was drawn facing left -
 * dozens of times a frame. Walking the source backwards costs nothing extra.
 */
int PSDL_BlitMirrored(SDL_Surface *src, const SDL_Rect *srcrect,
                      SDL_Surface *dst, SDL_Rect *dstrect)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	/* Destination column j (from the unclipped start) reads source column
	 * orig.x + orig.w - 1 - j. Clipping advanced us by (s.x - orig.x) columns. */
	int jstart = s.x - orig.x;
	int sx     = orig.x + orig.w - 1 - jstart;

	const Uint8 *sp = crow_of(src, s.y, sx);
	Uint8       *dp = row_of(dst, d.y, d.x);
	Uint8        key = (Uint8)src->colorkey;
	int          keyed = src->has_colorkey;

	for (int y = 0; y < s.h; ++y) {
		if (keyed) {
			for (int x = 0; x < s.w; ++x) {
				Uint8 v = sp[-x];
				if (v != key)
					dp[x] = v;
			}
		} else {
			for (int x = 0; x < s.w; ++x)
				dp[x] = sp[-x];
		}
		sp += src->pitch;
		dp += dst->pitch;
	}

	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

/*
 * XOR blit over palette indices. SDLPoP's blit_xor() built two scratch surfaces
 * per call to get here; the DOS original just XORed indices, which is what the
 * "flash" effect actually wants.
 */
int PSDL_BlitXor(SDL_Surface *src, const SDL_Rect *srcrect,
                 SDL_Surface *dst, SDL_Rect *dstrect)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s, d, orig;
	if (!clip_blit(src, srcrect, dst, dstrect, &s, &d, &orig)) {
		if (dstrect != NULL) { dstrect->w = 0; dstrect->h = 0; }
		return 0;
	}

	const Uint8 *sp = crow_of(src, s.y, s.x);
	Uint8       *dp = row_of(dst, d.y, d.x);
	for (int y = 0; y < s.h; ++y) {
		for (int x = 0; x < s.w; ++x)
			dp[x] ^= sp[x];
		sp += src->pitch;
		dp += dst->pitch;
	}

	if (dstrect != NULL)
		*dstrect = d;
	return 0;
}

/* Nearest-neighbour, for completeness. USE_SCALING is off in the target build. */
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
                   SDL_Surface *dst, SDL_Rect *dstrect)
{
	if (src == NULL || dst == NULL)
		return -1;

	SDL_Rect s = srcrect ? *srcrect : (SDL_Rect){ 0, 0, src->w, src->h };
	SDL_Rect d = dstrect ? *dstrect : (SDL_Rect){ 0, 0, dst->w, dst->h };
	if (s.w <= 0 || s.h <= 0 || d.w <= 0 || d.h <= 0)
		return 0;

	Uint8 key   = (Uint8)src->colorkey;
	int   keyed = src->has_colorkey;

	for (int y = 0; y < d.h; ++y) {
		int dy = d.y + y;
		if (dy < dst->clip_rect.y || dy >= dst->clip_rect.y + dst->clip_rect.h)
			continue;
		const Uint8 *sp = crow_of(src, s.y + (y * s.h) / d.h, s.x);
		Uint8       *dp = row_of(dst, dy, 0);
		for (int x = 0; x < d.w; ++x) {
			int dxp = d.x + x;
			if (dxp < dst->clip_rect.x || dxp >= dst->clip_rect.x + dst->clip_rect.w)
				continue;
			Uint8 v = sp[(x * s.w) / d.w];
			if (!keyed || v != key)
				dp[dxp] = v;
		}
	}
	return 0;
}
