/*
 * The demo's music.
 *
 * Original, and deliberately so. This used to play Prince of Persia's Adlib
 * tunes through SDLPoP's midi.c, which made the demo useless as a demo: the
 * music is someone else's, and the tunes live in the game's DAT files, so the
 * example program could not be built - let alone shipped - without a copy of the
 * game. picosdl is meant to be reusable, and an example that needs a particular
 * game's data to make a sound is not an example of picosdl.
 *
 * So: a sequencer small enough to read in one sitting, over three integer voices.
 * What it is here to exercise is unchanged - the mixer runs on core 1, the block
 * budget is real, and a tune playing continuously is what makes the cost visible
 * on the status line - but now none of that depends on anything but this file.
 *
 * No libm. The project forbids linking it (PLAN.md section 0), and there is no
 * need: pitch comes from a twelve-entry table of the lowest octave, shifted up by
 * whole octaves, and the waveform is arithmetic on a phase accumulator.
 */
#include "tune.h"

#include <string.h>

/* C0..B0 in millihertz. Every other note is one of these shifted left. */
static const unsigned s_semitone_mhz[12] = {
	16352, 17324, 18354, 19445, 20602, 21827,
	23125, 24500, 25957, 27500, 29135, 30868,
};

/* MIDI note numbers, so 60 is middle C. 0 means "rest". */
#define N_REST 0
#define A2 45
#define C3 48
#define D3 50
#define E3 52
#define G3 55
#define A3 57
#define C4 60
#define D4 62
#define E4 64
#define G4 67
#define A4 69
#define C5 72
#define D5 74
#define E5 76

/*
 * Four bars of eight, in A minor pentatonic: a walking bass under an arpeggio
 * with a plain melody over the top. Nothing clever - it exists to be audibly
 * present and obviously nobody else's.
 */
#define STEPS 32

static const unsigned char s_bass[STEPS] = {
	A2, 0, 0, 0, E3, 0, 0, 0,
	C3, 0, 0, 0, G3, 0, 0, 0,
	D3, 0, 0, 0, A2, 0, 0, 0,
	E3, 0, 0, 0, G3, 0, E3, 0,
};

static const unsigned char s_arp[STEPS] = {
	A3, C4, E4, C4, A3, C4, E4, G4,
	C4, E4, G4, E4, C4, E4, G4, A4,
	D4, A3, D4, E4, A3, C4, E4, C4,
	E4, G4, A4, G4, E4, D4, C4, A3,
};

static const unsigned char s_lead[STEPS] = {
	 0,  0, A4,  0,  0,  0, G4,  0,
	 0,  0, E4,  0,  0,  0, D4,  0,
	 0,  0, C5,  0,  0,  0, D5,  0,
	 0,  0, E5,  0, D5,  0, C5,  0,
};

/* One voice: a phase accumulator, a linear decay, and nothing else. */
struct voice {
	unsigned step;      /* phase increment per frame */
	unsigned phase;
	int      level;      /* current amplitude */
	int      decay;      /* subtracted per frame */
	int      triangle;   /* triangle if set, square otherwise */
};

static struct voice s_voice[TUNE_VOICES];
static int          s_rate = 22050;
static int          s_frames_per_step;
static int          s_frames_left;
static int          s_cursor;
static int          s_playing;

static unsigned note_step(int midi_note)
{
	/* MIDI 0 is C-1, so note 12 is C0 - the first entry of the table. */
	int octave = (midi_note / 12) - 1;
	int index  = midi_note % 12;
	if (octave < 0)
		return 0;

	unsigned long long mhz = (unsigned long long)s_semitone_mhz[index] << octave;
	return (unsigned)(((mhz << 32) / 1000u) / (unsigned)s_rate);
}

static void note_on(int v, int midi_note, int amplitude, int decay, int triangle)
{
	if (midi_note == N_REST)
		return;
	s_voice[v].step     = note_step(midi_note);
	s_voice[v].phase    = 0;
	s_voice[v].level    = amplitude;
	s_voice[v].decay    = decay;
	s_voice[v].triangle = triangle;
}

void tune_init(int sample_rate)
{
	s_rate = sample_rate > 0 ? sample_rate : 22050;
	/* Eighth notes at 120 BPM: a quarter note is half a second. */
	s_frames_per_step = s_rate / 4;
	tune_stop();
}

void tune_start(void)
{
	memset(s_voice, 0, sizeof(s_voice));
	s_cursor      = 0;
	s_frames_left = 0;
	s_playing     = 1;
}

void tune_stop(void)
{
	memset(s_voice, 0, sizeof(s_voice));
	s_playing = 0;
}

int tune_playing(void) { return s_playing; }

const char *tune_name(void) { return "demo tune (original)"; }

static void advance_step(void)
{
	/* Decays are chosen so each voice has faded before it is retriggered: the
	 * bass rings across two steps, the arpeggio is short and plucked. */
	note_on(0, s_bass[s_cursor], 5000, 5000 / (s_frames_per_step * 2 / 1), 1);
	note_on(1, s_arp [s_cursor], 3200, 3200 / (s_frames_per_step - 1), 1);
	note_on(2, s_lead[s_cursor], 3800, 3800 / (s_frames_per_step * 2 - 1), 0);

	s_cursor      = (s_cursor + 1) % STEPS;
	s_frames_left = s_frames_per_step;
}

/*
 * Mix into `stereo`, which the caller has already zeroed or filled. Adds rather
 * than overwrites so the demo's own plucked notes can sit on top; the caller
 * clamps once at the end.
 */
void tune_render(Sint32 *mix_mono, int frames)
{
	if (!s_playing)
		return;

	for (int i = 0; i < frames; ++i) {
		if (s_frames_left == 0)
			advance_step();
		--s_frames_left;

		Sint32 sum = 0;
		for (int v = 0; v < TUNE_VOICES; ++v) {
			struct voice *vo = &s_voice[v];
			if (vo->level <= 0)
				continue;
			vo->phase += vo->step;
			if (vo->triangle) {
				/* Triangle from the top 16 bits: up for half a period, down
				 * for the other half, scaled by the envelope. */
				Sint32 t = (Sint32)(vo->phase >> 16);            /* 0..65535 */
				Sint32 w = t < 32768 ? t - 16384 : 49151 - t;     /* +-16384 */
				sum += (w * vo->level) >> 14;
			} else {
				sum += (vo->phase & 0x80000000u) ? vo->level : -vo->level;
			}
			vo->level -= vo->decay;
			if (vo->level < 0)
				vo->level = 0;
		}
		mix_mono[i] += sum;
	}
}
