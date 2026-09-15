/*
 * picosdl's Pico backend: board wiring and the resource map.
 *
 * Three peripherals share one chip here, and two of them were written without
 * knowing about the other. Keeping the allocation in one place is the only way
 * this stays debuggable.
 *
 *   PIO0 SM0,SM1   display    - hardcoded in dispPioSt7789.c
 *   PIO1 SM0       I2S out
 *   PIO0 SM2       CYW43 radio (claimed by the SDK; it takes whatever is free)
 *
 *   DMA 0..3       display    - hardcoded, and NOT claimed by the driver, so
 *                               the video backend claims them on its behalf
 *                               before anything else can be handed them
 *   DMA 4,5        I2S data + control (claimed)
 *   DMA 6,7        CYW43 (claimed)
 *
 *   DMA_IRQ_0      I2S, handled on core 1
 *
 * Init order is therefore load-bearing: video first, because it has to claim
 * the display's hardcoded channels before the CYW43 driver asks for "any free
 * channel" and is given one of them.
 */
#ifndef PICOSDL_PICO_H
#define PICOSDL_PICO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The system clock. 138 MHz.
 *
 * Note which way round this is: the RP2350's SDK default is 150 MHz, so this is
 * a *down*clock, not an overclock. That also means the QMI flash timing the
 * bootrom set up for 150 MHz stays valid, and nothing about the core is being
 * pushed. It was 128 MHz, chosen because both vendored drivers had been
 * characterised there.
 *
 * 138 was picked by enumerating every frequency the SDK's PLL search can
 * actually reach - check_sys_clock_khz(), 12 MHz reference, fbdiv 16..320, VCO
 * 750..1600 MHz, two postdivs - and scoring each on the sample rate the I2S
 * divider lands on. 19 are reachable between 130 and 140 MHz; 138 is the best of
 * them, and better than 128 was:
 *
 *   sysclk    I2S divider   rounded to 16.8   actual rate    error
 *   128 MHz     45.3515        45.3516        22049.957 Hz   2.0 ppm
 *   138 MHz     48.8946        48.8945        22050.012 Hz   0.5 ppm
 *
 * It is VCO 1380 MHz with postdivs 5 and 2. What it buys, at 7.8% more clock:
 * the ST7789 push of a full 320x200 frame drops from 17.00 ms to 15.77 ms, and
 * core 1 gets the same 7.8% more room for the mixer.
 *
 * The cost is the panel, and it is the only thing here being overclocked. SCK is
 * structurally sysclk/2 - the ST7789 PIO program clocks from sideset across a
 * two-instruction loop, so there is no divider involved - which takes it from
 * 64.0 MHz to 69.0 MHz against a 62.5 MHz datasheet maximum. 64 was already 2.4%
 * over and known good on this panel; 69 is 10.4% over. If a panel ever shows
 * torn or speckled pixels, this is the first number to put back.
 *
 * Callers must set this before stdio_init_all() - see the note in the demo's
 * main().
 */
#define PSDL_PICO_SYS_CLOCK_KHZ 138000

/* The panel is 320x240 and the game's canvas is 320x200, so the picture is
 * letterboxed rather than stretched. */
#define PSDL_PICO_PANEL_W 320
#define PSDL_PICO_PANEL_H 240

/* I2S, from the MAX98357A bring-up. LRCLK is always BCLK+1: the PIO
 * program side-sets two bits based at bclkPin, so it cannot be moved. */
#define PSDL_PICO_I2S_BCLK_PIN 2
#define PSDL_PICO_I2S_DATA_PIN 4

/* Audio output rate. 22050 Hz is a compromise: high enough for the game's digi
 * samples and for OPL3 music later, low enough that core 1 has room to
 * synthesise. See the divider note in pio-i2s.c. */
#ifndef PSDL_PICO_AUDIO_RATE
#define PSDL_PICO_AUDIO_RATE 22050
#endif

/* True once a Bluetooth keyboard is connected and delivering reports. */
bool psdl_pico_keyboard_connected(void);

/* Human-readable one-liner about the input state, for status displays. */
const char *psdl_pico_input_status(void);

#endif /* PICOSDL_PICO_H */
