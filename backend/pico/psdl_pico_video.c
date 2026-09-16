/*
 * Video backend: Dmitry Grinberg's PIO ST7789 driver.
 *
 * This is the piece of hardware that made the whole design fall out. The driver
 * takes an 8bpp palettised buffer and a 256-entry CLUT and expands indices to
 * RGB565 in the PIO/DMA chain - the CPU never touches a pixel. That is VGA mode
 * 13h in hardware, which is exactly the shape Prince of Persia wants, so
 * picosdl's "one indexed format" restriction is not a compromise here; it is
 * what the panel already does.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"

#include "dispPioSt7789.h"
#include "pinout.h"

#include "psdl_font5x7.h"
#include "psdl_internal.h"
#include "psdl_pico.h"

static struct dmaTransfer *s_xfer;
static int s_offset_y;
static int s_ready;

/* ------------------------------------------------------- status bands
 *
 * The panel is 320x240 and a 320x200 canvas is letterboxed into it, which leaves two
 * 20-pixel strips that nothing ever wrote to. They are a natural place for a status
 * line, and drawing them costs almost nothing if it is done rarely.
 *
 * Two things about the panel decide the design.
 *
 * Indices are expanded through the CLUT *at push time*, by the PIO. So once a band
 * is pushed its pixels are fixed RGB on the panel and a later palette change does
 * not touch them - unlike the canvas, which is re-pushed every frame and therefore
 * follows the CLUT. A band drawn before the client set up its palette would stay
 * whatever the CLUT held then, which at start-up is black on black. Both bands are
 * therefore re-pushed once a second, along with the figures, which keeps them
 * legible through fades and start-up without any notion of "the palette changed".
 *
 * And the push has to be ordered against the frame, or the two DMA transfers
 * interleave and both tear. dispDrawBuffer() waits for whatever is in flight before
 * it starts, so doing the bands inside present() - before handing over the canvas -
 * is what synchronises them: the canvas transfer is still the one left running when
 * present returns, so the overlap that buys the frame rate is preserved and only the
 * band push blocks, once a second.
 */
#define BAND_H 20

/* One band's worth of pixels, reused for both. Static rather than from the arena:
 * 6.4 KB pinned at the bottom of a 28 KB LIFO arena for the life of the process is
 * exactly the mistake that exhausted it once already. */
static Uint8        s_band_pixels[PSDL_PICO_PANEL_W * BAND_H];
static SDL_Surface *s_band;

static int   s_bands_on;

/*
 * The bands' own colours, and the two CLUT slots they borrow to get them onto the
 * panel.
 *
 * The band buffer is 8bpp like everything else, so its pixels have to be palette
 * indices - but the colours must not be the *client's* palette entries. The CLUT
 * is expanded at push time, so a band pushed while the game has rewritten an
 * entry comes out in the game's colour: Prince of Persia implements its damage
 * flash by writing index 0 red, and a band repaint landing inside one painted the
 * letterbox red until the next repaint a second later. Fades did the same thing
 * more quietly.
 *
 * So the two indices are overridden to the band's colours immediately before the
 * push and restored from the palette afterwards. Nothing else can be reading the
 * CLUT at that point: present() has already waited for the previous transfer, and
 * each band push waits for its own. Which two indices they are does not matter,
 * since they are put back.
 */
#define BAND_IDX_BG 0
#define BAND_IDX_FG 1

static SDL_Color s_band_fg = { 255, 255, 255, SDL_ALPHA_OPAQUE };
static SDL_Color s_band_bg = {   0,   0,   0, SDL_ALPHA_OPAQUE };
static char  s_footer[48];

/* Figures for the header. Frames are counted here; the two idle counters are
 * elsewhere, each where the time is actually spent. */
static unsigned s_frames;
static Uint32   s_stats_ms;
static char     s_header[48];

#if PSDL_HAVE_AUDIO
extern volatile uint32_t psdl_pico_core1_busy_us;   /* psdl_pico_audio.c */
#endif
extern volatile uint32_t psdl_pico_core0_idle_us;   /* psdl_pico_time.c  */

void PSDL_StatusBands(SDL_bool on, SDL_Color fg, SDL_Color bg)
{
	s_bands_on = on ? 1 : 0;
	s_band_fg  = fg;
	s_band_bg  = bg;
	s_band_fg.a = SDL_ALPHA_OPAQUE;
	s_band_bg.a = SDL_ALPHA_OPAQUE;
}

void PSDL_SetFooterText(const char *text)
{
	if (text == NULL)
		text = "";
	snprintf(s_footer, sizeof(s_footer), "%s", text);
}

static void clut_write(int index, SDL_Color c)
{
	struct ClutEntry e = { .r = c.r, .g = c.g, .b = c.b };
	dispSetClut(index, 1, &e);
}

/* Lend the band its two colours. Nothing is reading the CLUT here - see the note
 * on BAND_IDX_BG. */
static void band_clut_override(void)
{
	clut_write(BAND_IDX_BG, s_band_bg);
	clut_write(BAND_IDX_FG, s_band_fg);
}

/* And give them back, from the palette rather than from the hardware: picosdl's
 * SDL_Palette is the authority on what the client last set. */
static void band_clut_restore(void)
{
	const SDL_Color *pal = PSDL_GlobalPalette()->colors;
	clut_write(BAND_IDX_BG, pal[BAND_IDX_BG]);
	clut_write(BAND_IDX_FG, pal[BAND_IDX_FG]);
}

/* Render one band into the shared buffer and push it, waiting for it to land. */
static void band_push(int y, const char *text)
{
	if (s_band == NULL)
		return;

	memset(s_band_pixels, BAND_IDX_BG, sizeof(s_band_pixels));

	int w = PSDL_Font5x7Width(text);
	int x = (PSDL_PICO_PANEL_W - w) / 2;
	if (x < 0)
		x = 0;
	PSDL_Font5x7Draw(s_band, x, (BAND_H - PSDL_FONT5X7_HEIGHT) / 2, BAND_IDX_FG, text);

	struct Rect r = { 0, (int16_t)y, PSDL_PICO_PANEL_W, BAND_H };
	struct dmaTransfer *t = dispDrawBuffer(s_band_pixels,
	                                       PSDL_PICO_PANEL_W * BAND_H, &r,
	                                       PSDL_PICO_PANEL_W);
	/* Wait, because the buffer is shared between the two bands and reused next
	 * second. One band is 6400 pixels, about 1.6 ms. */
	if (t != NULL)
		dispDmaTransferWaitFinish(t);
	s_xfer = NULL;          /* that wait consumed whatever was outstanding */
}

/*
 * Once a second: work out the figures, then repaint both bands.
 *
 * core 1 is the mixer and nothing else, so its busy time over the interval is its
 * load.
 *
 * core 0 is the game, and it idles in two places, both of which picosdl can see: in
 * SDL_Delay(), which is a real WFE sleep and is how a client waits out a frame -
 * SDLPoP's do_simple_wait() loops on SDL_Delay(1) - and blocked waiting for the
 * panel transfer here. Its load is the complement of the two together. Counting only
 * the panel wait, as a first version of this did, overstates the load by however long
 * the client sleeps, which for a frame-paced game is most of the difference.
 */
static void bands_tick(void)
{
	Uint32 now = psdl_backend_ticks_ms();
	if (s_stats_ms == 0)
		s_stats_ms = now;
	if ((Uint32)(now - s_stats_ms) < 1000)
		return;

	unsigned elapsed_ms = (unsigned)(now - s_stats_ms);
	uint64_t elapsed_us = (uint64_t)elapsed_ms * 1000u;

	unsigned fps  = elapsed_ms ? s_frames * 1000u / elapsed_ms : 0;
	unsigned idle = elapsed_us ? (unsigned)((uint64_t)psdl_pico_core0_idle_us * 100u / elapsed_us) : 0;
	if (idle > 100) idle = 100;
	unsigned c0   = 100u - idle;

	s_stats_ms   = now;
	s_frames     = 0;
	psdl_pico_core0_idle_us = 0;

	/* Core 1 is the mixer and nothing else, so with the audio off it is never
	 * launched and there is no second figure to report. Leave it out rather
	 * than print a permanent zero that looks like a measurement. */
#if PSDL_HAVE_AUDIO
	unsigned c1 = elapsed_us ? (unsigned)((uint64_t)psdl_pico_core1_busy_us * 100u / elapsed_us) : 0;
	if (c1 > 100) c1 = 100;
	psdl_pico_core1_busy_us = 0;

	snprintf(s_header, sizeof(s_header), "%u FPS   CORE0 %u%%   CORE1 %u%%", fps, c0, c1);
#else
	snprintf(s_header, sizeof(s_header), "%u FPS   CORE0 %u%%", fps, c0);
#endif

	band_clut_override();
	band_push(0, s_header);
	band_push(PSDL_PICO_PANEL_H - BAND_H, s_footer);
	band_clut_restore();
}

/*
 * The display driver hardcodes DMA channels 0..3 and does not claim them.
 * Nothing else in this firmware would know that: the CYW43 driver and the I2S
 * driver both ask the SDK for "any free channel", and would happily be given
 * one out from under the panel. Claim them here, first, so the allocator knows.
 */
static void claim_display_dma_channels(void)
{
	for (int ch = 0; ch < 4; ++ch) {
		if (dma_channel_is_claimed(ch))
			psdl_panic("picosdl: DMA channel %d already claimed - "
			           "the video backend must be initialised first", ch);
		dma_channel_claim(ch);
	}
}

/* The panel's control pins start as plain GPIOs; the driver moves the SPI ones
 * to PIO itself once its programs are loaded. */
static void lcd_pins_init(void)
{
	static const struct { uint8_t pin; bool is_output; } pins[] = {
		{ PIN_LCD_DnC,   true  },
		{ PIN_LCD_CS,    true  },
		{ PIN_SPI_CLK,   true  },
		{ PIN_SPI_MOSI,  true  },
		{ PIN_SPI_MISO,  false },
		{ PIN_LCD_RESET, true  },
	};

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
		gpio_init(pins[i].pin);
		gpio_set_dir(pins[i].pin, pins[i].is_output);
		if (pins[i].is_output)
			gpio_put(pins[i].pin, 1);
	}

	/* Reset pulse: active low. */
	gpio_put(PIN_LCD_RESET, 0);
	sleep_ms(10);
	gpio_put(PIN_LCD_RESET, 1);
	sleep_ms(120);
}

void psdl_backend_video_init(int w, int h)
{
	if (s_ready)
		return;

	claim_display_dma_channels();
	lcd_pins_init();

	if (!dispInit())
		psdl_panic("picosdl: dispInit() failed");

	/* The game's canvas is shorter than the panel. Centre it and paint the
	 * bars once - nothing draws there again, so they stay black. */
	s_offset_y = (PSDL_PICO_PANEL_H - h) / 2;
	if (s_offset_y < 0)
		s_offset_y = 0;

	struct Rect all = { 0, 0, PSDL_PICO_PANEL_W, PSDL_PICO_PANEL_H };
	struct dmaTransfer *clear = dispDrawOneColor(0x0000, &all);
	if (clear != NULL)
		dispDmaTransferWaitFinish(clear);

	s_band = psdl_surface_wrap_static(s_band_pixels, PSDL_PICO_PANEL_W, BAND_H,
	                                  PSDL_PICO_PANEL_W);

	printf("picosdl: display %dx%d, canvas %dx%d at y=%d\n",
	       PSDL_PICO_PANEL_W, PSDL_PICO_PANEL_H, w, h, s_offset_y);
	s_ready = 1;
}

void psdl_backend_video_present(const Uint8 *pixels, int w, int h, int pitch)
{
	if (!s_ready)
		return;

	/* dispDrawBuffer waits for any transfer still in flight before starting,
	 * so the previous frame is implicitly synced here. The framebuffer is not
	 * written by the DMA, only read, hence the cast. */
	/*
	 * Wait for the previous frame here rather than letting dispDrawBuffer do it
	 * implicitly, so the blocked time can be measured - it is core 0's only idle,
	 * and therefore the whole basis of the load figure.
	 */
	if (s_xfer != NULL) {
		absolute_time_t t0 = get_absolute_time();
		dispDmaTransferWaitFinish(s_xfer);
		psdl_pico_core0_idle_us +=
			(uint32_t)absolute_time_diff_us(t0, get_absolute_time());
		s_xfer = NULL;
	}
	++s_frames;

	/* Bands before the canvas: they block, and doing them first leaves the canvas
	 * transfer as the one still in flight, which is what overlaps with drawing. */
	if (s_bands_on)
		bands_tick();

	struct Rect r = { 0, (int16_t)s_offset_y, (uint16_t)w, (uint16_t)h };
	s_xfer = dispDrawBuffer((void *)(uintptr_t)pixels, (uint32_t)(w * h), &r,
	                        (uint16_t)pitch);
}

void psdl_backend_video_sync(void)
{
	if (s_xfer != NULL) {
		dispDmaTransferWaitFinish(s_xfer);
		s_xfer = NULL;
	}
}

/*
 * Palette writes go straight to the CLUT the PIO chain reads. The transfer in
 * flight is reading it right now, so wait first - otherwise a fade would tear
 * the frame it is fading.
 */
void psdl_backend_palette_set(int first, int ncolors, const SDL_Color *colors)
{
	if (!s_ready)
		return;

	psdl_backend_video_sync();

	static struct ClutEntry entries[256];
	for (int i = 0; i < ncolors; ++i) {
		entries[i].r = colors[i].r;
		entries[i].g = colors[i].g;
		entries[i].b = colors[i].b;
	}
	dispSetClut(first, (uint32_t)ncolors, entries);
}
