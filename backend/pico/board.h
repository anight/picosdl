/*
 * The board: every GPIO pin and the system clock, in one place.
 *
 * Everything the backend knows about how this hardware is wired is here, and
 * nothing else defines a pin number. The point is that porting picosdl to a board
 * with a different layout, or changing the clock, should mean editing this file
 * and nothing else.
 *
 * What is NOT here: the CYW43 radio's pins. On a Pico 2 W those are internal to
 * the module and the SDK's cyw43 driver owns them, so they are not ours to name.
 * Nothing in picosdl may use them.
 */
#ifndef PSDL_BOARD_H
#define PSDL_BOARD_H

/* ========================================================================
 * System clock
 * ======================================================================== */

/*
 * 138 MHz.
 *
 * ---- Before changing this, read the rest of this comment. ----
 *
 * The first thing to know is which way round it is. The RP2350's SDK default is
 * 150 MHz, so 138 is a *down*clock: the core is not being pushed, and the QMI
 * flash timing the bootrom set up for 150 MHz stays valid at anything below it.
 * The part that IS overclocked is the display, and that is the number to watch.
 *
 * Four things move when you change this, in rough order of how likely each is to
 * bite.
 *
 * 1. THE DISPLAY CLOCK, and it is not adjustable separately.
 *
 *    The ST7789 driver clocks SCK from PIO sideset across a two-instruction loop
 *    with both state machines at clkdiv = 1, so
 *
 *        SCK = sysclk / 2, always.
 *
 *    The ST7789 datasheet maximum is 62.5 MHz, i.e. 125 MHz of system clock. At
 *    138 MHz, SCK is 69.0 MHz - 10.4% over. That works on the panel this was
 *    developed against and is the main thing a different panel might not tolerate.
 *    The failure looks like torn, speckled or shifted pixels, not a blank screen.
 *    If you see that, come here first and lower this number.
 *
 * 2. HOW LONG A FRAME TAKES TO PUSH, which is what the frame rate is made of.
 *
 *    The per-pixel loop is 34 cycles, not 32: `SET Y,15` and the outer `JMP X--`
 *    each cost one with the clock parked, so bits occupy 32 of 34 cycles. For a
 *    320x200 canvas,
 *
 *        push = 64000 x 34 / sysclk
 *
 *    which is 15.8 ms at 138 MHz against a measured ~21 ms frame. The link, not
 *    the CPU, is what the frame rate is waiting for.
 *
 * 3. THE I2S SAMPLE RATE, which is computed at run time and can reject you.
 *
 *    pio-i2s.c works out sysclk / (rate x channels x bitDepth x 2) and programs it
 *    into a 16.8 fixed-point divider, so the rate you get is quantised to steps of
 *    1/256 of that. It panics at boot if the resulting error exceeds 0.5%, so a bad
 *    choice fails loudly rather than playing out of tune - but errors well under
 *    that are still worth avoiding, and they do not vary smoothly with the clock.
 *
 * 4. HOW MUCH ROOM CORE 1 HAS TO MIX. Linear in the clock. The game reports it as
 *    a percentage of each audio block's budget; at 138 MHz it sits at 21-22%
 *    average.
 *
 * ---- Choosing a value ----
 *
 * Not every frequency is reachable. The SDK searches a 12 MHz reference, fbdiv
 * 16..320, a 750-1600 MHz VCO and two postdividers; set_sys_clock_khz() returns
 * false for anything that does not fall out of that, and picosdl asks for it with
 * `required = true`, so an unreachable value is a panic at boot.
 *
 * These are reachable, and are the ones worth wanting - every row keeps 8 kHz
 * exact and 22050 Hz within a few parts per million:
 *
 *     sysclk     SCK      push    22050 Hz     panel
 *     101000   50.5 MHz  21.54ms   0.2 ppm   within spec
 *     115200   57.6 MHz  18.89ms   2.0 ppm   within spec
 *     125000   62.5 MHz  17.41ms  11.6 ppm   exactly at spec
 *     128000   64.0 MHz  17.00ms   2.0 ppm   2.4% over   (the previous default)
 *     130800   65.4 MHz  16.64ms   4.6 ppm   4.6% over
 *     135000   67.5 MHz  16.12ms   8.3 ppm   8.0% over
 *     136500   68.2 MHz  15.94ms   3.8 ppm   9.1% over
 *     138000   69.0 MHz  15.77ms   0.5 ppm  10.4% over   <-- current
 *     139500   69.8 MHz  15.60ms   4.8 ppm  11.7% over
 *     145200   72.6 MHz  14.99ms   5.2 ppm  16.2% over
 *     148000   74.0 MHz  14.70ms   2.7 ppm  18.4% over, and near the 150 MHz SDK
 *                                                       default - above that the
 *                                                       core is overclocked too
 *
 * For a rate other than 22050 Hz, or a value not in the table, compute the error
 * rather than guessing it:
 *
 *     div    = sysclk / (rate * 2 * 32 * 2)
 *     actual = sysclk / (round(div * 256) / 256) / (2 * 32 * 2)
 *
 * Callers must apply this before stdio_init_all(): changing the system clock
 * re-parents clk_peri, and a UART set up at the old clock then has the wrong
 * baud. See the note in the demo's main().
 */
#define PSDL_BOARD_SYS_CLOCK_KHZ 138000

/* ========================================================================
 * Display - ST7789 over PIO0, 4-wire SPI
 * ========================================================================
 *
 * The driver in pio-st7789 has its own pinout.h with its own names; it derives
 * them from these, so these are the only definitions. MISO is
 * declared because the driver configures the pin, though nothing reads it - the
 * panel is write-only in this design.
 *
 * Chip select and SPI clock must stay consecutive: the PIO program side-sets two
 * bits based at the CS pin, so SCK has to be CS + 1.
 */
#define PSDL_BOARD_LCD_DC_PIN      8   /* data / command select     */
#define PSDL_BOARD_LCD_CS_PIN      9   /* active low; sideset base  */
#define PSDL_BOARD_LCD_SCK_PIN    10   /* must be CS + 1            */
#define PSDL_BOARD_LCD_MOSI_PIN   11
#define PSDL_BOARD_LCD_MISO_PIN   12   /* configured, never read    */
#define PSDL_BOARD_LCD_RESET_PIN  15   /* active low                */

/* The panel is 320x240. A 320x200 canvas is letterboxed into it rather than
 * stretched, which is why psdl_pico_video.c carries a y offset. */
#define PSDL_BOARD_PANEL_W       320
#define PSDL_BOARD_PANEL_H       240

/* ========================================================================
 * Audio - MAX98357A over I2S on PIO1
 * ========================================================================
 *
 * Whether audio is built at all is a build choice: PICOSDL_AUDIO in
 * CMakeLists.txt. With it off none of this is used, PIO1 and DMA channels 4 and 5
 * are free, core 1 has nothing to do, and SDL_OpenAudio() fails with a clear
 * error. The pins below are still the board's, which is why they stay here.
 *
 * LRCLK is always BCLK + 1 and cannot be moved: the PIO program side-sets two
 * bits based at the BCLK pin.
 */
#define PSDL_BOARD_I2S_BCLK_PIN    2
#define PSDL_BOARD_I2S_LRCLK_PIN   3   /* = BCLK + 1, fixed by the PIO program */
#define PSDL_BOARD_I2S_DATA_PIN    4

/* ========================================================================
 * Joystick - two analog axes and a button
 * ========================================================================
 *
 * The axis pins have to be 26..29: that is where the RP2350's ADC inputs are, and
 * joystick.c derives the ADC input number as pin - 26.
 *
 * X and Y are not in pin order. That is how the stick is wired, it has been
 * checked against the hardware, and a README once had it the other way round -
 * so do not "tidy" it by swapping them.
 */
#define PSDL_BOARD_JOY_X_PIN      27   /* ADC1 */
#define PSDL_BOARD_JOY_Y_PIN      26   /* ADC0 */
#define PSDL_BOARD_JOY_BUTTON_PIN 22   /* active low */

/* Y reads inverted on this stick: pushing up lowers the ADC count. The input
 * backend negates once more on the way to SDL, whose Y is positive-downward
 * while joystick.c reports positive-up. */
#define PSDL_BOARD_JOY_INVERT_X   false
#define PSDL_BOARD_JOY_INVERT_Y   true

/* ========================================================================
 * Game controller - Adafruit Gamepad QT on I2C
 * ========================================================================
 *
 * A seesaw device: an ATtiny817 running Adafruit's firmware, which presents the
 * two stick axes as ADC channels and the six buttons as GPIO pins. Optional - if
 * nothing answers at the address, picosdl carries on with the analog stick alone.
 *
 * The pins are fixed by the RP2350's mux, which offers I2C in a strict four-pin
 * cycle: SDA only on even pins, SCL only on odd, alternating instance every two.
 * GP6/GP7 are I2C1, and they are the only free pair on this board that the
 * display carrier does not also use for its touch controller or SD slot.
 *
 * 400 kHz and the 100 us read delay are measured, not guessed. Adafruit's
 * CircuitPython driver waits 8 ms between writing a register pointer and reading
 * the answer, which is a safe default for any seesaw on any bus but would cost
 * 24 ms for the three registers a poll needs - longer than a frame. In practice
 * this device needs no added delay at all: 200 hardware-ID reads and 100 ADC reads
 * came back perfect with zero. The 100 us is margin, and the cost is 1.03 ms per
 * poll against a ~21 ms frame. 1 MHz works too but is outside Fast-mode spec for a
 * Stemma QT cable's pull-ups and saves only 0.24 ms.
 */
/* Whether the driver is built at all is a build choice, not a property of the
 * board: see PICOSDL_INPUT_GAMEPAD in CMakeLists.txt. What is a property of the
 * board is which pins it is on, which is what follows. */
#define PSDL_BOARD_GAMEPAD_I2C         i2c1   /* GP6/GP7 are I2C1, not I2C0 */
#define PSDL_BOARD_GAMEPAD_SDA_PIN     6
#define PSDL_BOARD_GAMEPAD_SCL_PIN     7
#define PSDL_BOARD_GAMEPAD_BAUD        400000
#define PSDL_BOARD_GAMEPAD_ADDR        0x50   /* seesaw default; jumpers give 51-53 */
#define PSDL_BOARD_GAMEPAD_READ_DELAY_US 100

/*
 * Stick orientation, as wired and oriented in this build. X reads backwards -
 * pushing left gives a rising ADC count - so it is inverted here.
 *
 * This is the same kind of fact as PSDL_BOARD_JOY_INVERT_* above and lives in the
 * same place: which way a stick is physically installed. It is deliberately NOT
 * where button assignments live. Those are a game's control bindings, not a
 * property of the board, and picosdl reports the pad's real buttons so that a
 * client asking for "button A" gets the one labelled A.
 */
#define PSDL_BOARD_GAMEPAD_INVERT_X  true
#define PSDL_BOARD_GAMEPAD_INVERT_Y  false

#endif /* PSDL_BOARD_H */
