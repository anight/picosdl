/*
 * Audio.
 *
 * SDL's model is a pull callback, and that maps cleanly onto a DMA-fed I2S
 * output: the backend calls psdl_audio_render() whenever a block needs filling,
 * and that calls the client's SDL_AudioCallback.
 *
 * The callback runs on whichever core the backend chose to mix on - core 1 on
 * the Pico, so a slow synth cannot stall the game loop. SDL_LockAudio therefore
 * has to be a real cross-core lock, not a no-op; it is what stops the game from
 * swapping out a sound pointer while the mixer is reading it.
 *
 * Only AUDIO_S16SYS stereo is supported, because it is what SDLPoP asks for.
 */
#include "psdl_internal.h"

static SDL_AudioSpec  s_spec;
static int            s_open;
static int            s_paused = 1;

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
	if (desired == NULL)
		return -1;
	if (s_open) {
		SDL_SetError("picosdl: audio already open");
		return -1;
	}

	if (desired->format != AUDIO_S16SYS) {
		SDL_SetError("picosdl: only AUDIO_S16SYS is supported");
		return -1;
	}
	if (desired->channels != 2) {
		SDL_SetError("picosdl: only stereo output is supported");
		return -1;
	}

	s_spec          = *desired;
	s_spec.silence  = 0;
	s_spec.samples  = PSDL_AUDIO_BLOCK_FRAMES;
	s_spec.size     = (Uint32)s_spec.samples * s_spec.channels * sizeof(Sint16);

	psdl_backend_audio_open(s_spec.freq, s_spec.channels, PSDL_AUDIO_BLOCK_FRAMES);

	s_open   = 1;
	s_paused = 1;

	if (obtained != NULL)
		*obtained = s_spec;
	return 0;
}

void SDL_CloseAudio(void)
{
	if (!s_open)
		return;
	psdl_backend_audio_pause(1);
	psdl_backend_audio_close();
	s_open   = 0;
	s_paused = 1;
}

void SDL_PauseAudio(int pause_on)
{
	if (!s_open)
		return;
	s_paused = pause_on ? 1 : 0;
	psdl_backend_audio_pause(s_paused);
}

SDL_AudioStatus SDL_GetAudioStatus(void)
{
	if (!s_open)
		return SDL_AUDIO_STOPPED;
	return s_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

void SDL_LockAudio(void)   { psdl_backend_audio_lock(); }
void SDL_UnlockAudio(void) { psdl_backend_audio_unlock(); }

/*
 * Called by the backend to fill one block. Always writes every frame: an
 * underrun that leaves stale data in the buffer is a loud repeating buzz,
 * whereas silence is merely a gap.
 */
void psdl_audio_render(Sint16 *buf, int frames)
{
	if (!s_open || s_paused || s_spec.callback == NULL) {
		memset(buf, 0, (size_t)frames * 2 * sizeof(Sint16));
		return;
	}

	psdl_backend_audio_lock();
	s_spec.callback(s_spec.userdata, (Uint8 *)buf,
	                frames * (int)s_spec.channels * (int)sizeof(Sint16));
	psdl_backend_audio_unlock();
}

void psdl_audio_init(void)
{
	s_open   = 0;
	s_paused = 1;
	memset(&s_spec, 0, sizeof(s_spec));
}
