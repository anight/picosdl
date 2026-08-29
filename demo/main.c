/*
 * picosdl demo: display, keyboard, joystick and sound, all through the SDL API.
 *
 * There is no game code here. The point is to prove the layer end to end - that
 * a program written against SDL2 calls compiles and runs on a Pico, and that
 * every subsystem works while the others are running. The awkward part of this
 * port was never any one peripheral; it is that the panel, the radio and the
 * DAC have to coexist on one chip.
 *
 * What it shows:
 *
 *   - 320x200 8bpp indexed framebuffer, letterboxed on a 320x240 panel
 *   - palette animation: the background bars are drawn with fixed indices and
 *     animated purely by rewriting the CLUT, which is what makes fades free
 *   - a flash-resident sprite blitted normally, mirrored (PSDL_BlitMirrored,
 *     which replaces SDLPoP's allocate-a-surface-per-flip hflip) and XORed
 *   - text through the ordinary keyed blitter, via the LIFO arena
 *   - a Bluetooth keyboard feeding SDL key events, with no scancode translation
 *   - an analog stick through SDL's joystick API
 *   - a four-voice synth in an SDL audio callback, mixed on core 1
 *   - the game's own title music: SDLPoP's unmodified midi.c driving DBOPL,
 *     reading the Adlib data straight out of flash
 *
 * The music is not decoration - it is how the mixer's real cost gets measured.
 * The demo reports mixer load as a percentage of each audio block's budget,
 * because the host figures behind the emulator choice cross an ISA boundary and
 * only the board settles them.
 *
 * Controls:
 *   stick        move the sprite (it faces the way it is going)
 *   stick button pluck a low note
 *   any key      pluck a note; the pitch follows the scancode
 *   1..4         play one of the game's Adlib tunes (1 is the title theme,
 *                which also starts by itself at boot)
 *   0            stop the music
 *   - / =        master volume down / up (starts at 25%)
 *   T            steady test tone, replacing everything else - if this is clean
 *                the output path is fine and the fault is upstream of it
 *   space        draw the sprite with an XOR blit
 *   F            fade the palette out and back, by CLUT writes alone
 *   M            print the memory report to the console
 *   Escape       stop
 */
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "SDL2/SDL.h"
#include "psdl_pico.h"

#include "font5x7.h"
#include "game_glue.h"

/* From SDLPoP's midi.c, used unmodified. */
void midi_callback(void *userdata, Uint8 *stream, int len);
void stop_midi(void);
extern short midi_playing;

/* Non-zero if midi.c ever had to truncate a block; see the note there. */
extern unsigned midi_overlong_blocks;

/*
 * A steady triangle wave, generated right where the mixed samples are handed
 * over. It is the bisection tool for audio faults: if the tone is clean then
 * the I2S clocking, the DMA chain, the block sizing and the DAC are all fine
 * and the problem is in whatever produces the samples.
 */
static volatile int s_test_tone_hz;

/*
 * Heap watermark.
 *
 * parse_midi() is the only thing in this firmware that allocates, and how much
 * it wants is the question that decides whether the tunes have to be pre-parsed
 * at build time. sbrk(0) is the top of the heap, so the difference across a
 * parse is what that tune cost.
 */
extern char *sbrk(int incr);

static unsigned heap_used(void)
{
	extern char __StackLimit, end;   /* from the linker script */
	(void)&__StackLimit;
	return (unsigned)(sbrk(0) - &end);
}

/* ------------------------------------------------------- palette layout */

#define PAL_BLACK      0
#define PAL_WHITE      1
#define PAL_GREY       2
#define PAL_RED        3
#define PAL_GREEN      4
#define PAL_YELLOW     5
#define PAL_CYAN       6

#define PAL_BG_BASE    16   /* 32 entries, cycled every frame  */
#define PAL_BG_COUNT   32
#define PAL_SPR_BASE   64   /* 8 entries, the sprite's colours */

static SDL_Color s_base_palette[256];

static void palette_build(void)
{
	memset(s_base_palette, 0, sizeof(s_base_palette));

	static const struct { int i; Uint8 r, g, b; } ui[] = {
		{ PAL_WHITE,  0xF0, 0xF0, 0xF0 },
		{ PAL_GREY,   0x70, 0x70, 0x78 },
		{ PAL_RED,    0xE0, 0x40, 0x30 },
		{ PAL_GREEN,  0x40, 0xD0, 0x50 },
		{ PAL_YELLOW, 0xF0, 0xC0, 0x30 },
		{ PAL_CYAN,   0x40, 0xC0, 0xE0 },
	};
	for (unsigned i = 0; i < sizeof(ui) / sizeof(ui[0]); ++i) {
		s_base_palette[ui[i].i].r = ui[i].r;
		s_base_palette[ui[i].i].g = ui[i].g;
		s_base_palette[ui[i].i].b = ui[i].b;
	}

	/* A dark blue-to-purple ramp for the background bands. */
	for (int i = 0; i < PAL_BG_COUNT; ++i) {
		int t = i < PAL_BG_COUNT / 2 ? i : PAL_BG_COUNT - 1 - i;  /* triangle */
		s_base_palette[PAL_BG_BASE + i].r = (Uint8)(t * 5);
		s_base_palette[PAL_BG_BASE + i].g = (Uint8)(t * 2);
		s_base_palette[PAL_BG_BASE + i].b = (Uint8)(24 + t * 8);
	}

	/* Sprite colours, indexed by the digits in the art below. */
	static const Uint8 spr[8][3] = {
		{ 0x00, 0x00, 0x00 },   /* 0 unused (transparent) */
		{ 0xC0, 0x90, 0x60 },   /* 1 skin shadow  */
		{ 0xF0, 0xC0, 0x90 },   /* 2 skin         */
		{ 0x20, 0x18, 0x10 },   /* 3 eyes         */
		{ 0x80, 0x20, 0x20 },   /* 4 tunic shadow */
		{ 0xD0, 0x40, 0x40 },   /* 5 tunic        */
		{ 0x30, 0x50, 0xA0 },   /* 6 trousers     */
		{ 0x60, 0x40, 0x20 },   /* 7 boots        */
	};
	for (int i = 0; i < 8; ++i) {
		s_base_palette[PAL_SPR_BASE + i].r = spr[i][0];
		s_base_palette[PAL_SPR_BASE + i].g = spr[i][1];
		s_base_palette[PAL_SPR_BASE + i].b = spr[i][2];
	}
}

/*
 * Push the palette with the background band ramp rotated by `phase` and
 * everything scaled by `brightness` (0..256). Both effects are pure CLUT
 * writes - the framebuffer is untouched, which is the whole point.
 */
static void palette_apply(int phase, int brightness)
{
	SDL_Color out[256];

	for (int i = 0; i < 256; ++i) {
		SDL_Color c = s_base_palette[i];
		if (i >= PAL_BG_BASE && i < PAL_BG_BASE + PAL_BG_COUNT) {
			int src = PAL_BG_BASE + ((i - PAL_BG_BASE + phase) % PAL_BG_COUNT);
			c = s_base_palette[src];
		}
		out[i].r = (Uint8)((c.r * brightness) >> 8);
		out[i].g = (Uint8)((c.g * brightness) >> 8);
		out[i].b = (Uint8)((c.b * brightness) >> 8);
		out[i].a = SDL_ALPHA_OPAQUE;
	}

	SDL_SetPaletteColors(PSDL_GlobalPalette(), out, 0, 256);
}

/* ---------------------------------------------------------- the sprite */

/*
 * Kept as art and expanded once into a static buffer, which is then wrapped by
 * SDL_CreateRGBSurfaceFrom. That takes the PSDL_SURF_FLASH path: picosdl treats
 * the pixels as owned by someone else, never copies them and never frees them -
 * exactly how the game's real sprites will be handed over once they are
 * pre-decoded into flash at build time.
 */
#define SPRITE_W 16
#define SPRITE_H 16

static const char *const sprite_art[SPRITE_H] = {
	"      1111      ",
	"     122221     ",
	"     123321     ",
	"     122221     ",
	"      1111      ",
	"    45555554    ",
	"   4555555554   ",
	"  455555555554  ",
	"  4 55555555 4  ",
	"    55555555    ",
	"     55  55     ",
	"     66  66     ",
	"     66  66     ",
	"    666  666    ",
	"   777    777   ",
	"  7777    7777  ",
};

static Uint8 sprite_pixels[SPRITE_W * SPRITE_H];

static SDL_Surface *sprite_create(void)
{
	for (int y = 0; y < SPRITE_H; ++y) {
		for (int x = 0; x < SPRITE_W; ++x) {
			char c = sprite_art[y][x];
			sprite_pixels[y * SPRITE_W + x] =
				(c >= '1' && c <= '7') ? (Uint8)(PAL_SPR_BASE + (c - '0')) : 0;
		}
	}

	SDL_Surface *s = SDL_CreateRGBSurfaceFrom(sprite_pixels, SPRITE_W, SPRITE_H,
	                                          8, SPRITE_W, 0, 0, 0, 0);
	if (s != NULL)
		SDL_SetColorKey(s, SDL_TRUE, 0);
	return s;
}

/* ------------------------------------------------------------- the synth */

/*
 * Four voices of decaying square wave. Deliberately trivial: this runs in the
 * SDL audio callback, which picosdl invokes on core 1, so what it proves is the
 * plumbing - callback frequency, buffer sizing, cross-core locking - rather than
 * anything about synthesis.
 */
#define VOICES 4

typedef struct {
	Uint32 phase;
	Uint32 step;      /* phase increment per frame, 16.16 */
	Sint32 level;     /* current amplitude       */
	Sint32 decay;     /* subtracted per frame    */
} voice_t;

static volatile voice_t s_voices[VOICES];
static int s_next_voice;

static void synth_pluck(int freq_hz, int amplitude, int decay_ms)
{
	if (freq_hz <= 0)
		return;

	/* Touching voice state the mixer reads: hold the audio lock, which is what
	 * SDL_LockAudio is for and why it has to work across cores here. */
	SDL_LockAudio();
	volatile voice_t *v = &s_voices[s_next_voice];
	s_next_voice = (s_next_voice + 1) % VOICES;

	v->phase = 0;
	v->step  = (Uint32)(((Uint64)freq_hz << 16) / PSDL_PICO_AUDIO_RATE);
	v->level = amplitude;
	v->decay = amplitude / ((decay_ms * PSDL_PICO_AUDIO_RATE) / 1000 + 1);
	if (v->decay < 1)
		v->decay = 1;
	SDL_UnlockAudio();
}

/*
 * How long the callback took, as a fraction of the time it had. Anything near
 * 100% means the mixer cannot keep up and audio will break; this is the number
 * the OPL3 question turns on.
 */
static volatile Uint32 s_mix_us_total, s_mix_blocks, s_mix_us_peak;

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;

	Uint64 started = SDL_GetPerformanceCounter();

	Sint16 *out    = (Sint16 *)stream;
	int     frames = len / (2 * (int)sizeof(Sint16));

	if (s_test_tone_hz > 0) {
		static Uint32 phase;
		Uint32 step = (Uint32)(((Uint64)s_test_tone_hz << 32) / PSDL_PICO_AUDIO_RATE);
		for (int i = 0; i < frames; ++i) {
			phase += step;
			/* Triangle from the top two bits of the accumulator: rises for
			 * half a period, falls for the other half. */
			Sint32 t = (Sint32)(phase >> 16);           /* 0..65535 */
			Sint32 v = t < 32768 ? t - 16384 : 49151 - t;  /* -16384..16383 */
			out[i * 2 + 0] = (Sint16)v;
			out[i * 2 + 1] = (Sint16)v;
		}
		s_mix_blocks++;
		return;
	}

	/* midi.c *adds* into the buffer and expects it zeroed first, which is also
	 * what the game's own audio_callback does. Do the music first, then mix the
	 * demo's plucked notes on top. */
	memset(stream, 0, (size_t)len);
	if (midi_playing)
		midi_callback(NULL, stream, len);

	for (int i = 0; i < frames; ++i) {
		Sint32 mix = 0;
		for (int v = 0; v < VOICES; ++v) {
			if (s_voices[v].level <= 0)
				continue;
			s_voices[v].phase += s_voices[v].step;
			/* Square wave: sign from the top bit of the phase accumulator. */
			mix += (s_voices[v].phase & 0x8000u) ? s_voices[v].level
			                                     : -s_voices[v].level;
			s_voices[v].level -= s_voices[v].decay;
			if (s_voices[v].level < 0)
				s_voices[v].level = 0;
		}

		/* Sum with whatever the music left here, then clamp once. */
		mix += out[i * 2 + 0];
		if (mix >  32767) mix =  32767;
		if (mix < -32768) mix = -32768;

		out[i * 2 + 0] = (Sint16)mix;
		out[i * 2 + 1] = (Sint16)mix;
	}

	Uint32 elapsed = (Uint32)(SDL_GetPerformanceCounter() - started);
	s_mix_us_total += elapsed;
	s_mix_blocks++;
	if (elapsed > s_mix_us_peak)
		s_mix_us_peak = elapsed;
}

/* How long one block of audio lasts - the time the callback has to produce it.
 * Taken from the spec SDL_OpenAudio actually gave us, not from what we asked
 * for: picosdl fixes the block size and reports it back. */
static unsigned s_block_budget_us = 1;

/* Percent of the available time the mixer is using, averaged since the last
 * call, and the worst single block since then. */
static void mixer_load(unsigned *avg_pct, unsigned *peak_pct)
{
	Uint32 blocks = s_mix_blocks;
	Uint32 total  = s_mix_us_total;
	Uint32 peak   = s_mix_us_peak;
	s_mix_blocks   = 0;
	s_mix_us_total = 0;
	s_mix_us_peak  = 0;

	*avg_pct  = blocks ? (unsigned)(total / blocks) * 100u / s_block_budget_us : 0;
	*peak_pct = peak * 100u / s_block_budget_us;
}

/* A note per scancode, so a keyboard sweeps a scale rather than one pitch. */
static int pitch_for_scancode(SDL_Scancode sc)
{
	static const int scale[] = { 262, 294, 330, 349, 392, 440, 494 };
	int degree = (int)sc % 7;
	int octave = ((int)sc / 7) % 3;
	return scale[degree] << octave;
}

/* ------------------------------------------------------------ the demo */

int main(void)
{
	/* Before stdio_init_all(), always. set_sys_clock_khz() re-parents clk_peri
	 * off clk_sys, and stdio derives the UART divisor from clk_peri when it
	 * starts - the other order leaves the console at the wrong baud rate. */
	set_sys_clock_khz(PSDL_PICO_SYS_CLOCK_KHZ, true);
	stdio_init_all();
	sleep_ms(1500);

	printf("\n=== picosdl demo ===\n");
	printf("sys clock %u Hz\n", (unsigned)clock_get_hz(clk_sys));

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
		printf("SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window *window = SDL_CreateWindow("picosdl", SDL_WINDOWPOS_UNDEFINED,
	                                      SDL_WINDOWPOS_UNDEFINED, 320, 200,
	                                      SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_Surface *screen = SDL_GetWindowSurface(window);
	if (screen == NULL) {
		printf("no window surface: %s\n", SDL_GetError());
		return 1;
	}

	palette_build();
	palette_apply(0, 256);

	SDL_Surface *sprite = sprite_create();
	if (sprite == NULL)
		printf("sprite: %s\n", SDL_GetError());

	SDL_Joystick *joystick = SDL_JoystickOpen(0);

	static SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq     = PSDL_PICO_AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 256;
	want.callback = audio_callback;
	if (SDL_OpenAudio(&want, &have) != 0) {
		printf("audio: %s\n", SDL_GetError());
	} else {
		s_block_budget_us = 1000000u * have.samples / (unsigned)have.freq;
		printf("audio: %d Hz, %u-frame blocks, %u us per block, volume %d%%\n",
		       have.freq, (unsigned)have.samples, s_block_budget_us,
		       PSDL_GetMasterVolume() * 100 / PSDL_VOLUME_UNITY);
		/* midi.c reads its mixing rate from this. It has to be the spec we
		 * actually got, or the music plays at the wrong speed. */
		glue_set_audiospec(&have);
		SDL_PauseAudio(0);
	}

	/* The game's Adlib tunes, straight out of flash. */
	static const struct { int id; const char *name; } tunes[] = {
		{ GLUE_SOUND_MAIN_THEME,      "main theme"        },
		{ GLUE_SOUND_STORY_1_ABSENCE, "story 1: absence"  },
		{ GLUE_SOUND_STORY_3_JAFFAR,  "story 3: Jaffar"   },
		{ GLUE_SOUND_ENDING_MUSIC,    "winning theme"     },
	};
	const char *now_playing = "(nothing)";

	/*
	 * Start the title theme straight away, the way the game's own start screen
	 * does. It also means the audio path demonstrates itself on a bare board:
	 * no keyboard has to pair first, which matters because Bluetooth is the one
	 * part of this that can quietly fail to come up.
	 */
	{
		int size = 0;
		const void *res = glue_find_resource("MIDISND2.DAT",
		                                     GLUE_SOUND_MAIN_THEME, &size);
		if (res == NULL) {
			printf("music: title theme (resource %d) not found\n",
			       GLUE_SOUND_MAIN_THEME);
		} else {
			glue_play_music(res);
			now_playing = "main theme";
			printf("music: playing the title theme at boot "
			       "(resource %d, %d bytes)\n", GLUE_SOUND_MAIN_THEME, size);
		}
	}

	/* State. */
	int    sprite_x = (screen->w - SPRITE_W) / 2;
	int    sprite_y = (screen->h - SPRITE_H) / 2;
	int    facing_left = 0;
	int    phase = 0;
	int    fade = 256, fade_dir = 0;
	int    running = 1;
	char   last_key[24] = "(none)";

	Uint32   frames = 0, fps = 0, fps_mark = SDL_GetTicks();
	unsigned mix_avg = 0, mix_peak = 0;

	while (running) {
		/* ---- input ---- */
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			switch (ev.type) {
			case SDL_QUIT:
				running = 0;
				break;

			case SDL_KEYDOWN: {
				SDL_Scancode sc = ev.key.keysym.scancode;
				snprintf(last_key, sizeof(last_key), "%s (0x%02X)",
				         SDL_GetScancodeName(sc), (unsigned)sc);

				if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_4) {
					int n = sc - SDL_SCANCODE_1;
					int size = 0;
					const void *res = glue_find_resource("MIDISND2.DAT",
					                                     tunes[n].id, &size);
					if (res == NULL) {
						printf("music: resource %d not found\n", tunes[n].id);
					} else {
						unsigned before = heap_used();
						glue_play_music(res);
						printf("music: %s (resource %d, %d bytes) - "
						       "parsed into %u bytes of heap, %u total\n",
						       tunes[n].name, tunes[n].id, size,
						       heap_used() - before, heap_used());
						now_playing = tunes[n].name;
					}
					break;
				}
				if (sc == SDL_SCANCODE_0) {
					stop_midi();
					now_playing = "(nothing)";
					break;
				}
				if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_EQUALS) {
					int v = PSDL_GetMasterVolume() +
					        (sc == SDL_SCANCODE_EQUALS ? 16 : -16);
					PSDL_SetMasterVolume(v);
					printf("volume: %d%%\n",
					       PSDL_GetMasterVolume() * 100 / PSDL_VOLUME_UNITY);
					break;
				}
				if (sc == SDL_SCANCODE_T) {
					s_test_tone_hz = s_test_tone_hz ? 0 : 440;
					printf("test tone: %s\n", s_test_tone_hz ? "440 Hz" : "off");
					break;
				}

				synth_pluck(pitch_for_scancode(sc), 7000, 350);

				if (sc == SDL_SCANCODE_ESCAPE)
					running = 0;
				else if (sc == SDL_SCANCODE_F && fade_dir == 0)
					fade_dir = -1;
				else if (sc == SDL_SCANCODE_M)
					PSDL_ReportMemory();
				break;
			}

			case SDL_JOYBUTTONDOWN:
				synth_pluck(110, 9000, 700);
				break;

			default:
				break;
			}
		}

		/* ---- movement ---- */
		Sint16 ax = SDL_JoystickGetAxis(joystick, 0);
		Sint16 ay = SDL_JoystickGetAxis(joystick, 1);
		int dx = ax / 6000;
		int dy = ay / 6000;
		if (dx != 0)
			facing_left = dx < 0;
		sprite_x += dx;
		sprite_y += dy;
		if (sprite_x < 0) sprite_x = 0;
		if (sprite_y < 0) sprite_y = 0;
		if (sprite_x > screen->w - SPRITE_W) sprite_x = screen->w - SPRITE_W;
		if (sprite_y > screen->h - SPRITE_H) sprite_y = screen->h - SPRITE_H;

		/* ---- draw ---- */
		/* Background bands. The indices never change from frame to frame; the
		 * motion you see comes entirely from rotating the CLUT below. */
		for (int y = 0; y < screen->h; y += 4) {
			SDL_Rect band = { 0, y, screen->w, 4 };
			SDL_FillRect(screen, &band,
			             (Uint32)(PAL_BG_BASE + ((y / 4) % PAL_BG_COUNT)));
		}

		SDL_Rect frame_rect = { 4, 4, screen->w - 8, screen->h - 8 };
		SDL_Rect inner      = { 5, 5, screen->w - 10, screen->h - 10 };
		SDL_FillRect(screen, &frame_rect, PAL_GREY);
		SDL_FillRect(screen, &inner, PAL_BLACK);

		const Uint8 *keys = SDL_GetKeyboardState(NULL);
		SDL_Rect at = { sprite_x, sprite_y, SPRITE_W, SPRITE_H };
		if (keys[SDL_SCANCODE_SPACE])
			PSDL_BlitXor(sprite, NULL, screen, &at);
		else if (facing_left)
			PSDL_BlitMirrored(sprite, NULL, screen, &at);
		else
			SDL_BlitSurface(sprite, NULL, screen, &at);

		char line[64];
		int  ty = 12;
		font5x7_draw(screen, 12, ty, PAL_YELLOW, "picosdl demo"); ty += 12;

		font5x7_draw(screen, 12, ty, PAL_CYAN, psdl_pico_input_status()); ty += 10;

		snprintf(line, sizeof(line), "key: %s", last_key);
		font5x7_draw(screen, 12, ty, PAL_WHITE, line); ty += 10;

		snprintf(line, sizeof(line), "stick: x=%6d y=%6d %s",
		         (int)ax, (int)ay, SDL_JoystickGetButton(joystick, 0) ? "[btn]" : "");
		font5x7_draw(screen, 12, ty, PAL_WHITE, line); ty += 10;

		snprintf(line, sizeof(line), "fps: %u   dropped events: %u",
		         (unsigned)fps, PSDL_DroppedEvents());
		font5x7_draw(screen, 12, ty, PAL_GREEN, line); ty += 10;

		snprintf(line, sizeof(line), "music: %s%s", now_playing,
		         s_test_tone_hz ? "  [TEST TONE]" : "");
		font5x7_draw(screen, 12, ty, PAL_YELLOW, line); ty += 10;

		snprintf(line, sizeof(line), "volume: %d%%   truncated blocks: %u",
		         PSDL_GetMasterVolume() * 100 / PSDL_VOLUME_UNITY,
		         midi_overlong_blocks);
		font5x7_draw(screen, 12, ty,
		             midi_overlong_blocks ? PAL_RED : PAL_CYAN, line); ty += 10;

		/* The number the OPL3 question turns on. Red once the mixer is using
		 * more than three quarters of its budget. */
		snprintf(line, sizeof(line), "mixer load: %u%% avg, %u%% peak",
		         mix_avg, mix_peak);
		font5x7_draw(screen, 12, ty, mix_peak > 75 ? PAL_RED : PAL_GREEN, line);

		font5x7_draw(screen, 12, screen->h - 22, PAL_GREY,
		             "stick moves - space xor - F fade");
		font5x7_draw(screen, 12, screen->h - 12, PAL_GREY,
		             "1-4 music  0 stop  -/= vol  T tone  M mem  Esc quit");

		SDL_UpdateWindowSurface(window);

		/* ---- palette effects ---- */
		phase = (phase + 1) % PAL_BG_COUNT;
		if (fade_dir != 0) {
			fade += fade_dir * 8;
			if (fade <= 0)   { fade = 0;   fade_dir =  1; }
			if (fade >= 256) { fade = 256; fade_dir =  0; }
		}
		palette_apply(phase, fade);

		/* ---- frame accounting ---- */
		++frames;
		Uint32 now = SDL_GetTicks();
		if (now - fps_mark >= 1000) {
			fps      = frames * 1000 / (now - fps_mark);
			frames   = 0;
			fps_mark = now;
			mixer_load(&mix_avg, &mix_peak);
			if (midi_playing)
				printf("fps %u, mixer %u%% avg / %u%% peak, music '%s'\n",
				       (unsigned)fps, mix_avg, mix_peak, now_playing);
		}
	}

	printf("picosdl demo: stopping\n");
	PSDL_ReportMemory();
	SDL_CloseAudio();
	SDL_Quit();
	return 0;
}
