/*
 * Init, teardown, and the error string.
 *
 * The error buffer is a fixed static one - there is no heap, and SDL's own
 * SDL_GetError() returns a pointer with the same "valid until the next error"
 * lifetime anyway.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "psdl_internal.h"

static char s_error[192];
static int  s_inited;

int SDL_Init(Uint32 flags)
{
	if (s_inited) {
		return SDL_InitSubSystem(flags);
	}

	psdl_palette_init();
	psdl_surface_init();
	psdl_events_init();
	psdl_timer_init();
	psdl_audio_init();

	if (flags & SDL_INIT_VIDEO)
		psdl_video_init();
	if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_VIDEO))
		psdl_backend_input_init();

	s_inited = 1;
	s_error[0] = '\0';
	return 0;
}

int SDL_InitSubSystem(Uint32 flags)
{
	if (!s_inited)
		return SDL_Init(flags);
	if (flags & SDL_INIT_VIDEO)
		psdl_video_init();
	if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER))
		psdl_backend_input_init();
	return 0;
}

void SDL_Quit(void)
{
	SDL_CloseAudio();
	s_inited = 0;
}

const char *SDL_GetError(void)
{
	return s_error;
}

void SDL_SetError(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_error, sizeof(s_error), fmt, ap);
	va_end(ap);
}

void SDL_ClearError(void)
{
	s_error[0] = '\0';
}

/*
 * Running out of a fixed pool is a bug in the caller's sizing, not a runtime
 * condition to handle - returning NULL would just move the crash somewhere less
 * informative. Say what ran out and stop.
 */
void psdl_panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);

#ifdef PICO_ON_DEVICE
	for (;;) { }
#else
	abort();
#endif
}
