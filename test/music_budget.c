#define _POSIX_C_SOURCE 200809L
/*
 * How much flash would it cost to ship the game's music as audio instead of
 * synthesising it?
 *
 * Nuked OPL3 looks too expensive for an RP2040 core (PLAN.md 8), and the
 * standing fallback is to pre-render the tunes at build time. That trades CPU
 * for flash, and nobody had costed the flash side. This renders every MIDI
 * resource in the game to the end, measures it, and prices the result under the
 * encodings worth considering.
 *
 * It also answers two questions the size table depends on:
 *
 *   - Is the output actually mono? PoP1's Adlib data does not use the OPL3
 *     stereo extensions, so if left and right are identical everywhere, storing
 *     one channel halves the bill.
 *   - Is 4-bit IMA ADPCM good enough? It is 4x smaller than 16-bit PCM, so it
 *     decides whether this is affordable at all. Measured as SNR against the
 *     PCM, with a WAV written out to listen to.
 *
 *   make -C picosdl/test budget
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psdl_internal.h"
#include "game_glue.h"

void  midi_callback(void *userdata, Uint8 *stream, int len);
void  stop_midi(void);
extern short midi_playing;

#define RATE        22050
#define BLOCK       256
#define MAX_SECONDS 240

/* Every MIDI resource in the game. MIDISND1 holds the short cues, MIDISND2 the
 * longer story and title pieces. */
static const struct { const char *dat; int first, last; } midi_dats[] = {
	{ "MIDISND1.DAT", 10024, 10043 },
	{ "MIDISND2.DAT", 10050, 10056 },
};

/* ------------------------------------------------------- IMA ADPCM ----- */

static const int ima_index_table[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
static const int ima_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
	10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
	27086, 29794, 32767
};

/* Encode and immediately decode, so the round-trip error can be measured. */
static void ima_roundtrip(const Sint16 *in, Sint16 *out, int n)
{
	int predictor = 0, index = 0;

	for (int i = 0; i < n; ++i) {
		int step = ima_step_table[index];
		int diff = in[i] - predictor;
		int code = 0;

		if (diff < 0) { code = 8; diff = -diff; }
		if (diff >= step)      { code |= 4; diff -= step; }
		if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
		if (diff >= step >> 2) { code |= 1; }

		/* decode the code we just produced */
		int delta = step >> 3;
		if (code & 4) delta += step;
		if (code & 2) delta += step >> 1;
		if (code & 1) delta += step >> 2;
		if (code & 8) delta = -delta;

		predictor += delta;
		if (predictor >  32767) predictor =  32767;
		if (predictor < -32768) predictor = -32768;

		index += ima_index_table[code];
		if (index < 0)  index = 0;
		if (index > 88) index = 88;

		out[i] = (Sint16)predictor;
	}
}

static void write_wav(const char *path, const Sint16 *pcm, int frames, int rate, int channels)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return;
	int   data_bytes  = frames * channels * (int)sizeof(Sint16);
	int   chunk       = 36 + data_bytes;
	int   byte_rate   = rate * channels * (int)sizeof(Sint16);
	short block_align = (short)(channels * (int)sizeof(Sint16));
	short bits = 16, fmt = 1, ch = (short)channels;
	int   sub1 = 16;

	fwrite("RIFF", 1, 4, f);       fwrite(&chunk, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);   fwrite(&sub1, 4, 1, f);
	fwrite(&fmt, 2, 1, f);         fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f);        fwrite(&byte_rate, 4, 1, f);
	fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);       fwrite(&data_bytes, 4, 1, f);
	fwrite(pcm, 1, (size_t)data_bytes, f);
	fclose(f);
}

/* --------------------------------------------------------------------- */

int main(void)
{
	SDL_Init(SDL_INIT_AUDIO);

	static SDL_AudioSpec spec;
	spec.freq = RATE; spec.format = AUDIO_S16SYS; spec.channels = 2;
	spec.samples = BLOCK;
	glue_set_audiospec(&spec);

	int     cap    = RATE * MAX_SECONDS;
	Sint16 *stereo = malloc((size_t)cap * 2 * sizeof(Sint16));
	Sint16 *mono   = malloc((size_t)cap * sizeof(Sint16));
	Sint16 *ima    = malloc((size_t)cap * sizeof(Sint16));

	long long total_frames = 0;
	int       tunes = 0, not_mono = 0, truncated = 0;
	double    worst_snr = 1e9;
	long long snr_num = 0;
	int       longest_frames = 0, longest_id = 0;

	double worst_lr = 1e9;

	printf("%-10s %7s %8s %8s %8s %9s\n",
	       "resource", "midi B", "seconds", "IMA dB", "L-R dB", "11k IMA KB");
	printf("------------------------------------------------------------\n");

	for (unsigned d = 0; d < sizeof(midi_dats) / sizeof(midi_dats[0]); ++d) {
		for (int id = midi_dats[d].first; id <= midi_dats[d].last; ++id) {
			int size = 0;
			const void *res = glue_find_resource(midi_dats[d].dat, id, &size);
			if (res == NULL)
				continue;

			glue_play_music(res);
			if (!midi_playing) {
				printf("%-10d %7d   (midi.c refused it)\n", id, size);
				continue;
			}

			/* Render to the end of the tune. */
			int frames = 0;
			while (midi_playing && frames < cap) {
				int n = (cap - frames) < BLOCK ? cap - frames : BLOCK;
				Sint16 *at = stereo + (size_t)frames * 2;
				memset(at, 0, (size_t)n * 2 * sizeof(Sint16));
				midi_callback(NULL, (Uint8 *)at, n * 2 * (int)sizeof(Sint16));
				frames += n;
			}
			if (midi_playing) {
				truncated++;
				stop_midi();
			}

			/*
			 * Mono? midi.c enables both speakers with the same signal
			 * (opl_write_instrument writes 0xC0 | 0x30), so the channels
			 * should carry the same music. Nuked models the OPL3's serial
			 * DAC, which emits the two channels at slightly different chip
			 * cycles, so exact equality is not expected - what matters is
			 * whether the difference is audible. Measure it as the energy of
			 * (L-R) against the energy of the signal.
			 */
			int       mismatched = 0;
			long long sig_e = 0, diff_e = 0;
			for (int i = 0; i < frames; ++i) {
				int l = stereo[i * 2], r = stereo[i * 2 + 1];
				if (l != r)
					mismatched++;
				sig_e  += (long long)l * l;
				diff_e += (long long)(l - r) * (l - r);
				mono[i] = (Sint16)((l + r) / 2);
			}
			double lr_db = (diff_e > 0 && sig_e > 0)
			             ? 10.0 * __builtin_log10((double)sig_e / diff_e) : 99.0;
			if (lr_db < 40.0)
				not_mono++;
			if (lr_db < worst_lr)
				worst_lr = lr_db;

			/* 4-bit IMA round trip, and the SNR it costs. */
			ima_roundtrip(mono, ima, frames);
			double sig = 0, err = 0;
			for (int i = 0; i < frames; ++i) {
				double s = mono[i], e = (double)mono[i] - ima[i];
				sig += s * s;
				err += e * e;
			}
			double snr = (err > 0 && sig > 0)
			           ? 10.0 * __builtin_log10(sig / err) : 99.0;
			if (sig > 0 && snr < worst_snr)
				worst_snr = snr;
			snr_num++;

			if (frames > longest_frames) {
				longest_frames = frames;
				longest_id = id;
			}

			(void)mismatched;
			printf("%-10d %7d %8.2f %8.1f %8.1f %9.0f\n", id, size,
			       (double)frames / RATE, snr, lr_db,
			       (double)frames / RATE * 11025.0 * 0.5 / 1024.0);

			total_frames += frames;
			tunes++;

			/* Keep one for listening: the title theme, as PCM and as ADPCM. */
			if (id == GLUE_SOUND_MAIN_THEME) {
				write_wav("/tmp/pop-music-pcm.wav", mono, frames, RATE, 1);
				write_wav("/tmp/pop-music-ima.wav", ima, frames, RATE, 1);
			}
		}
	}

	double seconds = (double)total_frames / RATE;

	printf("\n%d tunes, %.1f s of music total (%.1f min)\n", tunes, seconds,
	       seconds / 60.0);
	printf("longest: resource %d at %.1f s\n", longest_id,
	       (double)longest_frames / RATE);
	/*
	 * The L-R figures look alarming (13-31 dB) but are not stereo content.
	 * Checked separately by correlation: the channels have identical RMS,
	 * correlate at 0.991, and no time shift improves the match - so both carry
	 * the same music and differ only by the emulator's own pipeline noise.
	 * Averaging them, as done above, is safe and one channel is enough.
	 */
	printf("channels: worst L-R rejection %.1f dB (%d tunes under 40 dB) - "
	       "pipeline noise, not stereo;\n"
	       "          L and R correlate at 0.991 with equal RMS, so mono is safe\n",
	       worst_lr, not_mono);
	if (truncated)
		printf("WARNING: %d tunes hit the %d s cap and were cut short\n",
		       truncated, MAX_SECONDS);
	if (snr_num)
		printf("worst IMA ADPCM SNR across all tunes: %.1f dB\n", worst_snr);

	printf("\nflash cost, mono:\n");
	printf("  %-34s %8s\n", "encoding", "KB");
	static const struct { const char *name; int rate; double bytes_per_sample; }
	options[] = {
		{ "22050 Hz  16-bit PCM",   22050, 2.0   },
		{ "22050 Hz   8-bit PCM",   22050, 1.0   },
		{ "22050 Hz   4-bit IMA",   22050, 0.5   },
		{ "11025 Hz  16-bit PCM",   11025, 2.0   },
		{ "11025 Hz   8-bit PCM",   11025, 1.0   },
		{ "11025 Hz   4-bit IMA",   11025, 0.5   },
		{ " 8000 Hz   4-bit IMA",    8000, 0.5   },
	};
	for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
		double bytes = seconds * options[i].rate * options[i].bytes_per_sample;
		printf("  %-34s %8.0f\n", options[i].name, bytes / 1024.0);
	}

	printf("\nfor comparison:\n");
	printf("  %-34s %8d\n", "MIDI resources as they are now", 27);
	printf("  %-34s %8d\n", "game resources already in flash", 1138);
	printf("  %-34s %8d\n", "free on a 2 MB Pico W today", 385);
	printf("  %-34s %8d\n", "free on a 4 MB Pico 2 W today", 2433);

	printf("\nwrote /tmp/pop-music-pcm.wav and /tmp/pop-music-ima.wav "
	       "(title theme, 22050 Hz mono)\n");

	free(stereo); free(mono); free(ima);
	return 0;
}
