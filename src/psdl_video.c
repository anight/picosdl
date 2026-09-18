/*
 * The window and the screen surface.
 *
 * There is one window, it is always fullscreen, and its surface wraps a
 * framebuffer the client allocated and handed to PSDL_CreateWindow(). picosdl
 * owns no pixels.
 *
 * SDL_UpdateWindowSurface hands that buffer to the backend, which on hardware
 * kicks a DMA chain and returns immediately - the wait happens at the start of
 * the next present, so drawing overlaps with the panel push. A client that draws
 * into the buffer it just presented calls PSDL_PresentSync() first; only it knows
 * how many buffers it is cycling.
 */
#include "psdl_internal.h"

struct SDL_Window {
	int          w, h;
	Uint32       flags;
	SDL_Surface *surface;
	int          in_use;
};

static struct SDL_Window s_window;
static int               s_video_ready;

void psdl_video_init(void)
{
	if (s_video_ready)
		return;
	psdl_backend_video_init(PSDL_SCREEN_W, PSDL_SCREEN_H);
	s_video_ready = 1;
}

SDL_Surface *psdl_screen_surface(void)
{
	return s_window.surface;
}

/*
 * Create the window over a framebuffer the caller owns.
 *
 * picosdl allocates no pixels, so this takes them. The surface it hands back
 * wraps the caller's memory and is never freed by the library - PSDL_SURF_EXTERN
 * says so, and SDL_FreeSurface() on it releases only the header.
 *
 * There is no SDL_CreateWindow(): its signature has nowhere to put the memory,
 * and answering it would mean keeping a full-screen buffer inside the library for
 * every client, including the ones that already have one.
 */
SDL_Window *PSDL_CreateWindow(void *pixels, int w, int h, int pitch)
{
	if (pixels == NULL || w <= 0 || h <= 0 || pitch < w * PSDL_BYTES_PER_PIXEL) {
		SDL_SetError("PSDL_CreateWindow: bad buffer %dx%d pitch %d", w, h, pitch);
		return NULL;
	}

	psdl_video_init();

	if (s_window.in_use)
		return &s_window;

	s_window.surface = SDL_CreateRGBSurfaceFrom(pixels, w, h, PSDL_COLOR_DEPTH,
	                                            pitch, 0, 0, 0, 0);
	if (s_window.surface == NULL)
		return NULL;

	s_window.w      = w;
	s_window.h      = h;
	s_window.flags  = SDL_WINDOW_FULLSCREEN_DESKTOP;
	s_window.in_use = 1;
	return &s_window;
}

/*
 * Present a framebuffer the caller owns, without a surface over it.
 *
 * The same path underneath as SDL_UpdateWindowSurface() - the backend takes a
 * pointer, a size and a byte pitch and does not care where the memory came from.
 */
void PSDL_PresentBuffer(const void *pixels, int w, int h, int pitch)
{
	if (pixels == NULL || w <= 0 || h <= 0)
		return;
	psdl_backend_video_present((const Uint8 *)pixels, w, h, pitch);
}

void PSDL_PresentSync(void)
{
	psdl_backend_video_sync();
}

void SDL_DestroyWindow(SDL_Window *window)
{
	if (window == NULL || !window->in_use)
		return;
	SDL_FreeSurface(window->surface);
	window->surface = NULL;
	window->in_use  = 0;
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
	return window ? window->surface : NULL;
}

int SDL_UpdateWindowSurface(SDL_Window *window)
{
	if (window == NULL || window->surface == NULL)
		return -1;

	SDL_Surface *s = window->surface;
	psdl_backend_video_present((const Uint8 *)s->pixels, s->w, s->h, s->pitch);
	return 0;
}

/*
 * Push only the rectangles that changed.
 *
 * Worth it because the panel link, not the CPU, is what a frame waits for: a full
 * 320x200 push is 17.4 ms at 125 MHz, and a narrow strip is a fraction of that.
 * The panel retains whatever it was last sent, so the parts left out keep showing
 * the previous frame - which is the property a wipe transition and a
 * single-buffered client both rely on.
 *
 * Rectangles are clipped to the surface; an empty or fully-clipped one is skipped
 * rather than refused. Passing NULL is the full frame, as SDL does.
 */
int SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
	if (window == NULL || window->surface == NULL)
		return -1;

	SDL_Surface *s = window->surface;
	if (rects == NULL || numrects <= 0)
		return SDL_UpdateWindowSurface(window);

	for (int i = 0; i < numrects; ++i) {
		int x = rects[i].x, y = rects[i].y;
		int w = rects[i].w, h = rects[i].h;
		if (x < 0) { w += x; x = 0; }
		if (y < 0) { h += y; y = 0; }
		if (x + w > s->w) w = s->w - x;
		if (y + h > s->h) h = s->h - y;
		if (w <= 0 || h <= 0)
			continue;
		psdl_backend_video_present_rect((const Uint8 *)s->pixels, s->pitch, x, y, w, h);
	}
	return 0;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
	if (w) *w = window ? window->w : PSDL_SCREEN_W;
	if (h) *h = window ? window->h : PSDL_SCREEN_H;
}

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
	return window ? window->flags : 0;
}

/* The panel has one mode. Report success so callers that toggle do not error. */
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
	(void)flags;
	if (window == NULL)
		return -1;
	window->flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	return 0;
}
