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
#include <stdio.h>

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

#if !PSDL_HAVE_AUDIO
	/*
	 * No audio hardware in this build, so say so and fail.
	 *
	 * Failing is the honest answer and the one SDL gives: opening an audio device
	 * is allowed to fail on a desktop too, so a well-written client already has a
	 * path for it. SDLPoP sets digi_unavailable and plays silently; picosdl's own
	 * demo prints the error and carries on. Succeeding and then never calling the
	 * callback would be worse - a client would have no way to know, and would spend
	 * its mixing budget on samples nothing consumes.
	 */
	(void)obtained;
	SDL_SetError("picosdl: built without audio (PICOSDL_AUDIO=OFF)");
	return -1;
#else
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
#endif /* PSDL_HAVE_AUDIO */
}

void SDL_CloseAudio(void)
{
	if (!s_open)
		return;
#if PSDL_HAVE_AUDIO
	psdl_backend_audio_pause(1);
	psdl_backend_audio_close();
#endif
	s_open   = 0;
	s_paused = 1;
}

void SDL_PauseAudio(int pause_on)
{
	if (!s_open)
		return;
	s_paused = pause_on ? 1 : 0;
#if PSDL_HAVE_AUDIO
	psdl_backend_audio_pause(s_paused);
#endif
}

SDL_AudioStatus SDL_GetAudioStatus(void)
{
	if (!s_open)
		return SDL_AUDIO_STOPPED;
	return s_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

#if PSDL_HAVE_AUDIO
void SDL_LockAudio(void)   { psdl_backend_audio_lock(); }
void SDL_UnlockAudio(void) { psdl_backend_audio_unlock(); }
#else
/* Nothing mixes on another core, so there is nothing to lock against. Present
 * because clients bracket their state changes with these unconditionally. */
void SDL_LockAudio(void)   { }
void SDL_UnlockAudio(void) { }
#endif

#if PSDL_HAVE_AUDIO
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

#endif /* PSDL_HAVE_AUDIO */

void psdl_audio_init(void)
{
	s_open   = 0;
	s_paused = 1;
	memset(&s_spec, 0, sizeof(s_spec));
}

#if !PSDL_HAVE_AUDIO
/*
 * The master volume with no mixer to apply it.
 *
 * Kept rather than dropped because it is public API: a client may read or set it
 * whatever the build, and having it vanish would mean every caller needs its own
 * #if. The value is remembered and does nothing, which is the truthful behaviour
 * for a volume control with no output.
 *
 * With audio on, these live in the backend beside the mixer that uses them.
 */
static int s_silent_volume = PSDL_DEFAULT_VOLUME;

void PSDL_SetMasterVolume(int volume)
{
	if (volume < 0)                 volume = 0;
	if (volume > PSDL_VOLUME_UNITY) volume = PSDL_VOLUME_UNITY;
	s_silent_volume = volume;
}

int PSDL_GetMasterVolume(void) { return s_silent_volume; }
#endif /* !PSDL_HAVE_AUDIO */

/* --------------------------------------------------- volume keys */

/*
 * The keyboard's volume and mute keys.
 *
 * SDL2 does not do this, and on a desktop it would be wrong to: the window
 * manager takes the media keys and the application never sees them. There is no
 * window manager here, so the library stands in for it - otherwise these keys
 * reach a game that has no idea what to do with them. SDLPoP, for one, lists all
 * four in its key handler purely to ignore them.
 *
 * The step is a 3 dB ladder rather than a fixed increment, because the volume is
 * a linear multiplier and even steps in it sound wildly uneven: 25 -> 41 is an
 * obvious jump, 205 -> 221 is inaudible. Doubling the multiplier is +6 dB, so a
 * factor of the square root of two per press is +3 dB, which is about the
 * smallest change that reads as "louder".
 *
 * There is no stored index. Up takes the lowest rung above the current volume and
 * down the highest below it, so the ladder works from wherever the volume happens
 * to be - a client's PSDL_DEFAULT_VOLUME override, or a programmatic
 * PSDL_SetMasterVolume() - without a table entry having to exist for that value.
 */
static const short s_volume_ladder[] = {
	4, 6, 8, 11, 16, 23, 32, 45, 64, 91, 128, 181, PSDL_VOLUME_UNITY
};
#define PSDL_VOLUME_RUNGS ((int)(sizeof s_volume_ladder / sizeof s_volume_ladder[0]))

/* What to come back to when unmuting something that was already silent. */
#define PSDL_UNMUTE_FALLBACK PSDL_DEFAULT_VOLUME

static int s_premute_volume = -1;

static void volume_step(int up)
{
	int v = PSDL_GetMasterVolume();
	int n = v;

	if (up) {
		n = PSDL_VOLUME_UNITY;
		for (int i = 0; i < PSDL_VOLUME_RUNGS; ++i)
			if (s_volume_ladder[i] > v) { n = s_volume_ladder[i]; break; }
	} else {
		n = 0;
		for (int i = PSDL_VOLUME_RUNGS - 1; i >= 0; --i)
			if (s_volume_ladder[i] < v) { n = s_volume_ladder[i]; break; }
	}

	/* Stepping down to silence is a mute by another route; remember where from,
	 * so that unmuting has somewhere to go. */
	if (n == 0 && v > 0)
		s_premute_volume = v;
	else if (n > 0)
		s_premute_volume = -1;

	PSDL_SetMasterVolume(n);
	psdl_backend_log("picosdl: volume %d/%d\n", n, PSDL_VOLUME_UNITY);
}

static void volume_mute_toggle(void)
{
	int v = PSDL_GetMasterVolume();

	if (v > 0) {
		s_premute_volume = v;
		PSDL_SetMasterVolume(0);
		psdl_backend_log("picosdl: muted (was %d/%d)\n", v, PSDL_VOLUME_UNITY);
		return;
	}

	int back = s_premute_volume > 0 ? s_premute_volume : PSDL_UNMUTE_FALLBACK;
	s_premute_volume = -1;
	PSDL_SetMasterVolume(back);
	psdl_backend_log("picosdl: unmuted, volume %d/%d\n", back, PSDL_VOLUME_UNITY);
}

/*
 * Returns non-zero if the key was a volume key and has been dealt with, in which
 * case the caller must not queue an event for it. Both the press and the release
 * are swallowed, so a game never sees a lone SDL_KEYUP it did not ask for.
 */
int psdl_audio_volume_key(SDL_Scancode scancode, int pressed)
{
	switch (scancode) {
	case SDL_SCANCODE_VOLUMEUP:
		if (pressed) volume_step(1);
		return 1;
	case SDL_SCANCODE_VOLUMEDOWN:
		if (pressed) volume_step(0);
		return 1;
	case SDL_SCANCODE_MUTE:
	case SDL_SCANCODE_AUDIOMUTE:
		if (pressed) volume_mute_toggle();
		return 1;
	default:
		return 0;
	}
}
