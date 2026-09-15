/*
 * Surfaces, without a heap.
 *
 * Pixels come from one of three places and the surface flags record which, so
 * SDL_FreeSurface can dispatch without a general allocator underneath:
 *
 *   PSDL_SURF_EXTERN  caller-supplied pixels (const data in XIP, or a static
 *                     buffer someone else owns). Freeing is a no-op.
 *   PSDL_SURF_STATIC  a named static framebuffer, or a screen-pool slot.
 *   PSDL_SURF_ARENA   the LIFO bump arena below.
 *
 * Orthogonal to that, PSDL_SURF_CONST marks a surface whose *object* is also in
 * flash - the game's build-time-converted sprites, 673 of them. Those cannot be
 * written to at all, so every mutating entry point checks is_const() first.
 *
 * The arena exists for peels. SDLPoP creates a peel when a sprite is about to
 * overwrite background it will need back, and restore_peels() walks the table
 * backwards - so peels are released in strict stack order and a bump pointer is
 * all the bookkeeping required. Freeing out of order is tolerated but does not
 * reclaim until the top of the stack is dead, which keeps the invariant simple
 * and makes a non-LIFO caller show up as arena pressure rather than corruption.
 */
#include <stdio.h>

#include "psdl_internal.h"

/* --------------------------------------------------------------- storage */

static SDL_Surface  s_headers[PSDL_MAX_SURFACES];
static SDL_Surface *s_free_headers;          /* singly linked via userdata */
static int          s_headers_in_use;
static int          s_headers_high_water;

/* 4-byte aligned so the blitters can move whole words. */
static Uint8 s_arena[PSDL_ARENA_BYTES] __attribute__((aligned(4)));
static size_t s_arena_top;
static size_t s_arena_high_water;

typedef struct {
	SDL_Surface *surface;
	size_t       base;    /* arena offset this entry started at */
	int          dead;
} arena_entry_t;

#ifndef PSDL_ARENA_MAX_ENTRIES
#define PSDL_ARENA_MAX_ENTRIES 64
#endif
static arena_entry_t s_arena_stack[PSDL_ARENA_MAX_ENTRIES];
static int           s_arena_depth;

/*
 * Full-screen buffers get their own pool rather than coming out of the arena.
 * SDLPoP asks for several of these (the window surface, the offscreen buffer
 * the game draws into) and at 320x200 they are 62.5 KB each - putting them in
 * the arena would mean sizing the arena for them and wasting it the rest of the
 * time. They also outlive everything, so they never want reclaiming.
 */
#define PSDL_SCREEN_BYTES ((size_t)PSDL_SCREEN_W * (size_t)PSDL_SCREEN_H)
static Uint8 s_screen_pool[PSDL_SCREEN_BUFFERS][PSDL_SCREEN_BYTES] __attribute__((aligned(4)));
static Uint8 s_screen_pool_used[PSDL_SCREEN_BUFFERS];

void psdl_surface_init(void)
{
	s_free_headers = NULL;
	for (int i = PSDL_MAX_SURFACES - 1; i >= 0; --i) {
		s_headers[i].userdata = s_free_headers;
		s_free_headers = &s_headers[i];
	}
	s_headers_in_use = 0;
	s_arena_top = 0;
	s_arena_depth = 0;
	memset(s_screen_pool_used, 0, sizeof(s_screen_pool_used));

	psdl_pixel_format.palette = PSDL_GlobalPalette();
}

SDL_Surface *psdl_surface_alloc_header(void)
{
	SDL_Surface *s = s_free_headers;
	PSDL_ASSERT(s != NULL, "out of surface headers - raise PSDL_MAX_SURFACES");
	s_free_headers = (SDL_Surface *)s->userdata;

	memset(s, 0, sizeof(*s));
	if (++s_headers_in_use > s_headers_high_water)
		s_headers_high_water = s_headers_in_use;
	return s;
}

void psdl_surface_free_header(SDL_Surface *s)
{
	memset(s, 0, sizeof(*s));
	s->userdata = s_free_headers;
	s_free_headers = s;
	--s_headers_in_use;
}

/* -------------------------------------------------------------- plumbing */

/* The single shared format. Generated flash surfaces point at this too. */
SDL_PixelFormat psdl_pixel_format = {
	.format        = SDL_PIXELFORMAT_INDEX8,
	.palette       = NULL,          /* filled in by psdl_surface_init */
	.BitsPerPixel  = 8,
	.BytesPerPixel = 1,
};

static void surface_finish(SDL_Surface *s, void *pixels, int w, int h, int pitch, Uint32 region)
{
	s->format       = &psdl_pixel_format;
	s->flags        = region;
	s->w            = w;
	s->h            = h;
	s->pitch        = pitch;
	s->pixels       = pixels;
	s->refcount     = 1;
	s->locked       = 0;
	s->has_colorkey = SDL_FALSE;
	s->colorkey     = 0;
	s->alpha_mod    = SDL_ALPHA_OPAQUE;
	s->blend_mode   = SDL_BLENDMODE_NONE;
	s->clip_rect.x  = 0;
	s->clip_rect.y  = 0;
	s->clip_rect.w  = w;
	s->clip_rect.h  = h;
}

/* Wrap a framebuffer that lives in .bss and outlives everything. */
SDL_Surface *psdl_surface_wrap_static(void *pixels, int w, int h, int pitch)
{
	SDL_Surface *s = psdl_surface_alloc_header();
	surface_finish(s, pixels, w, h, pitch, PSDL_SURF_STATIC);
	return s;
}

/* ----------------------------------------------------------- public API */

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
	(void)flags; (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;

	if (width <= 0 || height <= 0) {
		SDL_SetError("SDL_CreateRGBSurface: bad size %dx%d", width, height);
		return NULL;
	}

	/* A request for exactly the screen size comes from the dedicated pool. */
	if (width == PSDL_SCREEN_W && height == PSDL_SCREEN_H) {
		for (int i = 0; i < PSDL_SCREEN_BUFFERS; ++i) {
			if (s_screen_pool_used[i])
				continue;
			s_screen_pool_used[i] = 1;
			memset(s_screen_pool[i], 0, PSDL_SCREEN_BYTES);
			SDL_Surface *s = psdl_surface_alloc_header();
			surface_finish(s, s_screen_pool[i], width, height, PSDL_SCREEN_W,
			               PSDL_SURF_STATIC);
			s->pool_slot = i + 1;
			return s;
		}
		SDL_SetError("picosdl: out of screen buffers - raise PSDL_SCREEN_BUFFERS");
		return NULL;
	}

	/* Everything is 8bpp; pitch is rounded up so rows stay word-aligned. */
	int    pitch = (width + 3) & ~3;
	size_t bytes = (size_t)pitch * (size_t)height;

	PSDL_ASSERT(s_arena_depth < PSDL_ARENA_MAX_ENTRIES, "arena entry stack full");
	if (s_arena_top + bytes > PSDL_ARENA_BYTES) {
		SDL_SetError("picosdl: arena exhausted (%u used, %u wanted, %u total)",
		             (unsigned)s_arena_top, (unsigned)bytes, (unsigned)PSDL_ARENA_BYTES);
		/* The caller reports the error; this says what is holding the space,
		 * which is the part that takes time to work out afterwards. */
		PSDL_DumpArena();
		return NULL;
	}

	size_t base   = s_arena_top;
	Uint8 *pixels = s_arena + base;
	s_arena_top   = (base + bytes + 3) & ~(size_t)3;
	if (s_arena_top > s_arena_high_water)
		s_arena_high_water = s_arena_top;

	memset(pixels, 0, bytes);

	SDL_Surface *s = psdl_surface_alloc_header();
	surface_finish(s, pixels, width, height, pitch, PSDL_SURF_ARENA);

	s_arena_stack[s_arena_depth].surface = s;
	s_arena_stack[s_arena_depth].base    = base;
	s_arena_stack[s_arena_depth].dead    = 0;
	++s_arena_depth;
	return s;
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth,
                                      int pitch, Uint32 Rmask, Uint32 Gmask,
                                      Uint32 Bmask, Uint32 Amask)
{
	(void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
	SDL_Surface *s = psdl_surface_alloc_header();
	surface_finish(s, pixels, width, height, pitch ? pitch : width, PSDL_SURF_EXTERN);
	return s;
}

/*
 * A surface generated into flash is genuinely read-only: it lives in XIP, so
 * every field write - a refcount decrement, a lock counter, a colour key -
 * faults. Callers do all three on ordinary SDL surfaces, so each entry point
 * has to check first. This is the single most important invariant in the file.
 */
static inline int is_const(const SDL_Surface *s)
{
	return (s->flags & PSDL_SURF_CONST) != 0;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
	if (surface == NULL || is_const(surface))
		return;
	if (--surface->refcount > 0)
		return;

	if (surface->pool_slot > 0) {
		s_screen_pool_used[surface->pool_slot - 1] = 0;
	} else if ((surface->flags & PSDL_SURF_REGION) == PSDL_SURF_ARENA) {
		/* Mark dead, then unwind as far as the stack allows. In the normal
		 * LIFO case this reclaims immediately. */
		for (int i = s_arena_depth - 1; i >= 0; --i) {
			if (s_arena_stack[i].surface == surface) {
				s_arena_stack[i].dead = 1;
				break;
			}
		}
		while (s_arena_depth > 0 && s_arena_stack[s_arena_depth - 1].dead) {
			--s_arena_depth;
			s_arena_top = s_arena_stack[s_arena_depth].base;
		}
	}
	/* STATIC and FLASH own no memory we can give back. */

	psdl_surface_free_header(surface);
}

int SDL_LockSurface(SDL_Surface *surface)
{
	if (surface == NULL)
		return -1;
	if (!is_const(surface))
		surface->locked++;
	return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
	if (surface != NULL && !is_const(surface) && surface->locked > 0)
		surface->locked--;
}

/* There is only one format, so a conversion is a copy - and every caller in
 * SDLPoP converts in order to then modify the copy, so it has to be a real one. */
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags)
{
	(void)fmt; (void)flags;
	if (src == NULL)
		return NULL;

	SDL_Surface *dst = SDL_CreateRGBSurface(0, src->w, src->h, 8, 0, 0, 0, 0);
	if (dst == NULL)
		return NULL;
	for (int y = 0; y < src->h; ++y)
		memcpy((Uint8 *)dst->pixels + (size_t)y * dst->pitch,
		       (const Uint8 *)src->pixels + (size_t)y * src->pitch,
		       (size_t)src->w);
	dst->has_colorkey = src->has_colorkey;
	dst->colorkey     = src->colorkey;
	dst->blend_mode   = src->blend_mode;
	dst->alpha_mod    = src->alpha_mod;
	return dst;
}

SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags)
{
	(void)pixel_format;
	return SDL_ConvertSurface(src, NULL, flags);
}

int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key)
{
	if (surface == NULL)
		return -1;

	if (is_const(surface)) {
		/* A generated sprite carries a baked key - the transparent index for
		 * its palette row - and cannot be changed. Accept a call that asks for
		 * what it already has, so code that sets the key defensively still
		 * works, and refuse one that would silently render the wrong thing.
		 * Use PSDL_BlitTransp to turn transparency off for a single blit. */
		if ((flag ? SDL_TRUE : SDL_FALSE) == surface->has_colorkey &&
		    (!flag || key == surface->colorkey))
			return 0;
		SDL_SetError("picosdl: SDL_SetColorKey on a flash sprite (key %u->%u); "
		             "use PSDL_BlitTransp instead",
		             (unsigned)surface->colorkey, (unsigned)key);
		return -1;
	}

	surface->has_colorkey = flag ? SDL_TRUE : SDL_FALSE;
	surface->colorkey     = key;
	return 0;
}

int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode)
{
	if (surface == NULL)
		return -1;
	if (is_const(surface))
		return 0;
	surface->blend_mode = blendMode;
	return 0;
}

int SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha)
{
	if (surface == NULL)
		return -1;
	if (is_const(surface))
		return 0;
	surface->alpha_mod = alpha;
	return 0;
}

int SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette)
{
	(void)palette;
	if (surface == NULL || is_const(surface))
		return -1;
	/* There is one palette and every surface already points at it. */
	surface->format->palette = PSDL_GlobalPalette();
	return 0;
}

SDL_bool SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect)
{
	if (surface == NULL)
		return SDL_FALSE;

	if (is_const(surface))
		return SDL_TRUE;   /* a flash sprite is only ever a blit source */

	SDL_Rect full = { 0, 0, surface->w, surface->h };
	if (rect == NULL) {
		surface->clip_rect = full;
		return SDL_TRUE;
	}

	int x0 = rect->x < 0 ? 0 : rect->x;
	int y0 = rect->y < 0 ? 0 : rect->y;
	int x1 = rect->x + rect->w;
	int y1 = rect->y + rect->h;
	if (x1 > surface->w) x1 = surface->w;
	if (y1 > surface->h) y1 = surface->h;

	surface->clip_rect.x = x0;
	surface->clip_rect.y = y0;
	surface->clip_rect.w = x1 > x0 ? x1 - x0 : 0;
	surface->clip_rect.h = y1 > y0 ? y1 - y0 : 0;
	return (surface->clip_rect.w > 0 && surface->clip_rect.h > 0) ? SDL_TRUE : SDL_FALSE;
}

void SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect)
{
	if (surface != NULL && rect != NULL)
		*rect = surface->clip_rect;
}

/* ------------------------------------------------------------ diagnostics */

/*
 * What is in the arena right now, entry by entry.
 *
 * PSDL_ReportMemory()'s total cannot distinguish demand from dead weight, and
 * the difference is what matters: the arena is LIFO, so one long-lived entry
 * near the bottom pins everything above it and the total reads as healthy use.
 * Two real bugs were found by printing this - a 16.5 KB surface nothing read
 * holding the floor, and a leak of small ones - and neither was visible in the
 * total. `dead` means freed but not yet reclaimable, which is the interesting
 * state: it can only happen when a caller released out of stack order.
 */
void PSDL_DumpArena(void)
{
	printf("picosdl: arena %u/%u bytes, %d entries\n",
	       (unsigned)s_arena_top, (unsigned)PSDL_ARENA_BYTES, s_arena_depth);
	for (int i = 0; i < s_arena_depth; ++i) {
		size_t end = (i + 1 < s_arena_depth) ? s_arena_stack[i + 1].base
		                                     : s_arena_top;
		const SDL_Surface *su = s_arena_stack[i].surface;
		printf("  [%3d] at %6u %6u bytes %-4s %dx%d\n", i,
		       (unsigned)s_arena_stack[i].base,
		       (unsigned)(end - s_arena_stack[i].base),
		       s_arena_stack[i].dead ? "dead" : "live",
		       su ? su->w : -1, su ? su->h : -1);
	}
}

void PSDL_ReportMemory(void)
{
	int screens = 0;
	for (int i = 0; i < PSDL_SCREEN_BUFFERS; ++i)
		screens += s_screen_pool_used[i];

	printf("picosdl: surfaces %d/%d (peak %d), arena %u/%u bytes (peak %u, depth %d), "
	       "screen buffers %d/%d\n",
	       s_headers_in_use, PSDL_MAX_SURFACES, s_headers_high_water,
	       (unsigned)s_arena_top, (unsigned)PSDL_ARENA_BYTES,
	       (unsigned)s_arena_high_water, s_arena_depth,
	       screens, PSDL_SCREEN_BUFFERS);
}
