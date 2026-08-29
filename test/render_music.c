#define _POSIX_C_SOURCE 200809L
/*
 * Render one of the game's Adlib tunes to a WAV on the host.
 *
 * The demo plays this music on the Pico, but there is no way to listen to that
 * from here. This runs the identical path - SDLPoP's midi.c driving Nuked OPL3
 * over the resources in flash - on the desktop, so the pipeline can be checked
 * before it ever reaches a board: is it silence, is it the right tune, is it
 * the right tempo.
 *
 * It also times the synthesis, which is the number PLAN.md's largest open
 * question turns on. The host figure is not the Pico figure, but a ratio of
 * host-seconds per audio-second bounds what to expect.
 *
 *   make -C picosdl/test music
 *   aplay /tmp/pop-main-theme.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psdl_internal.h"
#include "game_glue.h"

void  midi_callback(void *userdata, Uint8 *stream, int len);
void  stop_midi(void);
extern short midi_playing;

#define RATE    22050
#define SECONDS 30
#define BLOCK   256

static void write_wav(const char *path, const Sint16 *pcm, int frames)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		printf("cannot write %s\n", path);
		return;
	}
	int data_bytes = frames * 2 * (int)sizeof(Sint16);
	int chunk = 36 + data_bytes;
	int byte_rate = RATE * 2 * (int)sizeof(Sint16);
	short block_align = 2 * (short)sizeof(Sint16);
	short bits = 16, fmt = 1, channels = 2;
	int sub1 = 16, rate = RATE;

	fwrite("RIFF", 1, 4, f);          fwrite(&chunk, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);      fwrite(&sub1, 4, 1, f);
	fwrite(&fmt, 2, 1, f);            fwrite(&channels, 2, 1, f);
	fwrite(&rate, 4, 1, f);           fwrite(&byte_rate, 4, 1, f);
	fwrite(&block_align, 2, 1, f);    fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);          fwrite(&data_bytes, 4, 1, f);
	fwrite(pcm, 1, (size_t)data_bytes, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	int  id   = argc > 1 ? atoi(argv[1]) : GLUE_SOUND_MAIN_THEME;
	const char *out = argc > 2 ? argv[2] : "/tmp/pop-main-theme.wav";

	SDL_Init(SDL_INIT_AUDIO);

	/* midi.c reads its mixing rate from here. */
	static SDL_AudioSpec spec;
	spec.freq     = RATE;
	spec.format   = AUDIO_S16SYS;
	spec.channels = 2;
	spec.samples  = BLOCK;
	glue_set_audiospec(&spec);

	int size = 0;
	const void *res = glue_find_resource("MIDISND2.DAT", id, &size);
	if (res == NULL) {
		printf("resource %d not found in MIDISND2.DAT\n", id);
		return 1;
	}
	printf("resource %d: %d bytes\n", id, size);

	glue_play_music(res);
	if (!midi_playing) {
		printf("midi.c refused to play it\n");
		return 1;
	}

	int     frames = RATE * SECONDS;
	Sint16 *pcm    = calloc((size_t)frames * 2, sizeof(Sint16));

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	int silent_blocks = 0, total_blocks = 0;
	long long energy = 0;
	for (int done = 0; done < frames; done += BLOCK) {
		int n = (frames - done) < BLOCK ? frames - done : BLOCK;
		Sint16 *at = pcm + (size_t)done * 2;
		memset(at, 0, (size_t)n * 2 * sizeof(Sint16));
		midi_callback(NULL, (Uint8 *)at, n * 2 * (int)sizeof(Sint16));

		long long block_energy = 0;
		for (int i = 0; i < n * 2; ++i)
			block_energy += (long long)at[i] * at[i];
		if (block_energy == 0)
			silent_blocks++;
		energy += block_energy;
		total_blocks++;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
	                 (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	write_wav(out, pcm, frames);

	double rms = energy > 0 ? __builtin_sqrt((double)energy / (frames * 2.0)) : 0.0;
	printf("rendered %d s to %s\n", SECONDS, out);
	printf("  RMS level      %.0f (%.1f%% of full scale)%s\n",
	       rms, rms / 32768.0 * 100.0, rms < 1.0 ? "   <-- SILENCE" : "");
	printf("  silent blocks  %d of %d\n", silent_blocks, total_blocks);
	printf("  synthesis took %.3f s of host CPU for %d s of audio (%.1f%% of "
	       "real time on this machine)\n",
	       elapsed, SECONDS, elapsed / SECONDS * 100.0);
	printf("  still playing  %s\n", midi_playing ? "yes" : "no (tune ended)");

	stop_midi();
	free(pcm);
	return rms < 1.0;
}
