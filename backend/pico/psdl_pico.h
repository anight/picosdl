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

/* The system clock. 128 MHz, not the RP2040 default of 125 or the RP2350
 * default of 150, because both vendored drivers were characterised at it: it
 * makes the I2S divider exact at 8 kHz and it is the clock the ST7789 PIO
 * timings were measured at. Callers must set this before stdio_init_all() -
 * see the note in the demo's main(). */
#define PSDL_PICO_SYS_CLOCK_KHZ 128000

/* The panel is 320x240 and the game's canvas is 320x200, so the picture is
 * letterboxed rather than stretched. */
#define PSDL_PICO_PANEL_W 320
#define PSDL_PICO_PANEL_H 240

/* I2S, matching pico-test-i2s-max98357a. LRCLK is always BCLK+1: the PIO
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
