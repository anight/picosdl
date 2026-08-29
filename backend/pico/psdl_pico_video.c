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

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"

#include "dispPioSt7789.h"
#include "pinout.h"

#include "psdl_internal.h"
#include "psdl_pico.h"

static struct dmaTransfer *s_xfer;
static int s_offset_y;
static int s_ready;

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
