/*
 * A backend that does nothing, so picosdl can be compiled and exercised on a
 * PC.
 *
 * This is the seed of the "host" backend TODO.md calls for: the same
 * picosdl, with the presentation half replaced. Right now it only captures what
 * would have been pushed to the panel, which is enough for tests; giving it a
 * real SDL2 window later is a change to this file alone.
 */
#include <stdarg.h>
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

/* Partial pushes accumulate and are never cleared, as the real panel retains what
 * it was last sent - which is what a single-buffered client relies on. */
void psdl_backend_video_present_rect(const Uint8 *pixels, int pitch,
                                     int x, int y, int w, int h)
{
	for (int row = 0; row < h; ++row) {
		int dy = y + row;
		if (dy < 0 || dy >= PSDL_SCREEN_H)
			continue;
		int cx = x, cw = w;
		if (cx < 0) { cw += cx; cx = 0; }
		if (cx + cw > PSDL_SCREEN_W) cw = PSDL_SCREEN_W - cx;
		if (cw <= 0)
			continue;
		memcpy(host_last_frame + (size_t)dy * PSDL_SCREEN_W + cx,
		       pixels + (size_t)dy * pitch + cx, (size_t)cw);
	}
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

/* The host present copies synchronously, so nothing is ever still being read. */
int psdl_backend_video_buffer_busy(const void *pixels)
{
	(void)pixels;
	return 0;
}

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

/*
 * Master volume. Declared in SDL.h and implemented by every backend, so the
 * portable half can use it - psdl_audio_volume_key() does. Stored and returned
 * rather than applied: this backend's "DAC" is a capture buffer, and scaling what
 * the tests compare would make every one of them depend on the volume.
 */
static int s_master_volume = PSDL_DEFAULT_VOLUME;

void PSDL_SetMasterVolume(int volume)
{
	if (volume < 0)              volume = 0;
	if (volume > PSDL_VOLUME_UNITY) volume = PSDL_VOLUME_UNITY;
	s_master_volume = volume;
}

int PSDL_GetMasterVolume(void)
{
	return s_master_volume;
}

/*
 * The backend logger. No ring needed here: this backend is single-threaded and
 * nothing prints from a signal handler, so printf is already safe. On hardware it
 * is not, which is why the interface exists - see backend/pico/psdl_pico_log.c.
 */
void psdl_backend_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}
