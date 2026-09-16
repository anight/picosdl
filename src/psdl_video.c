/*
 * The window and the screen surface.
 *
 * There is one window, it is always fullscreen, and its surface is a 320x200
 * 8bpp framebuffer from the screen pool. SDL_UpdateWindowSurface hands that
 * buffer to the backend, which on hardware kicks a DMA chain and returns
 * immediately - the wait happens at the start of the next present, so drawing
 * overlaps with the panel push.
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

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	(void)title; (void)x; (void)y; (void)w; (void)h;

	psdl_video_init();

	if (s_window.in_use)
		return &s_window;

	s_window.surface = SDL_CreateRGBSurface(0, PSDL_SCREEN_W, PSDL_SCREEN_H, 8, 0, 0, 0, 0);
	PSDL_ASSERT(s_window.surface != NULL, "could not create the window surface");

	s_window.w      = PSDL_SCREEN_W;
	s_window.h      = PSDL_SCREEN_H;
	s_window.flags  = flags | SDL_WINDOW_FULLSCREEN_DESKTOP;
	s_window.in_use = 1;
	return &s_window;
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
