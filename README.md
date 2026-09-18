# picosdl

A small subset of SDL2 for the RP2040 and RP2350, with a real backend: an ST7789
panel, an I2S DAC, a Bluetooth HID keyboard and an analog stick.

It exists so that a game written against the SDL2 API can be built for a
microcontroller without being rewritten. It is not a port of SDL — it implements
the calls a 2D game actually makes, in a way that suits the hardware, and stops
there. Roughly 2500 lines.

It was extracted from a port of Prince of Persia, which is why the API surface
is shaped the way it is: where a decision had to be made, it went the way that
game needed. Nothing in the library knows about that game, and it links against
none of it.

## What makes it small

Three decisions, each of which removes a whole category of code.

### One pixel format

Everything is 8bpp indexed against a single global 256-entry palette. No format
negotiation, no conversion, no generic blitter.

This is not a compromise imposed on the client — it is what the hardware already
does. The ST7789 driver takes a palettised buffer and a CLUT and expands indices
to RGB565 in the PIO/DMA chain, so the CPU never touches a pixel on its way out.
That is VGA mode 13h, which is what a lot of DOS-era 2D games already are.

The consequence worth naming: **the palette is the display hardware.**
`SDL_SetPaletteColors` writes the CLUT directly, so a full-screen fade costs 256
register writes rather than touching 64000 pixels, and a screen flash is one
palette entry. Effects that would be prohibitive on this CPU become free.

A build has one pixel format, but there are two builds. `PSDL_COLOR_DEPTH=16`
serves a client that produces RGB565 itself, and it earns its place by *removing*
this layer rather than adding a second one beside it — see
[Direct colour](#direct-colour) below. The default is 8 and everything above is
what that means.

### Nothing allocates

There is no `malloc` anywhere in the library. Surfaces come from three fixed
regions, and a flag on the surface records which, so `SDL_FreeSurface` can
dispatch without a general allocator underneath:

| region | what it holds | freeing |
|---|---|---|
| `PSDL_SURF_EXTERN` | pixels owned by the caller, typically `const` data in XIP | no-op |
| `PSDL_SURF_STATIC` | the screen-buffer pool, 320×200 each | returns a slot |
| `PSDL_SURF_ARENA` | a LIFO bump arena | pops the stack |

Orthogonally, `PSDL_SURF_CONST` marks a surface whose **object** — not just its
pixels — lives in flash. Those cannot be written to at all: a refcount
decrement, a lock counter or a colour-key change is a write to XIP and faults.
Every mutating entry point checks the flag first and does nothing. That is the
single most important invariant in `psdl_surface.c`, and it is what lets a
client keep its entire sprite set in flash and blit straight out of it.

The arena is a bump pointer because its expected client releases in strict stack
order, which makes fragmentation structurally impossible. Out-of-order frees are
tolerated but do not reclaim until the top of the stack is dead — so a non-LIFO
caller shows up as arena pressure rather than as corruption.

Full-screen surfaces get their own pool rather than coming from the arena: at
62.5 KB each they would otherwise force the arena to be sized for them and waste
it the rest of the time.

### Scancodes need no translation

`SDL_Scancode` values *are* USB HID usage codes for every key a game cares
about. The Bluetooth stack hands over a usage; that usage is the scancode; it
indexes the key-state array directly. There is no mapping table because there
is nothing to map.

### The game controller is a real SDL_GameController

`SDL_IsGameController()` reports a pad when one is attached, and the
`SDL_CONTROLLER*` events are real. That matters more than presenting another
joystick would: SDL's controller API is *mapped*, so a client gets "button Y"
rather than "button 3", and a pad with a labelled diamond does what the labels say.
As a bare joystick the same pad would offer six anonymous buttons that most games
ignore.

`psdl_gamecontroller.c` is portable and knows nothing about how a pad is attached.
A backend that finds one calls three feeders - present, axis, button - and the axis
and button ones only emit an event when the value changes, so a backend may call
them every poll. A backend with no pad calls none of them, `SDL_IsGameController()`
stays false, and a client falls back to the joystick exactly as before. That is
what the host backend does.

`backend/pico` implements one: an Adafruit Gamepad QT on I2C1, which is a seesaw
device - an ATtiny817 running Adafruit's firmware, with the stick on two ADC
channels and the buttons on GPIO. It is optional; nothing answering at the address
is not an error.

Two things about it are worth knowing before adding another pad. Presence is a
*write* that gets acknowledged, not a read: seesaw is entitled to NAK a bare read
when no register has been selected, and a read-probe looks exactly like absent
hardware. And Adafruit's driver waits 8 ms between writing a register pointer and
reading the answer, which would cost 24 ms for the three registers a poll needs -
longer than a frame. Measured, this device needs none: a poll is 1.03 ms at
400 kHz. See the note in `board.h`.

The board's own analog stick keeps working alongside a pad. Both feed the
controller's left stick and the larger deflection wins per axis - larger rather
than most-recent, because both are polled every frame and neither rests at exactly
zero, so a stick at rest would otherwise overwrite a deflected one milliseconds
later. A client that finds a controller stops listening to the joystick, so without
this the board's stick would go dead the moment a pad was plugged in.

### The letterbox bands are a status line

The panel is 320x240 and a 320x200 canvas is letterboxed into it, leaving two
20-pixel strips that nothing otherwise writes to. `PSDL_StatusBands(SDL_TRUE, fg, bg)`
puts them to use: the header carries picosdl's own frame rate and the load on both
cores, and the footer carries whatever `PSDL_SetFooterText()` was given, centred.

Colours are RGB, not palette indices, and that is the point: the bands are
picosdl's overlay rather than part of the client's indexed world, so they keep
the colours asked for whatever the client does to its palette. Indices would tie
them to it — a client that flashes the screen by rewriting entry 0 would flash
the letterbox with it.

They work at both depths. At 16bpp they are *simpler*: the band is drawn 8bpp
through the font blitter exactly as below and expanded to RGB565 on the way out,
so the two CLUT slots the 8bpp path has to borrow and give back — the next two
paragraphs — are not needed at all, because there is no client palette to collide
with.

Two facts about the panel shape this.

Indices are expanded through the CLUT by the PIO **at push time**, so once a band is
pushed its pixels are fixed RGB and later palette changes do not touch them — unlike
the canvas, which is re-pushed every frame and therefore follows the CLUT. A band
drawn before the client set up its palette would sit there as black on black for
ever. So both bands are repainted once a second along with the figures, which keeps
them legible through fades and start-up with no notion of "the palette changed".

And the push has to be ordered against the frame or the two transfers interleave and
both tear. `dispDrawBuffer()` waits for whatever is in flight before it starts, so the
bands go out from inside `present()`, *before* the canvas is handed over: the canvas
transfer is then the one still running when present returns, so the overlap that buys
the frame rate survives and only the band push blocks — 1.6 ms, once a second.
Pushing them every frame would instead cost 20% of the frame rate.

A consequence worth knowing: the bands update only when the client presents a frame.
Through a long timed wait that draws nothing, the header holds its last value. That is
the price of not having a second, unsynchronised writer to the panel.

**The load figures.** Core 1 runs the mixer and nothing else, so time inside the
client's audio callback over an interval *is* its load. Core 0 idles in two places,
both of which picosdl can see: inside `SDL_Delay()`, which is a real WFE sleep and is
how a client paces a frame, and blocked waiting for the panel transfer. Its load is
the complement of the two together — counting only the panel wait reads about double.
On the client driving this hardest, core 0 sits near 10% at 56 fps, which is the frame
rate being panel-bound rather than CPU-bound, visible as a measurement.

### The serial console

Single keypresses in the serial terminal, no Enter. `h` lists what the build has:

```
v = volume, +/- = louder/quieter, m = mute, h = this
s = bluetooth status, r = forget pairing and re-search, n = re-search, d = bluetooth log
```

The second line only with Bluetooth. Stdin is polled from the input poll with
`getchar_timeout_us(0)` — core 0, outside any interrupt, which is the context stdio
wants — rather than through `btstack_stdin_setup()`. That matters because the
BTstack route meant a build without Bluetooth had no console at all, including the
commands that have nothing to do with it: a board with a stick and a pad and no
keyboard had a volume it could neither read nor change, since the volume keys arrive
over Bluetooth too.

`+`, `-` and `m` hand the audio layer the same scancodes the keyboard's media keys
produce, so the 3 dB ladder and the mute memory cannot drift between the two ways of
reaching them.

### The volume keys do not reach the game

Volume up, volume down and mute are handled by the library and consumed: no
event is queued and `SDL_GetKeyboardState()` never shows them held. They step the
master volume on a 3 dB ladder, and mute toggles, remembering where it was.

This is a deliberate departure from SDL2, where a game does see them - but on a
desktop it never has to care, because the window manager takes the media keys
first. There is no window manager here, so the library stands in for it. Without
that, every game would have to implement volume control or have those keys do
nothing.

## Direct colour

Everything above describes the 8bpp build, which is picosdl's reason for
existing. `PSDL_COLOR_DEPTH=16` is for the client the indexed design does not
suit, and it is worth being precise about which one that is.

An indexed pipeline is a good deal when the art is already indexed: the palette
becomes free effects, the blitter has one format to handle, and the PIO expands
to RGB565 on the way out so the CPU never touches a pixel. A software 3D
renderer inverts every term of that. It computes shading per pixel and produces
RGB565 directly — which is what the panel wants anyway — so reaching an indexed
panel means quantising a whole frame to 256 colours on the CPU, every frame. That
costs the smooth shading the renderer exists to produce, and it costs the time
twice over: once to quantise, and again because there is no palette left to stand
in for the effects it made free.

So at 16 the indexed layer is not bridged, it is **removed**:

| | 8 | 16 |
|---|---|---|
| `SDL_CreateWindow` | returns the canvas | fails, with an error saying why |
| canvas | a 320x200 8bpp surface | none — the client owns its framebuffer |
| present | `SDL_UpdateWindowSurface` | `PSDL_PresentRGB565` |
| palette | the CLUT, written directly | not in the path |
| blitters | all of them | none — nothing to blit into |
| screen pool | 62.5 KB per buffer | not compiled in |

Everything else is untouched and is the reason to still be here: input, events,
timers, audio, the status bands and the serial console all behave identically.

A client is written for one depth or the other and finds out at compile time —
`SDL_CreateWindow()` is absent in effect at 16, `PSDL_PresentRGB565()` is absent
at 8. That is deliberate. A library that quietly handed back a surface in a
format none of its own blitters could draw into would move the failure a long way
from its cause.

The push is asynchronous, like the 8bpp one: `PSDL_PresentRGB565()` starts the
transfer and returns, so the client's next frame overlaps it and the following
call waits. A single-buffered client — which is the normal case here, since a
full-screen RGB565 buffer is 128 KB — calls `PSDL_PresentSync()` before drawing
into the buffer the DMA is still reading. `pitch` is in **pixels**, not bytes,
because a caller holding a `uint16_t*` has that and not a byte count.

```c
static uint16_t framebuffer[320 * 200];      /* the client's own, 128 KB */

for (;;) {
    PSDL_PresentSync();                       /* last frame has landed */
    draw_into(framebuffer);                   /* whatever produces RGB565 */
    PSDL_PresentRGB565(framebuffer, 320, 200, 320);
}
```

The panel runs in RGB565 at both depths — 8bpp reaches it as RGB565 too, expanded
through the CLUT on the way — so the depth changes the PIO and DMA wiring and
nothing about the ST7789 itself. The driver is told which at `dispInit()` and it
is fixed for the life of the program; a client knows at start-up which kind of
pixels it draws, and one of the two paths is dead code for it.

## Layout

```
include/SDL2/SDL.h    the API: types, constants, and the PSDL_ extensions
src/                  portable - no hardware, only the backend interface
  psdl_surface.c        the three regions and the LIFO arena
  psdl_blit.c           the blitters (the only hot path)
  psdl_palette.c        the global CLUT
  psdl_video.c          window and present; the RGB565 present at 16bpp
  psdl_events.c         event ring and key state
  psdl_timer.c          timers, serviced from SDL_PumpEvents
  psdl_audio.c          SDL's pull-callback model over the backend
  psdl_rwops.c          memory RWops; file RWops fails cleanly
  psdl_stubs.c          joystick, and everything meaningless here
  psdl_gamecontroller.c SDL_GameController, fed by whichever backend has a pad
backend/pico/         the hardware half
  bt/                   Bluetooth HID keyboard
  seesaw_gamepad.c      Adafruit Gamepad QT over I2C
pio-st7789/           the ST7789 PIO driver, a submodule
pio-i2s/              the I2S output driver, a submodule
test/                 host tests: builds with cc, runs on a PC
cmake/                the Pico SDK bootstrap, shared with embedders
```

`src/` compiles with a plain host compiler. Everything that touches hardware is
behind the fourteen functions in `psdl_internal.h`, and `test/host_backend.c` is
a complete implementation of them that runs on a PC.

## Extensions beyond SDL

Five blitters, each because the operation it names is one a 2D game wraps SDL in
anyway, and doing it directly avoids a scratch surface and a round trip through
a generic blitter:

- **`PSDL_BlitMirrored`** — draws the source reversed. The usual SDL idiom is to
  allocate a surface, blit into it flipped, draw it and free it, every time a
  sprite faces the other way. Walking the source backwards costs nothing extra.
- **`PSDL_BlitXor`** — XORs palette indices, which is what the DOS-era blitter
  modes did. The alternative is two scratch surfaces per call.
- **`PSDL_BlitOffset`** — adds a constant to each non-transparent index, for
  sprite sets whose sixteen colours live at an offset in the global palette.
- **`PSDL_BlitTransp`** — chooses transparency *per call* rather than per
  surface. This one is not a convenience: the usual idiom is `SDL_SetColorKey`
  on the source before each blit, and that write is not available on a const
  surface in flash. The key is baked in and this picks whether to honour it.
- **`PSDL_BlitMono`** — draws every non-transparent pixel as one colour,
  ignoring the source index. Used for fonts and 1bpp art, where the alternative
  is converting the whole glyph to ARGB8888 first.

Also `PSDL_GlobalPalette()`; `PSDL_SetMasterVolume()` / `PSDL_GetMasterVolume()`,
a single gain applied after everything is mixed; `PSDL_ReportMemory()`, which
prints the high-water marks of all three regions; and `PSDL_DumpArena()`, which
lists the arena entry by entry and is called automatically if the arena runs out.

And on a 16bpp build only, `PSDL_PresentRGB565()` and `PSDL_PresentSync()` — the
whole of the output path at that depth, since there is no window surface to
update. See [Direct colour](#direct-colour).

## Building

As its own project:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or as a subdirectory of a game that has already initialised the Pico SDK:

```cmake
include(picosdl/cmake/pico_sdk_bootstrap.cmake)
project(yourgame C CXX ASM)
pico_sdk_init()
add_subdirectory(picosdl)
target_link_libraries(yourgame PRIVATE picosdl)
```

`CMakeLists.txt` detects which case it is and only calls `project()` and
`pico_sdk_init()` when it is the top-level project.

The ST7789 driver is a submodule, so:

```bash
git submodule update --init
```

### Choosing what to build

Four CMake options say which hardware is present. They are independent, all
default on, and **none is required**:

```bash
cmake -S . -B build \
  -DPICOSDL_INPUT_BT_KEYBOARD=OFF \
  -DPICOSDL_INPUT_JOYSTICK=ON \
  -DPICOSDL_INPUT_GAMEPAD=OFF \
  -DPICOSDL_AUDIO=OFF
```

`PICOSDL_AUDIO=OFF` is for a board with no MAX98357A and no speaker, which is a
normal thing to have. The I2S driver and its PIO program are not built, and
`SDL_OpenAudio()` fails with a clear error rather than pretending — that is what SDL
does when there is no device, so a client that handles the desktop case already
handles this one. SDLPoP sets `digi_unavailable` and plays silently; picosdl's demo
prints the error and carries on. Succeeding and never calling the callback would be
worse: a client would have no way to know, and would spend its mixing budget on
samples nothing consumes.

It also frees PIO1, DMA channels 4 and 5, and core 1, which does nothing else in
this library but mix. `PSDL_SetMasterVolume()` still works — it remembers a value
and applies it to nothing, because it is public API and a client should not need
its own `#if` around a volume control.

Off means absent, not ignored: the driver is not compiled. A build with all three
off is legitimate and produces a library with a display and a speaker and no way to
press anything — which is what a kiosk or a permanent attract-mode demo wants.
Nothing tries to talk you out of it, and `SDL_NumJoysticks()` then reports 0 so a
client can discover there is nothing to read rather than waiting on input that will
never arrive.

Bluetooth is the one worth turning off if you do not need it. It brings in BTstack
twice over — the BLE and Classic stacks, because a keyboard may be either and you
cannot tell from the outside — plus the CYW43 driver and that chip's firmware blob.
The blob alone is 234 KB of flash and is not code, so nothing else can shrink it.

Measured on `picosdl-demo` itself — this library and nothing else — for
`pico2_w` at 8bpp. Everything on is 506,804 bytes of `.text` and 211,336 of
`.bss`; each row is what turning that one option off gives back:

| off | flash | RAM |
|---|---|---|
| Bluetooth | **430,864 B (420.8 KB)** | **22,164 B (21.6 KB)** |
| audio | 4,376 B (4.3 KB) | 5,252 B (5.1 KB) |
| joystick | 1,784 B (1.7 KB) | 28 B |
| gamepad | 2,648 B (2.6 KB) | 16 B |

If you are short of space there is only one of these worth switching off. With
Bluetooth out, the whole demo — this library, its backend, the SDK and the demo
program — is 75,940 bytes of flash, so Bluetooth alone is about five and a half
times everything else in the binary. The other three are about having the
hardware, not about size.

One consequence worth knowing: `psdl_pico_input_status()` reports
`NO KEYBOARD IN THIS BUILD` rather than a link state. The serial console survives —
stdin is polled directly rather than through BTstack, so only the four
Bluetooth-specific commands go with it.

### Choosing the colour depth

`PICOSDL_COLOR_DEPTH` is a different kind of option from the four above. Those
say which hardware is attached; this one changes the shape of the API, so a
client is built for one value or the other and cannot be indifferent to it.

```bash
cmake -S . -B build -DPICOSDL_COLOR_DEPTH=16
```

8 is the default and is everything this README describes. 16 gives up the
indexed layer — no window surface, no palette, no blitters — in exchange for
handing an RGB565 framebuffer straight to the DMA. [Direct
colour](#direct-colour) covers what that is for and what it costs; the short
version is that it suits a client which already produces RGB565 and nothing
else.

Anything other than 8 or 16 is refused at configure time rather than producing a
build that half works.

### The demo

A standalone 8bpp build produces `picosdl-demo`, which is the thing to run first
on a new board. It is an 8bpp program — it opens a window, blits sprites and
animates the CLUT — so it is not built by default at
`PICOSDL_COLOR_DEPTH=16`, where `SDL_CreateWindow()` returns NULL. It still
compiles and links there, and asking for it explicitly gets you a binary that
prints the error and exits; there is just no reason to want one. It exercises a palette animation driven by CLUT writes alone, a
flash-resident sprite blitted plain, mirrored and XORed, text through the keyed
blitter and the LIFO arena, a Bluetooth keyboard, an analog stick, a four-voice
synth in an audio callback and an original tune - and puts the frame rate, the
mixer load as a percentage of each audio block's budget, and the memory report on
screen while doing it.

```bash
cmake -S . -B build && cmake --build build
./picodev.sh flash-and-logs build/picosdl-demo.elf
```

It links this library and nothing else, so it is also the shortest complete example
of using it. `T` is worth knowing about: a steady test tone replacing everything
else, which bisects an audio fault into "the clocking, the DMA chain and the DAC"
versus "whatever produces the samples".

As a subdirectory the demo is off by default, on the assumption that a client does
not want a second binary. `set(PICOSDL_BUILD_DEMO ON)` before `add_subdirectory()`
turns it back on, which is worth doing while bringing a board up.

### Flashing and the console

`picodev.sh` drives a board over SWD with a CMSIS-DAP probe:

```bash
./picodev.sh flash [firmware.elf]   # program and reset
./picodev.sh reset                  # reset, program nothing
./picodev.sh halt                   # stop the cores and silence the audio
./picodev.sh logs                   # watch the console (tail -f, in effect)
./picodev.sh flash-and-logs [fw]    # program, reset, then watch from the first line
```

`halt` stops both cores and leaves them stopped; `reset` starts the board again.
Both cores matters - halting core 0 alone leaves a mixer running on core 1 - and so
does the order: halting does not by itself make a board quiet, because the audio
DMA chain re-triggers itself and keeps cycling its last two buffers into the DAC
with no CPU involved. The PIO state machines have to be stopped, which every
command here does before anything else.

`flash-and-logs` attaches the console reader *before* programming, because a
reader started afterwards has already missed the start-up banner, and drains the
port first so the log does not open with leftovers from the previous run. Only the
board's output goes to stdout, so `./picodev.sh flash-and-logs | tee boot.log`
captures just that.

It refuses to read a console another process already has open. Two readers on one
tty do not take turns - each gets whichever bytes it wins, and both see lines with
pieces missing from the middle. That looks exactly like corruption on the wire and
is worth failing loudly rather than debugging twice.

`PICOPOP_CONSOLE` overrides the device, which otherwise defaults to the probe's
UART bridge found under `/dev/serial/by-id` - stable, unlike `ttyACMn`, which
moves when anything else is plugged in. `PICOPOP_BAUD`, `OPENOCD` and
`PROBE_SERIAL` override the rest.

### Tests

```bash
make -C test        # 56 checks
make -C test asan   # the same, under AddressSanitizer and UBSan
```

They cover the places bugs actually live: clipping at every edge, the mirrored
blit clipped on its *left* edge — which has to read a different source column
than an unclipped one, and an off-by-one there is invisible until a sprite walks
off the side of the screen — LIFO reclamation, screen-pool exhaustion, and
event-ring overflow.

They are 8bpp tests, because that is where the code they exercise lives: the
blitters, the surface regions and the palette are all the indexed half. The
16bpp path has no host coverage — see `TODO.md`.

## The hardware

**Every pin number and the system clock are in `backend/pico/board.h`**, and
nothing else defines either - the ST7789 driver's `pinout.h` derives its
names from there too. Porting to a differently wired board, or changing the clock,
is that one file. The clock comment in it lists which reachable frequencies are
worth wanting and what each does to the panel, the frame time and the sample rate.

**`pico2_w` (RP2350) at 125 MHz** is the default. Below the SDK's 150 MHz default,
so the core is not overclocked, and SCK is sysclk/2 = 62.5 MHz, exactly the ST7789
maximum, so the panel is not either. 128 and 138 MHz are both reachable and both
work here, but they run the panel 2.4% and 10.4% over its rated maximum, which is
not something a different panel has to tolerate. The clock is not arbitrary in the
other direction either:
it keeps the I2S divider exact at 8 kHz (122.070312) and within 12 ppm at 22050.

**`pico_w` (RP2040) builds too** — `-DPICO_BOARD=pico_w`. Nothing in the library
is RP2350-only: the two PIO drivers assemble for both parts, and the backend uses
no instruction or peripheral the RP2040 lacks. The board these are developed and
run on daily is the `pico2_w`, so treat the RP2040 as supported by construction
rather than exercised — it builds clean and the arithmetic below works out, but
the panel, the DAC and the radio have not all been brought up on one at once.

What the smaller part is short of is room, and the numbers say where. The demo
with everything on is 522,896 bytes of flash and 211,720 of `.bss` there, against
a 256 KB main SRAM region — so it fits, with 49 KB left for stack, heap and a
client's own data. That is workable for an 8bpp client whose art is in flash, and
it is not much. A 16bpp client is the harder case: its own 320x200 RGB565
framebuffer is 128 KB, which does not fit beside a Bluetooth build on this part.
Turning Bluetooth off is what makes the space — 421 KB of flash and 22 KB of RAM,
by far the largest lever either board has.

Note that the CYW43 radio is currently required on both, even with the keyboard
off: the configure step refuses a `PICO_BOARD` without Bluetooth support
regardless of `PICOSDL_INPUT_BT_KEYBOARD`. A plain `pico` or `pico2` will not
configure today, which is a gap rather than a decision.

`set_sys_clock_khz()` must be called **before** `stdio_init_all()`. It re-parents
`clk_peri` off `clk_sys`, and stdio derives the UART divisor from `clk_peri` when
it starts — the other order leaves the console at the wrong baud rate.

### Resource map

Three peripherals share one chip and two of the drivers were written without
knowing about the other, so the allocation is kept in one place,
`backend/pico/psdl_pico.h`.

| | |
|---|---|
| PIO0 SM0, SM1 | display — hardcoded in `dispPioSt7789.c` |
| PIO1 SM0 | I2S out |
| PIO0 SM2 | CYW43 radio — the SDK claims whatever is free |
| DMA 0–3 | display — hardcoded, and **not** claimed by the driver |
| DMA 4, 5 | I2S data + control |
| DMA 6, 7 | CYW43 |
| DMA_IRQ_0 | I2S, handled on core 1 |
| GPIO 8–12, 15 | LCD: D/C, CS, SCK, MOSI, MISO, RESET |
| GPIO 2, 3, 4 | I2S BCLK, LRCLK, DIN |
| GPIO 27, 26, 22 | joystick X (ADC1), Y (ADC0), button (active low) |
| GPIO 6, 7 | I2C1 SDA, SCL — game controller (optional) |
| GPIO 0, 1 | UART console |

**Init order is load-bearing.** The display driver hardcodes DMA channels 0–3
and does not claim them, while the CYW43 and I2S drivers both ask the SDK for
"any free channel". So the video backend claims 0–3 on the driver's behalf and
must run first. It panics if any of them is already taken, which turns a silent
corruption into a message.

## Things that had to be got right

Hard-won, and all of them cost real debugging time.

**The RP2350 pad isolation latch.** Pads come up isolated. A raw FUNCSEL write
leaves the latch set and the pin never drives, which presents as dead hardware.
Going through `gpio_set_function()` clears it.

**BTstack must not run its own loop.** Under
`pico_cyw43_arch_threadsafe_background` the run loop is driven by interrupts off
the async_context, so `btstack_run_loop_execute()` parks for ever. The usual
keyboard-host example calls it because it has nothing else to do; here the game
owns the main loop, so it is deliberately never called and BTstack progresses on
its own.

**Audio runs on core 1, and everything on that path is `__not_in_flash_func`.**
BTstack persists pairings to flash, and a flash erase stops XIP — any code
fetched from flash during that window hard-faults. Core 1 is locked out while it
happens, but an interrupt at a higher priority than the lockout's spin could
still preempt it, so the handler must not need flash to run. Audio glitches for
the duration of a pairing write; that is the correct trade.

`PioI2S_init` is called **from core 1** for a related reason:
`irq_set_exclusive_handler` and `irq_set_enabled` act on the calling core's
NVIC, so calling them from core 0 would leave the DMA interrupt firing on the
wrong core and the mixer never running.

**`SDL_LockAudio` is a recursive cross-core lock.** It has to hold off a mixer
on the other core, so it is a hardware spinlock — and those are not recursive.
`psdl_audio_render` holds it across the client callback, so a callback that
itself called `SDL_LockAudio` would take the same spinlock twice on one core and
hang. Tracking the owning core makes the second acquisition a counter bump.

**Timers fire from `SDL_PumpEvents`, not from an alarm interrupt.** A timer
callback that pushes an event would otherwise give the event ring a genuine
interrupt producer and force everything downstream to be interrupt-safe, for no
benefit to a client that polls continuously.

**The audio DMA sustains itself.** The data channel chains to a control channel
holding a two-entry address ring, so finishing one buffer re-triggers the other
for ever and the output never glitches. The consequence is that halting the
cores does not stop the sound: a debugger halt leaves the last two buffers
cycling into the DAC. Stopping the PIO state machines is what silences it, which
is what `picodev.sh` does before programming.

**The display driver holds no pin numbers.** `backend/pico/board.h` is the only
place they exist; `psdl_pico_video.c` fills in a `struct dispPinout` from it and
hands that to `dispInit()`. Upstream carries them as literals in its own header,
which means a board is described in two places that drift apart. `dispInit()`
also checks the one rule the PIO program imposes — SCK must be CS + 1, because
SM1 side-sets two bits based at CS — and refuses rather than leaving the panel
dark with nothing to read.

**The I2S divider check demanded an exact ratio.** Upstream panics unless the
requested divider lands exactly on a multiple of 1/256. The divider's eight
fractional bits are a rounding target, not a constraint, and requiring exactness
rejects almost every rate the library exists to produce: at 32-bit stereo,
11025, 22050 and 44100 Hz are all fractional at 128, 138 and 150 MHz alike, and
only 8000 Hz happens to divide exactly. 22050 Hz at 138 MHz needs 48.894558,
which rounds to a real rate of 22050.012 Hz — an error of 0.5 ppm, four orders
of magnitude below anything audible, and upstream refused to boot on it. The
copy here measures what the rounding costs and compares it against a configurable
tolerance, `PioI2S_MAX_CLOCK_ERROR_PPM`. That is the only place it diverges from
upstream, and it is worth offering back.

## Footprint

Static RAM, measured from a linked image:

| | 8bpp | 16bpp |
|---|---|---|
| screen pool (2 × 320×200) | 128000 | — |
| LIFO arena | 16384 | 16384 |
| surface headers (192) | 13056 | 13056 |
| status band staging (8bpp) | 6400 | 6400 |
| status band staging (RGB565) | — | 12800 |
| event ring (64) | 3584 | 3584 |
| palette | 1024 | 1024 |
| audio mix buffer | 1024 | 1024 |
| arena stack | 768 | 768 |
| **total** | **170240 (166.2 KB)** | **55040 (53.8 KB)** |

The depth changes two rows and nothing else. The screen pool is what an 8bpp
client draws into and does not exist at 16, where the client owns its own
framebuffer — so the 115 KB the library gives back there is not a saving, it is
the client's to spend, and a 320×200 RGB565 buffer is 128 KB of it. The second
band buffer is the cost of keeping the status line at a depth where the PIO does
not expand its pixels.

All of it is tunable: `PSDL_SCREEN_BUFFERS`, `PSDL_ARENA_BYTES`,
`PSDL_MAX_SURFACES`, `PSDL_EVENT_QUEUE_LEN` and `PSDL_AUDIO_BLOCK_FRAMES` are
`#ifndef`-guarded in `src/psdl_internal.h`, so a client can override any of them
from its own build. At 8bpp the screen pool dominates, and a client that
composites directly into the framebuffer rather than into an offscreen buffer can
halve it.

The library contributes **zero** heap. Two SDK functions do allocate, both
one-shot at init — `alarm_pool_create_on_timer_with_unused_hardware_alarm` and
`cyw43_btbus_init` — so a build that poisons `malloc` at link time has to leave
those two alone.

## Status

Running on a Pico 2 W with the panel, the DAC, the radio and the stick all live
at once. The 16bpp path is running on the same board, driven by a software 3D
renderer at 320x200. Known gaps are in `TODO.md`.

## Licence and provenance

**BSD-2-Clause** — see [`LICENSE`](LICENSE).

Permissive on purpose. This library contains no code from the game it was written
for: it is an independent implementation of the slice of SDL2 that game uses, and
everything it depends on is permissive too. Being built alongside a GPL program
does not make a library derived from it, and a copyleft licence here would
contradict the one thing the design keeps insisting on — that picosdl is reusable
by anything, including firmware that is not open source.

Parts that came from elsewhere keep their own terms, and `LICENSE` lists them:
`backend/pico/bt` carries BSD-3-Clause material from BTstack and pico-examples,
the 9x14 font is derived from a public-domain X11 font, and `pio-st7789` and
`pio-i2s` are submodules with their own licence files.
