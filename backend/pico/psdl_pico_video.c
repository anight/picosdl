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

#include "psdl_font.h"
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

/*
 * 9x14 rather than 5x7. The bands are 20 rows of a physical panel, and 7-pixel
 * glyphs are legible there but small; 14 leaves three rows of margin top and
 * bottom. Width is what bounds the choice, not height: the widest header this
 * prints is "100 FPS   CORE0 100%   CORE1 100%", 33 characters, which at 9 px
 * each is 297 of the 320 available. The next size up in the family, 10x20, needs
 * 330 and would clip.
 */
#define BAND_FONT (&PSDL_Font9x14)

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
#define BAND_IDX_BG   0
#define BAND_IDX_FG   1
#define BAND_IDX_ICON 2

static SDL_Color s_band_fg = { 255, 255, 255, SDL_ALPHA_OPAQUE };
static SDL_Color s_band_bg = {   0,   0,   0, SDL_ALPHA_OPAQUE };

#if PSDL_COLOR_DEPTH == 16
/*
 * At 16bpp the bands are drawn exactly as below - the font blitter into the 8bpp
 * band buffer, because that is what PSDL_FontDraw() writes - and then expanded to
 * RGB565 here on the way out.
 *
 * Which makes them *simpler* than at 8bpp, where the CLUT is expanded at push
 * time and the band therefore has to borrow two palette slots and give them back
 * (see BAND_IDX_BG). Nothing is shared at this depth: the colours asked for are
 * written into the pixels, and no client palette exists to collide with.
 *
 * 12.8 KB of staging, static for the same reason the 8bpp band buffer is - a
 * long-lived allocation at the bottom of a LIFO arena is what exhausted it once
 * already.
 */
static Uint16 s_band565[PSDL_PICO_PANEL_W * BAND_H];

static Uint16 color_to_565(SDL_Color c)
{
	return (Uint16)(((c.r * 31 / 255) << 11) | ((c.g * 63 / 255) << 5) | (c.b * 31 / 255));
}
#endif

#if PSDL_HAVE_BT_KEYBOARD
/*
 * The Bluetooth rune, in the top right corner: blue when a keyboard is connected,
 * grey when not. Same 9x14 cell and the same column-major layout as the font, so
 * it is drawn by the same loop.
 *
 * It is the one thing on the panel that says whether the radio found anything.
 * Without it the only way to know is the serial console, which is exactly the
 * situation this replaces.
 */
#define BAND_ICON_W 9
#define BAND_ICON_H 14
/* Inset from the right edge. The glyph cell has no side bearing of its own, so
 * without this the rune touches the last column of the panel. */
#define BAND_ICON_MARGIN 4
static const Uint16 band_icon_bt[BAND_ICON_W] = {
	0x0000, 0x0208, 0x0110, 0x00A0, 0x1FFF, 0x08A2, 0x0514, 0x0208, 0x0000,
};
static const SDL_Color s_icon_on  = {  32, 140, 255, SDL_ALPHA_OPAQUE };
static const SDL_Color s_icon_off = {  90,  90,  90, SDL_ALPHA_OPAQUE };
#endif
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

#if PSDL_COLOR_DEPTH == 8
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
#if PSDL_HAVE_BT_KEYBOARD
	clut_write(BAND_IDX_ICON,
	           psdl_pico_bt_connected() ? s_icon_on : s_icon_off);
#endif
}

/* And give them back, from the palette rather than from the hardware: picosdl's
 * SDL_Palette is the authority on what the client last set. */
static void band_clut_restore(void)
{
	const SDL_Color *pal = PSDL_GlobalPalette()->colors;
	clut_write(BAND_IDX_BG, pal[BAND_IDX_BG]);
	clut_write(BAND_IDX_FG, pal[BAND_IDX_FG]);
#if PSDL_HAVE_BT_KEYBOARD
	clut_write(BAND_IDX_ICON, pal[BAND_IDX_ICON]);
#endif
}
#endif /* PSDL_COLOR_DEPTH == 8 */

/* Render one band into the shared buffer and push it, waiting for it to land. */
#if PSDL_HAVE_BT_KEYBOARD
/*
 * Straight into the band buffer rather than through the blitter. The font goes
 * the long way round on purpose - it exercises the sprite path - but this is one
 * 9x14 bitmap in a buffer we own, and a scratch surface for it would be
 * ceremony.
 */
static void band_draw_icon(int x, int y)
{
	for (int col = 0; col < BAND_ICON_W; ++col) {
		for (int row = 0; row < BAND_ICON_H; ++row) {
			if (!((band_icon_bt[col] >> row) & 1))
				continue;
			int px = x + col, py = y + row;
			if (px < 0 || px >= PSDL_PICO_PANEL_W || py < 0 || py >= BAND_H)
				continue;
			s_band_pixels[py * PSDL_PICO_PANEL_W + px] = BAND_IDX_ICON;
		}
	}
}
#endif

/*
 * `align_left` because the two bands want different things. The header's fields are
 * padded to three columns, so the line is exactly as wide whatever the figures
 * say; anchoring it to the left edge then pins every digit to one position, and
 * nothing on the line ever moves. Centring it would have shifted the lot sideways
 * for no reason. The footer is a fixed string and centring is what suits it.
 */
static void band_push(int y, const char *text, int align_left, int with_icon)
{
	if (s_band == NULL)
		return;

	memset(s_band_pixels, BAND_IDX_BG, sizeof(s_band_pixels));

	int text_y = (BAND_H - BAND_FONT->height) / 2;
	int w = PSDL_FontWidth(BAND_FONT, text);
	int x = align_left ? 0 : (PSDL_PICO_PANEL_W - w) / 2;
	if (x < 0)
		x = 0;
	PSDL_FontDraw(BAND_FONT, s_band, x, text_y, BAND_IDX_FG, text);

#if PSDL_HAVE_BT_KEYBOARD
	/* Right-hand end of the band, on the text's own line. */
	if (with_icon)
		band_draw_icon(PSDL_PICO_PANEL_W - BAND_ICON_W - BAND_ICON_MARGIN,
		               text_y);
#else
	(void)with_icon;
#endif

	struct Rect r = { 0, (int16_t)y, PSDL_PICO_PANEL_W, BAND_H };
#if PSDL_COLOR_DEPTH == 16
	const Uint16 fg565 = color_to_565(s_band_fg);
	const Uint16 bg565 = color_to_565(s_band_bg);
#if PSDL_HAVE_BT_KEYBOARD
	const Uint16 icon565 = color_to_565(psdl_pico_bt_connected() ? s_icon_on : s_icon_off);
#endif
	for (unsigned i = 0; i < PSDL_PICO_PANEL_W * BAND_H; ++i) {
		switch (s_band_pixels[i]) {
		case BAND_IDX_FG:   s_band565[i] = fg565; break;
#if PSDL_HAVE_BT_KEYBOARD
		case BAND_IDX_ICON: s_band565[i] = icon565; break;
#endif
		default:            s_band565[i] = bg565; break;
		}
	}
	struct dmaTransfer *t = dispDrawBuffer16(s_band565,
	                                         PSDL_PICO_PANEL_W * BAND_H, &r,
	                                         PSDL_PICO_PANEL_W);
#else
	struct dmaTransfer *t = dispDrawBuffer(s_band_pixels,
	                                       PSDL_PICO_PANEL_W * BAND_H, &r,
	                                       PSDL_PICO_PANEL_W);
#endif
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

	snprintf(s_header, sizeof(s_header), "fps:%3u c0:%3u%% c1:%3u%%", fps, c0, c1);
#else
	snprintf(s_header, sizeof(s_header), "fps:%3u c0:%3u%%", fps, c0);
#endif

#if PSDL_COLOR_DEPTH == 8
	band_clut_override();
#endif
	band_push(0, s_header, 1, 1);
	band_push(PSDL_PICO_PANEL_H - BAND_H, s_footer, 0, 0);
#if PSDL_COLOR_DEPTH == 8
	band_clut_restore();
#endif
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
		{ PSDL_BOARD_LCD_DC_PIN,    true  },
		{ PSDL_BOARD_LCD_CS_PIN,    true  },
		{ PSDL_BOARD_LCD_SCK_PIN,   true  },
		{ PSDL_BOARD_LCD_MOSI_PIN,  true  },
		{ PSDL_BOARD_LCD_MISO_PIN,  false },
		{ PSDL_BOARD_LCD_RESET_PIN, true  },
	};

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
		gpio_init(pins[i].pin);
		gpio_set_dir(pins[i].pin, pins[i].is_output);
		if (pins[i].is_output)
			gpio_put(pins[i].pin, 1);
	}

	/* Reset pulse: active low. */
	gpio_put(PSDL_BOARD_LCD_RESET_PIN, 0);
	sleep_ms(10);
	gpio_put(PSDL_BOARD_LCD_RESET_PIN, 1);
	sleep_ms(120);
}

void psdl_backend_video_init(int w, int h)
{
	if (s_ready)
		return;

	claim_display_dma_channels();
	lcd_pins_init();

	/* The driver has no board of its own: it is told how this one is wired.
	 * board.h is the only place these numbers exist. */
	static const struct dispPinout lcd_pins = {
		.dnc   = PSDL_BOARD_LCD_DC_PIN,
		.cs    = PSDL_BOARD_LCD_CS_PIN,
		.sck   = PSDL_BOARD_LCD_SCK_PIN,
		.mosi  = PSDL_BOARD_LCD_MOSI_PIN,
		.miso  = PSDL_BOARD_LCD_MISO_PIN,
		.reset = PSDL_BOARD_LCD_RESET_PIN,
	};

	if (!dispInit(&lcd_pins, PSDL_COLOR_DEPTH))
		psdl_panic("picosdl: dispInit() rejected the pinout in board.h - "
		           "every pin must be 0..31 and SCK must be CS + 1");

	/* The game's canvas is shorter than the panel. Centre it and paint the
	 * bars once - nothing draws there again, so they stay black. */
	s_offset_y = (PSDL_PICO_PANEL_H - h) / 2;
	if (s_offset_y < 0)
		s_offset_y = 0;

	struct Rect all = { 0, 0, PSDL_PICO_PANEL_W, PSDL_PICO_PANEL_H };
	struct dmaTransfer *clear = dispDrawOneColor(0x0000, &all);
	if (clear != NULL)
		dispDmaTransferWaitFinish(clear);

	/* Always 8bpp, whatever the build depth: PSDL_FontDraw() and the blitters
	 * write palette indices, and at depth 16 the result is expanded to RGB565 in
	 * band_push() on its way out. */
	s_band = psdl_surface_wrap_static(s_band_pixels, PSDL_PICO_PANEL_W, BAND_H,
	                                  PSDL_PICO_PANEL_W, &psdl_pixel_format_index8);

	printf("picosdl: display %dx%d, canvas %dx%d at y=%d\n",
	       PSDL_PICO_PANEL_W, PSDL_PICO_PANEL_H, w, h, s_offset_y);
	s_ready = 1;
}

/*
 * Wait for the panel, counting the time as core 0 idle.
 *
 * It is core 0's largest single idle and therefore the whole basis of the load
 * figure on the status band, so it is measured here rather than left to
 * dispDrawBuffer's implicit wait.
 */
static void wait_for_panel(void)
{
	if (s_xfer == NULL)
		return;
	absolute_time_t t0 = get_absolute_time();
	dispDmaTransferWaitFinish(s_xfer);
	psdl_pico_core0_idle_us += (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
	s_xfer = NULL;
}

/*
 * Hand a rectangle of a caller's buffer to the panel.
 *
 * One implementation for both depths. The difference is entirely in which driver
 * entry point takes the data and what unit its stride is in - the 8bpp path goes
 * through the CLUT expansion in the PIO, the 16bpp path is already what the panel
 * wants - and neither touches a pixel on the CPU.
 *
 * `pitch` is in bytes here, as it is in SDL_Surface and in the public API;
 * dispDrawBuffer16() wants pixels, so the 16bpp path divides.
 *
 * Always asynchronous: this arms the transfer and returns, and the next present
 * waits for it. Whether that is safe is the client's to know - it owns the
 * framebuffers and picosdl cannot see how many it cycles - so a client drawing
 * into the buffer it just presented calls PSDL_PresentSync() first.
 */
static struct dmaTransfer *push(const Uint8 *pixels, int pitch,
                                int x, int y, int w, int h, int offset_y)
{
	struct Rect r = { (int16_t)x, (int16_t)(y + offset_y), (uint16_t)w, (uint16_t)h };
	const Uint8 *origin = pixels + (size_t)y * pitch + (size_t)x * PSDL_BYTES_PER_PIXEL;

#if PSDL_COLOR_DEPTH == 16
	return dispDrawBuffer16((void *)(uintptr_t)origin, (uint32_t)(w * h), &r,
	                        (uint16_t)(pitch / 2));
#else
	return dispDrawBuffer((void *)(uintptr_t)origin, (uint32_t)(w * h), &r,
	                      (uint16_t)pitch);
#endif
}

/*
 * Where the canvas sits on the panel.
 *
 * Computed from the height being pushed rather than cached from init, because a
 * client presenting its own buffer picks its own size and need not match the
 * PSDL_SCREEN_H the backend was initialised with.
 */
static int letterbox_offset(int h)
{
	int offset_y = (PSDL_PICO_PANEL_H - h) / 2;
	return offset_y < 0 ? 0 : offset_y;
}

void psdl_backend_video_present_rect(const Uint8 *pixels, int pitch,
                                     int x, int y, int w, int h)
{
	if (!s_ready)
		return;

	wait_for_panel();

	s_xfer = push(pixels, pitch, x, y, w, h, s_offset_y);
}

void psdl_backend_video_present(const Uint8 *pixels, int w, int h, int pitch)
{
	if (!s_ready)
		return;

	wait_for_panel();
	++s_frames;

	/* Bands before the canvas: they block, and doing them first leaves the canvas
	 * transfer as the one still in flight, which is what overlaps with drawing. */
	if (s_bands_on)
		bands_tick();

	s_xfer = push(pixels, pitch, 0, 0, w, h, letterbox_offset(h));
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
