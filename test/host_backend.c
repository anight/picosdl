/*
 * A backend that does nothing, so picosdl can be compiled and exercised on a
 * PC.
 *
 * This is the seed of the "host" backend PLAN.md phase 3 calls for: the same
 * picosdl, with the presentation half replaced. Right now it only captures what
 * would have been pushed to the panel, which is enough for tests; giving it a
 * real SDL2 window later is a change to this file alone.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "psdl_internal.h"

/* What the last present would have shown, for tests to inspect. */
Uint8    host_last_frame[PSDL_SCREEN_W * PSDL_SCREEN_H];
int      host_present_count;
SDL_Color host_clut[256];

void psdl_backend_video_init(int w, int h)
{
	(void)w; (void)h;
	memset(host_last_frame, 0, sizeof(host_last_frame));
	host_present_count = 0;
}

void psdl_backend_video_present(const Uint8 *pixels, int w, int h, int pitch)
{
	for (int y = 0; y < h && y < PSDL_SCREEN_H; ++y)
		memcpy(host_last_frame + (size_t)y * PSDL_SCREEN_W,
		       pixels + (size_t)y * pitch,
		       (size_t)(w < PSDL_SCREEN_W ? w : PSDL_SCREEN_W));
	host_present_count++;
}

void psdl_backend_video_sync(void) { }

void psdl_backend_palette_set(int first, int ncolors, const SDL_Color *colors)
{
	for (int i = 0; i < ncolors; ++i)
		host_clut[first + i] = colors[i];
}

void psdl_backend_input_init(void) { }
void psdl_backend_input_poll(void) { }

void psdl_backend_audio_open(int freq, int channels, int block_frames)
{
	(void)freq; (void)channels; (void)block_frames;
}
void psdl_backend_audio_close(void) { }
void psdl_backend_audio_pause(int pause_on) { (void)pause_on; }
void psdl_backend_audio_lock(void) { }
void psdl_backend_audio_unlock(void) { }

Uint64 psdl_backend_ticks_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (Uint64)tv.tv_sec * 1000000u + (Uint64)tv.tv_usec;
}

Uint32 psdl_backend_ticks_ms(void)
{
	return (Uint32)(psdl_backend_ticks_us() / 1000u);
}

void psdl_backend_delay_ms(Uint32 ms) { (void)ms; }
