/*
 * Tests for the parts of picosdl that are pure logic: clipping, the blitters,
 * the arena's LIFO discipline, and the event ring.
 *
 * These are the places bugs actually live. A mirrored blit that is clipped on
 * its left edge has to read a different source column than an unclipped one, an
 * off-by-one there is invisible until a sprite walks off the side of the screen,
 * and none of it needs hardware to check.
 *
 *   cc -I include -I src -o /tmp/t test/test_picosdl.c test/host_backend.c \
 *      src/psdl_*.c && /tmp/t
 */
#include <stdio.h>
#include <string.h>

#include "psdl_internal.h"

static int tests_run, tests_failed;

#define CHECK(cond, ...) do { \
		tests_run++; \
		if (!(cond)) { \
			tests_failed++; \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void section(const char *name)
{
	printf("%s\n", name);
}

/* A surface filled with a recognisable pattern: pixel (x,y) = 1 + x + y*16. */
static SDL_Surface *make_pattern(int w, int h)
{
	SDL_Surface *s = SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			((Uint8 *)s->pixels)[y * s->pitch + x] = (Uint8)(1 + x + y * 16);
	return s;
}

static Uint8 px(SDL_Surface *s, int x, int y)
{
	return ((Uint8 *)s->pixels)[y * s->pitch + x];
}

/* --------------------------------------------------------------- fill */

static void test_fill(void)
{
	section("fill");

	SDL_Surface *dst = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);

	SDL_Rect r = { 2, 3, 4, 5 };
	SDL_FillRect(dst, &r, 42);
	CHECK(px(dst, 2, 3) == 42, "top-left of the rect not filled");
	CHECK(px(dst, 5, 7) == 42, "bottom-right of the rect not filled");
	CHECK(px(dst, 1, 3) == 0,  "filled outside the rect (left)");
	CHECK(px(dst, 6, 3) == 0,  "filled outside the rect (right)");
	CHECK(px(dst, 2, 8) == 0,  "filled outside the rect (below)");

	/* A rect hanging off the edges must clip, not wrap or overrun. The visible
	 * part of {-4,-4,8,8} is x 0..3, y 0..3, so (4,0) is just past its right
	 * edge on a row the earlier fill never touched. */
	SDL_Rect over = { -4, -4, 8, 8 };
	SDL_FillRect(dst, &over, 7);
	CHECK(px(dst, 0, 0) == 7, "clipped fill did not reach the corner");
	CHECK(px(dst, 3, 3) == 7, "clipped fill did not reach its far corner");
	CHECK(px(dst, 4, 0) == 0, "clipped fill went too far");

	/* Honour the clip rect. */
	SDL_Rect clip = { 8, 8, 4, 4 };
	SDL_SetClipRect(dst, &clip);
	SDL_FillRect(dst, NULL, 99);
	SDL_SetClipRect(dst, NULL);
	CHECK(px(dst, 8, 8)   == 99, "fill did not honour the clip rect");
	CHECK(px(dst, 11, 11) == 99, "fill did not honour the clip rect");
	CHECK(px(dst, 7, 8)   != 99, "fill escaped the clip rect");
	CHECK(px(dst, 12, 8)  != 99, "fill escaped the clip rect");

	SDL_FreeSurface(dst);
}

/* -------------------------------------------------------------- blits */

static void test_blit_opaque_and_keyed(void)
{
	section("blit: opaque and colour-keyed");

	SDL_Surface *src = make_pattern(4, 4);
	SDL_Surface *dst = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	SDL_FillRect(dst, NULL, 200);

	SDL_Rect at = { 2, 2, 0, 0 };
	SDL_BlitSurface(src, NULL, dst, &at);
	CHECK(px(dst, 2, 2) == 1,  "opaque blit: wrong first pixel (%d)", px(dst, 2, 2));
	CHECK(px(dst, 5, 5) == 1 + 3 + 3 * 16, "opaque blit: wrong last pixel");
	CHECK(at.w == 4 && at.h == 4, "dstrect not updated with the blitted size");

	/* With a colour key, pixels equal to the key are left alone. */
	((Uint8 *)src->pixels)[0] = 77;
	SDL_SetColorKey(src, SDL_TRUE, 77);
	SDL_FillRect(dst, NULL, 200);
	SDL_Rect at2 = { 2, 2, 0, 0 };
	SDL_BlitSurface(src, NULL, dst, &at2);
	CHECK(px(dst, 2, 2) == 200, "keyed blit wrote a transparent pixel");
	CHECK(px(dst, 3, 2) == 2,   "keyed blit dropped an opaque pixel (%d)",
	      px(dst, 3, 2));   /* source (1,0) = 1 + 1 + 0*16 */

	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}

static void test_blit_clipping(void)
{
	section("blit: clipping at every edge");

	SDL_Surface *src = make_pattern(4, 4);
	SDL_Surface *dst = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);

	/* Off the top-left: only the bottom-right of the source survives, and the
	 * surviving pixels must be the *right* ones - source column 2 lands on
	 * destination column 0. */
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect tl = { -2, -2, 0, 0 };
	SDL_BlitSurface(src, NULL, dst, &tl);
	CHECK(px(dst, 0, 0) == 1 + 2 + 2 * 16,
	      "clipped-at-origin blit read the wrong source pixel (%d)", px(dst, 0, 0));

	/* Off the bottom-right: partial, and nothing may be written past the edge
	 * (which on a real framebuffer would be the next row). */
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect br = { 14, 14, 0, 0 };
	SDL_BlitSurface(src, NULL, dst, &br);
	CHECK(px(dst, 14, 14) == 1, "bottom-right blit missing");
	CHECK(px(dst, 15, 15) == 1 + 1 + 1 * 16, "bottom-right blit wrong");
	CHECK(px(dst, 0, 15) == 0, "blit wrapped past the right edge");

	/* Entirely off-screen: no writes at all, and dstrect zeroed. */
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect off = { 100, 100, 0, 0 };
	SDL_BlitSurface(src, NULL, dst, &off);
	CHECK(off.w == 0 && off.h == 0, "fully clipped blit did not zero dstrect");
	int clean = 1;
	for (int i = 0; i < 16 * 16; ++i)
		if (((Uint8 *)dst->pixels)[i] != 0)
			clean = 0;
	CHECK(clean, "fully clipped blit wrote something");

	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}

static void test_blit_mirrored(void)
{
	section("blit: mirrored");

	SDL_Surface *src = make_pattern(4, 1);   /* 1, 2, 3, 4 */
	SDL_Surface *dst = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);

	SDL_FillRect(dst, NULL, 0);
	SDL_Rect at = { 4, 0, 0, 0 };
	PSDL_BlitMirrored(src, NULL, dst, &at);
	CHECK(px(dst, 4, 0) == 4 && px(dst, 5, 0) == 3 &&
	      px(dst, 6, 0) == 2 && px(dst, 7, 0) == 1,
	      "mirrored blit not reversed: %d %d %d %d",
	      px(dst, 4, 0), px(dst, 5, 0), px(dst, 6, 0), px(dst, 7, 0));

	/*
	 * Clipped on the left. Destination column 0 is the second-from-last
	 * destination column of the unclipped blit, so it must show source pixel 2,
	 * not source pixel 4. Getting this wrong is the classic mirrored-clip bug:
	 * it looks fine until a sprite walks off the left of the screen.
	 */
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect left = { -2, 0, 0, 0 };
	PSDL_BlitMirrored(src, NULL, dst, &left);
	CHECK(px(dst, 0, 0) == 2 && px(dst, 1, 0) == 1,
	      "mirrored blit clipped on the left read wrong columns: %d %d",
	      px(dst, 0, 0), px(dst, 1, 0));

	/* Clipped on the right: the visible part is the first half of the
	 * reversed sprite, i.e. source pixels 4 and 3. */
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect right = { 14, 0, 0, 0 };
	PSDL_BlitMirrored(src, NULL, dst, &right);
	CHECK(px(dst, 14, 0) == 4 && px(dst, 15, 0) == 3,
	      "mirrored blit clipped on the right read wrong columns: %d %d",
	      px(dst, 14, 0), px(dst, 15, 0));

	/* Mirroring must respect the colour key too. */
	((Uint8 *)src->pixels)[3] = 0;
	SDL_SetColorKey(src, SDL_TRUE, 0);
	SDL_FillRect(dst, NULL, 200);
	SDL_Rect keyed = { 4, 0, 0, 0 };
	PSDL_BlitMirrored(src, NULL, dst, &keyed);
	CHECK(px(dst, 4, 0) == 200, "mirrored keyed blit wrote a transparent pixel");
	CHECK(px(dst, 5, 0) == 3,   "mirrored keyed blit dropped an opaque pixel");

	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}

static void test_blit_xor_and_offset(void)
{
	section("blit: xor and palette offset");

	SDL_Surface *src = make_pattern(2, 1);   /* 1, 2 */
	SDL_Surface *dst = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);

	SDL_FillRect(dst, NULL, 0xF0);
	SDL_Rect at = { 0, 0, 0, 0 };
	PSDL_BlitXor(src, NULL, dst, &at);
	CHECK(px(dst, 0, 0) == (0xF0 ^ 1), "xor blit wrong (%d)", px(dst, 0, 0));
	CHECK(px(dst, 1, 0) == (0xF0 ^ 2), "xor blit wrong");

	/* The offset blit is how a sprite set whose 16 colours live at a base
	 * index in the global palette gets drawn. */
	SDL_SetColorKey(src, SDL_TRUE, 0);
	SDL_FillRect(dst, NULL, 0);
	SDL_Rect at2 = { 0, 0, 0, 0 };
	PSDL_BlitOffset(src, NULL, dst, &at2, 64);
	CHECK(px(dst, 0, 0) == 65 && px(dst, 1, 0) == 66,
	      "offset blit wrong: %d %d", px(dst, 0, 0), px(dst, 1, 0));

	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}

/* -------------------------------------------------------------- arena */

static void test_arena_lifo(void)
{
	section("arena: LIFO discipline");

	/* Freed in reverse order - the normal peel pattern - the arena must end up
	 * exactly where it started, however many rounds we do. */
	for (int round = 0; round < 100; ++round) {
		SDL_Surface *a = SDL_CreateRGBSurface(0, 40, 30, 8, 0, 0, 0, 0);
		SDL_Surface *b = SDL_CreateRGBSurface(0, 50, 40, 8, 0, 0, 0, 0);
		SDL_Surface *c = SDL_CreateRGBSurface(0, 60, 50, 8, 0, 0, 0, 0);
		if (a == NULL || b == NULL || c == NULL) {
			CHECK(0, "arena exhausted on round %d - LIFO frees are not "
			         "reclaiming", round);
			return;
		}
		SDL_FreeSurface(c);
		SDL_FreeSurface(b);
		SDL_FreeSurface(a);
	}
	CHECK(1, "LIFO rounds completed");

	/* Freed out of order, the arena may not reclaim until the top is dead -
	 * but it must never hand the same memory to two live surfaces. */
	SDL_Surface *a = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	SDL_Surface *b = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	Uint8 *a_pixels = a->pixels;
	SDL_FreeSurface(a);                       /* out of order */
	SDL_Surface *c = SDL_CreateRGBSurface(0, 16, 16, 8, 0, 0, 0, 0);
	CHECK(c->pixels != a_pixels || b == NULL,
	      "arena reused a still-live allocation's memory");
	CHECK(c->pixels != b->pixels, "arena handed out b's memory again");
	SDL_FreeSurface(c);
	SDL_FreeSurface(b);
}

static void test_client_owned_window(void)
{
	section("client-owned window");

	/* picosdl allocates no pixels: the window wraps memory we brought. */
	static Uint8 canvas[PSDL_SCREEN_W * PSDL_SCREEN_H];
	for (size_t i = 0; i < sizeof(canvas); ++i)
		canvas[i] = (Uint8)i;

	SDL_Window *win = PSDL_CreateWindow(canvas, PSDL_SCREEN_W, PSDL_SCREEN_H,
	                                    PSDL_SCREEN_W);
	CHECK(win != NULL, "PSDL_CreateWindow should accept a caller buffer");

	SDL_Surface *s = SDL_GetWindowSurface(win);
	CHECK(s != NULL, "window should have a surface");
	CHECK(s != NULL && s->pixels == canvas,
	      "the window surface must wrap the caller's memory, not a copy");
	CHECK(s != NULL && s->pitch == PSDL_SCREEN_W, "window surface pitch wrong");
	CHECK(s != NULL && s->w == PSDL_SCREEN_W && s->h == PSDL_SCREEN_H,
	      "window surface size wrong");

	/* External pixels: freeing releases the header and must not touch them. */
	CHECK(s != NULL && (s->flags & PSDL_SURF_REGION) == PSDL_SURF_EXTERN,
	      "a client buffer should be PSDL_SURF_EXTERN");

	/* Drawing through the surface reaches the caller's array. */
	SDL_Rect r = { 0, 0, 4, 1 };
	SDL_FillRect(s, &r, 0xAB);
	CHECK(canvas[0] == 0xAB && canvas[3] == 0xAB && canvas[4] == 4,
	      "FillRect through the window surface should write the caller's buffer");

	/* A present of our own buffer reaches the backend. */
	extern int host_present_count;
	int before = host_present_count;
	SDL_UpdateWindowSurface(win);
	CHECK(host_present_count == before + 1, "UpdateWindowSurface should present");

	PSDL_PresentBuffer(canvas, PSDL_SCREEN_W, PSDL_SCREEN_H, PSDL_SCREEN_W);
	CHECK(host_present_count == before + 2, "PresentBuffer should present");

	/* Bad geometry is refused rather than half-accepted. */
	SDL_DestroyWindow(win);
	CHECK(PSDL_CreateWindow(NULL, 8, 8, 8) == NULL, "NULL pixels should fail");
	CHECK(PSDL_CreateWindow(canvas, 8, 8, 2) == NULL,
	      "a pitch narrower than the row should fail");
}

static void test_events(void)
{
	section("events");

	psdl_events_init();

	psdl_push_key(SDL_SCANCODE_A, 1, KMOD_LSHIFT);
	const Uint8 *keys = SDL_GetKeyboardState(NULL);
	CHECK(keys[SDL_SCANCODE_A] == 1, "key state not set on press");

	SDL_Event ev;
	CHECK(SDL_PollEvent(&ev) == 1, "no event dequeued");
	CHECK(ev.type == SDL_KEYDOWN, "wrong event type");
	CHECK(ev.key.keysym.scancode == SDL_SCANCODE_A, "wrong scancode");
	CHECK(ev.key.keysym.mod == KMOD_LSHIFT, "modifiers lost");

	psdl_push_key(SDL_SCANCODE_A, 0, 0);
	CHECK(keys[SDL_SCANCODE_A] == 0, "key state not cleared on release");
	CHECK(SDL_PollEvent(&ev) == 1 && ev.type == SDL_KEYUP, "no key-up event");
	CHECK(SDL_PollEvent(&ev) == 0, "queue should be empty");

	/* Overflow must drop and count, not corrupt the ring. */
	unsigned before = PSDL_DroppedEvents();
	for (int i = 0; i < PSDL_EVENT_QUEUE_LEN + 8; ++i)
		psdl_push_key(SDL_SCANCODE_B, 1, 0);
	CHECK(PSDL_DroppedEvents() > before, "overflow was not counted");

	int drained = 0;
	while (SDL_PollEvent(&ev))
		drained++;
	CHECK(drained == PSDL_EVENT_QUEUE_LEN - 1,
	      "ring should hold %d events, drained %d",
	      PSDL_EVENT_QUEUE_LEN - 1, drained);

	/* A scancode outside the table must not write past the key state array. */
	psdl_push_key((SDL_Scancode)9999, 1, 0);
	CHECK(SDL_PollEvent(&ev) == 0, "out-of-range scancode was queued");
}

/* ------------------------------------------------------------ palette */

static void test_palette(void)
{
	section("palette");

	extern SDL_Color host_clut[256];

	SDL_Color c[4] = {
		{ 0x10, 0x20, 0x30, 255 },
		{ 0x40, 0x50, 0x60, 255 },
		{ 0x70, 0x80, 0x90, 255 },
		{ 0xA0, 0xB0, 0xC0, 255 },
	};
	SDL_SetPaletteColors(PSDL_GlobalPalette(), c, 8, 4);
	CHECK(host_clut[8].r == 0x10 && host_clut[11].b == 0xC0,
	      "palette did not reach the backend CLUT");

	/* Writes past the end must be trimmed, not wrapped. */
	SDL_SetPaletteColors(PSDL_GlobalPalette(), c, 254, 4);
	CHECK(host_clut[255].r == c[1].r, "trailing palette write not clamped");

	/* SDL_MapRGB is a nearest-index search on an indexed target. */
	Uint32 idx = SDL_MapRGB(NULL, 0x40, 0x50, 0x60);
	CHECK(idx == 9, "SDL_MapRGB found index %u, expected 9", (unsigned)idx);
}

/* ------------------------------------------------------------- timers */

static Uint32 timer_fires;
static Uint32 timer_cb(Uint32 interval, void *param)
{
	(void)param;
	timer_fires++;
	return interval;
}

static void test_timers(void)
{
	section("timers");

	void psdl_timer_service(void);

	psdl_timer_init();
	SDL_TimerID id = SDL_AddTimer(1, timer_cb, NULL);
	CHECK(id > 0, "SDL_AddTimer failed");

	timer_fires = 0;
	Uint32 start = SDL_GetTicks();
	while (SDL_GetTicks() - start < 20)
		psdl_timer_service();
	CHECK(timer_fires > 0, "1 ms timer never fired over 20 ms");

	CHECK(SDL_RemoveTimer(id) == SDL_TRUE, "SDL_RemoveTimer failed");
	Uint32 after = timer_fires;
	for (int i = 0; i < 1000; ++i)
		psdl_timer_service();
	CHECK(timer_fires == after, "timer fired after removal");
}

/* --------------------------------------------------------------- main */

int main(void)
{
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

	test_fill();
	test_blit_opaque_and_keyed();
	test_blit_clipping();
	test_blit_mirrored();
	test_blit_xor_and_offset();
	test_arena_lifo();
	test_client_owned_window();
	test_events();
	test_palette();
	test_timers();

	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	PSDL_ReportMemory();
	return tests_failed != 0;
}
