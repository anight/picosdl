# picosdl

A subset of SDL2 for the RP2040 and RP2350, with a hardware backend: an ST7789
panel over PIO, an I2S DAC, a Bluetooth HID keyboard, an analog stick and an
I2C game controller.

Its purpose is to let a game written against the SDL2 API be built for a
microcontroller without being rewritten. It is not a port of SDL. It implements
the calls a 2D game makes, in the way that suits this hardware, and stops there.
The portable core is about 3,500 lines including the public header.

The API surface is shaped by the program it was extracted from, a port of Prince
of Persia: where a decision had to be made, it was made the way that program
needed. The library contains no code from it and links against none of it.

---

## Contents

- [Quick start](#quick-start)
- [Using the library](#using-the-library)
- [Colour depth](#colour-depth)
- [Building](#building)
- [The hardware backend](#the-hardware-backend)
- [Footprint](#footprint)
- [Tests](#tests)
- [Tooling](#tooling)
- [Implementation notes](#implementation-notes)
- [Source layout](#source-layout)
- [Licence and provenance](#licence-and-provenance)

---

## Quick start

```bash
git submodule update --init          # pio-st7789 and pio-i2s
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A standalone 8bpp build also produces `picosdl-demo`, which links this library
and nothing else and is therefore both the bring-up program for a new board and
the shortest complete example of using the library. It exercises palette
animation driven by CLUT writes alone, a flash-resident sprite blitted plain,
mirrored and XORed, text through the keyed blitter and the arena, a Bluetooth
keyboard, an analog stick and a four-voice synth in an audio callback, and
displays the frame rate, the mixer load and the memory report while doing so.
Pressing `T` substitutes a steady test tone for everything else, which separates
an audio fault into the clocking, DMA chain and DAC on one side and whatever
produces the samples on the other.

```bash
./picodev.sh flash-and-logs build/picosdl-demo.elf   # needs an SWD probe
```

Without a probe, copy `build/picosdl-demo.uf2` onto the board over USB with
BOOTSEL held. The serial console is not available that way, and the console is
most of what the demo reports.

To embed the library in a project that has already initialised the Pico SDK:

```cmake
include(picosdl/cmake/pico_sdk_bootstrap.cmake)
project(yourgame C CXX ASM)
pico_sdk_init()
add_subdirectory(picosdl)
target_link_libraries(yourgame PRIVATE picosdl)
```

`CMakeLists.txt` detects whether it is the top-level project and calls
`project()` and `pico_sdk_init()` only when it is. As a subdirectory the demo is
off by default; `set(PICOSDL_BUILD_DEMO ON)` before `add_subdirectory()` enables
it, which is useful while bringing a board up.

---

## Using the library

### Pixel format and the palette

A build has exactly one pixel format. At the default depth every surface is 8bpp
indexed against a single global 256-entry palette. There is no format
negotiation, no conversion and no generic blitter.

This follows the hardware rather than constraining it. The ST7789 driver takes a
palettised buffer and a CLUT and expands indices to RGB565 in the PIO and DMA
chain, so the CPU never touches a pixel on its way to the panel. That is VGA
mode 13h in hardware, which is what much DOS-era 2D art already is.

The consequence that matters to a client is that **the palette is the display
hardware**. `SDL_SetPaletteColors()` writes the CLUT directly, so a full-screen
fade costs 256 register writes rather than 64,000 pixel writes, and a screen
flash is a single palette entry. Effects that would be prohibitive on this CPU
are close to free.

### Framebuffers and presenting

picosdl allocates no pixels. Every framebuffer belongs to the client, which is
what allows the library to hold no full-screen memory of its own and a client to
pay for exactly the buffers it uses. At 62.5 KB each, that is the difference
between the library costing 40 KB and costing 170.

There are two ways to put a frame on the panel, differing only in whether an
`SDL_Surface` is wanted over the buffer:

```c
static Uint8 canvas[320 * 200];                 /* the client's */

/* With a surface: the blitters, SDL_FillRect() and partial-rect updates. */
SDL_Window  *win = PSDL_CreateWindow(canvas, 320, 200, 320);
SDL_Surface *fb  = SDL_GetWindowSurface(win);   /* wraps canvas, no copy */
SDL_UpdateWindowSurface(win);

/* Without one: a finished framebuffer goes straight to the panel. */
PSDL_PresentBuffer(canvas, 320, 200, 320);      /* pitch in BYTES */
```

A client with its own renderer wants the second; there is nothing a surface adds
to a frame it has already produced. A client that wants somewhere to draw wants
the first. Both work at either colour depth. `SDL_CreateWindow()` is deliberately
absent: its signature has nowhere to put the client's memory, and accepting it
would force a full-screen buffer into the library for every client, including
those that already have one.

A present is asynchronous. It returns once the transfer has started, so drawing
the next frame overlaps it. One transfer runs at a time — there is a single DMA
chain to the panel — and a present drains the previous one before arming its own.
The governing invariant is therefore: **the buffer being read is always the one
most recently presented, and never more than one.**

Two calls let a client act on that without tracking it:

```c
void     PSDL_PresentSync(void);                /* blocking  */
SDL_bool PSDL_BufferBusy(const void *pixels);   /* non-blocking */
```

`PSDL_BufferBusy()` answers for the buffer named, so a client asks about the one
it is about to touch. A buffer that is not the one in flight answers `SDL_FALSE`
immediately; `NULL` asks about the panel rather than about a buffer.

With **one buffer**, the buffer to draw into is the one being read, so wait:

```c
PSDL_PresentSync();
draw_into(fb);
PSDL_PresentBuffer(fb, w, h, pitch);
```

With **two buffers**, filling the free one is always safe, but presenting it is
not free: a present drains the previous transfer first and blocks there. A client
that wants that time back waits in its own code instead:

```c
PSDL_PresentBuffer(a, w, h, pitch);
draw_into(b);                                 /* safe: a is the one in flight */
while (PSDL_BufferBusy(a)) do_something();    /* rather than stalling below */
PSDL_PresentBuffer(b, w, h, pitch);           /* returns at once */
```

Omitting that loop is still correct; the present waits on the client's behalf,
spending the wait inside picosdl rather than on the client's work.

Both calls concern the DMA completing its *read* of the framebuffer, not the
pixels reaching the glass — a few remain in the PIO's FIFO and shifter when they
report done. That is the correct guarantee for reusing the memory and the wrong
one for timing anything visual.

### Memory

There is no `malloc` in the library. Surfaces come from three fixed regions, and
a flag on each surface records which, so `SDL_FreeSurface()` dispatches without a
general allocator beneath it:

| region | contents | freeing |
|---|---|---|
| `PSDL_SURF_EXTERN` | pixels owned by the caller, typically `const` data in XIP | no-op |
| `PSDL_SURF_STATIC` | a static buffer someone else keeps alive | no-op |
| `PSDL_SURF_ARENA` | a LIFO bump arena | pops the stack |

Orthogonally, `PSDL_SURF_CONST` marks a surface whose *object* — not merely its
pixels — lives in flash. Such a surface cannot be written to at all: a refcount
decrement, a lock counter or a colour-key change is a write to XIP and faults.
Every mutating entry point tests the flag first and does nothing. This is the
central invariant of `psdl_surface.c`, and it is what allows a client to keep an
entire sprite set in flash and blit directly out of it.

The arena is a bump pointer because its expected client releases in strict stack
order, which makes fragmentation structurally impossible. Out-of-order frees are
tolerated but do not reclaim until the top of the stack is dead, so a non-LIFO
caller presents as arena pressure rather than as corruption.

### Input

**Keyboard.** `SDL_Scancode` values are USB HID usage codes for every key a game
uses. The Bluetooth stack delivers a usage, that usage is the scancode, and it
indexes the key-state array directly. There is no mapping table because there is
nothing to map.

**Game controller.** `SDL_IsGameController()` reports a pad when one is attached
and the `SDL_CONTROLLER*` events are real. SDL's controller API is mapped, so a
client receives "button Y" rather than "button 3" and a pad with a labelled
diamond behaves as labelled; presented as a bare joystick the same pad would
offer six anonymous buttons that most games ignore.

`psdl_gamecontroller.c` is portable and knows nothing about how a pad is
attached. A backend that finds one calls three feeders — present, axis and button
— of which the axis and button feeders emit an event only when the value changes,
so a backend may call them every poll. A backend with no pad calls none,
`SDL_IsGameController()` stays false, and a client falls back to the joystick.

The Pico backend implements one: an Adafruit Gamepad QT on I2C1, a seesaw device
presenting its stick on two ADC channels and its buttons on GPIO. It is optional;
nothing answering at the address is not an error.

**Joystick.** The board's analog stick works alongside a pad. Both feed the
controller's left stick and the larger deflection wins per axis — larger rather
than most-recent, because both are polled every frame and neither rests at
exactly zero, so a stick at rest would otherwise overwrite a deflected one
milliseconds later. A client that finds a controller stops reading the joystick,
so without this merge the board's stick would go dead the moment a pad was
attached.

### Audio and volume

`psdl_audio.c` presents SDL's pull-callback model over the backend. The mixer
runs on core 1 and does nothing else.

Volume up, volume down and mute are handled by the library and consumed: no event
is queued and `SDL_GetKeyboardState()` never shows them held. They step a master
volume on a 3 dB ladder, and mute toggles while remembering its previous setting.

This departs from SDL2, where a game does see those keys. On a desktop that is
harmless because the window manager takes the media keys first. There is no window
manager here, so the library stands in for one; without it every game would have
to implement volume control or leave those keys inert.

### The status bands

The panel is 320×240. A 320×200 canvas letterboxed into it leaves two 20-pixel
strips that nothing else writes to. `PSDL_StatusBands(SDL_TRUE, fg, bg)` puts them
to use: the header carries picosdl's frame rate and the load on both cores, and
the footer carries whatever `PSDL_SetFooterText()` was given, centred.

Colours are RGB rather than palette indices. The bands are picosdl's overlay
rather than part of the client's indexed world, so they keep the colours asked
for whatever the client does to its palette. Indices would tie them to it — a
client that flashes the screen by rewriting entry 0 would flash the letterbox
with it.

Two properties of the panel shape the implementation.

Indices are expanded through the CLUT by the PIO **at push time**, so once a band
is pushed its pixels are fixed RGB and later palette changes do not affect them,
unlike the canvas, which is re-pushed every frame and therefore follows the CLUT.
A band drawn before the client set up its palette would remain black on black.
Both bands are therefore repainted once a second along with the figures, which
keeps them legible through fades and start-up without any notion of "the palette
changed".

The push must also be ordered against the frame, or the two transfers interleave
and both tear. `dispDrawBuffer()` waits for whatever is in flight before it
starts, so the bands are pushed from inside `present()` *before* the canvas is
handed over. The canvas transfer is then the one still running when present
returns, so the overlap that buys the frame rate survives and only the band push
blocks — 1.6 ms, once a second. Pushing the bands every frame would instead cost
20% of the frame rate.

One consequence: the bands update only when the client presents a frame. Through
a long timed wait that draws nothing, the header holds its last value. That is
the price of not having a second, unsynchronised writer to the panel.

**The load figures.** Core 1 runs the mixer and nothing else, so time spent inside
the client's audio callback over an interval is its load. Core 0 idles in two
places the library can see: inside `SDL_Delay()`, which is a real WFE sleep and is
how a client paces a frame, and blocked waiting for the panel transfer. Its load
is the complement of the two together; counting only the panel wait reads about
double. On the client driving this hardest, core 0 sits near 10% at 56 fps, the
frame rate being panel-bound rather than CPU-bound.

### The serial console

Single keypresses in the serial terminal, without Enter. `h` lists what the build
provides:

```
v = volume, +/- = louder/quieter, m = mute, h = this
s = bluetooth status, r = forget pairing and re-search, n = re-search, d = bluetooth log
```

The second line appears only in a Bluetooth build. Stdin is polled from the input
poll with `getchar_timeout_us(0)` — core 0, outside any interrupt, which is the
context stdio requires — rather than through `btstack_stdin_setup()`. Routing it
through BTstack would leave a build without Bluetooth with no console at all,
including the commands unrelated to it: a board with a stick and a pad but no
keyboard would have a volume it could neither read nor change, the volume keys
also arriving over Bluetooth.

`+`, `-` and `m` hand the audio layer the same scancodes the keyboard's media keys
produce, so the 3 dB ladder and the mute memory cannot diverge between the two
routes.

### Extensions beyond SDL2

Five blitters, each because the operation it names is one a 2D game wraps SDL in
anyway, and performing it directly avoids a scratch surface and a round trip
through a generic blitter:

- **`PSDL_BlitMirrored`** — draws the source reversed. The usual SDL idiom
  allocates a surface, blits into it flipped, draws it and frees it, every time a
  sprite faces the other way. Walking the source backwards costs nothing extra.
- **`PSDL_BlitXor`** — XORs palette indices, as the DOS-era blitter modes did.
  The alternative is two scratch surfaces per call.
- **`PSDL_BlitOffset`** — adds a constant to each non-transparent index, for
  sprite sets whose sixteen colours live at an offset in the global palette.
- **`PSDL_BlitTransp`** — selects transparency per call rather than per surface.
  This is not a convenience: the usual idiom is `SDL_SetColorKey()` on the source
  before each blit, and that write is unavailable on a const surface in flash. The
  key is baked in and this selects whether to honour it.
- **`PSDL_BlitMono`** — draws every non-transparent pixel in one colour, ignoring
  the source index. Used for fonts and 1bpp art, where the alternative is
  converting the glyph to ARGB8888 first.

Also `PSDL_GlobalPalette()`; `PSDL_SetMasterVolume()` and `PSDL_GetMasterVolume()`,
a single gain applied after mixing; `PSDL_ReportMemory()`, which prints the
high-water marks of all three regions; and `PSDL_DumpArena()`, which lists the
arena entry by entry and is called automatically if the arena is exhausted.

---

## Colour depth

`PICOSDL_COLOR_DEPTH` selects what a pixel is. It differs in kind from the
hardware options below: those state what is attached, whereas this changes the
shape of the API, so a client is built for one value or the other and cannot be
indifferent to it.

| | 8 (default) | 16 |
|---|---|---|
| a pixel | a palette index | an RGB565 value |
| `SDL_Surface::format` | `SDL_PIXELFORMAT_INDEX8` | `SDL_PIXELFORMAT_RGB565` |
| palette | the CLUT, written directly | nothing to index |
| blitters, `PSDL_Blit*` | operate on indices | index operations, so not useful |
| a 320×200 frame costs | 62.5 KB | 125 KB |
| present with | `dispDrawBuffer()` | `dispDrawBuffer16()` |

The panel itself runs in RGB565 at both depths; 8bpp reaches it as RGB565 too,
expanded through the CLUT. The depth therefore changes the PIO and DMA wiring and
nothing about the ST7789.

An indexed pipeline is advantageous when the art is already indexed: the palette
provides free effects, the blitter has one format to handle, and the PIO expands
to RGB565 on the way out so the CPU never touches a pixel. A software 3D renderer
inverts every term of that. It computes shading per pixel and produces RGB565
directly, which is what the panel wants, so reaching an indexed panel would mean
quantising a whole frame to 256 colours on the CPU every frame. That costs the
smooth shading the renderer exists to produce, and costs the time twice over —
once to quantise, and again because there is no palette left to stand in for the
effects it made free.

Everything not in the table above behaves identically at both depths: input,
events, timers, audio, the status bands and the serial console. Bands are drawn
8bpp through the font blitter whatever the build, and expanded on the way out. At
16bpp that is simpler, there being no client palette for them to collide with.

The depth is fixed at build time throughout. It reaches the display driver as
`DISP_COLOR_DEPTH`, which compiles only the selected path: a different PIO
program, a different state machine count, a different DMA chain, and at 8bpp a
CLUT the other depth has no use for. `dispInit()` is passed the depth as well and
refuses if it disagrees with what the driver was built for, which turns a
misconfigured build into a refusal at start-up rather than an unrecognisable
panel. Values other than 8 and 16 are refused at configure time.

---

## Building

### Options

Four options state which hardware is present. They are independent, all default
on, and none is required:

```bash
cmake -S . -B build \
  -DPICOSDL_INPUT_BT_KEYBOARD=OFF \
  -DPICOSDL_INPUT_JOYSTICK=ON \
  -DPICOSDL_INPUT_GAMEPAD=OFF \
  -DPICOSDL_AUDIO=OFF
```

Off means absent, not ignored: the driver is not compiled. A build with all
input options off is legitimate and produces a library with a display and a
speaker and no way to press anything, which is what a kiosk or a permanent
attract-mode demo requires. `SDL_NumJoysticks()` then reports 0, so a client can
discover there is nothing to read rather than waiting on input that will never
arrive.

`PICOSDL_AUDIO=OFF` suits a board with no MAX98357A. The I2S driver and its PIO
program are not built and `SDL_OpenAudio()` fails with a clear error rather than
pretending, which is what SDL does when there is no device, so a client that
handles the desktop case already handles this one. Succeeding and never calling
the callback would be worse: a client would have no way to know, and would spend
its mixing budget on samples nothing consumes. `PSDL_SetMasterVolume()` continues
to work — it remembers a value and applies it to nothing — because it is public
API and a client should not need its own `#if` around a volume control.

Measured on `picosdl-demo`, which links this library and nothing else, for
`pico2_w` at 8bpp. With everything on it is 507,668 bytes of `.text` and 146,728
of `.bss`; each row is what turning that one option off returns:

| off | flash | RAM |
|---|---|---|
| Bluetooth | **431,344 B (421.2 KB)** | **22,220 B (21.7 KB)** |
| audio | 3,792 B (3.7 KB) | 5,252 B (5.1 KB) |
| gamepad | 2,640 B (2.6 KB) | 20 B |
| joystick | 1,792 B (1.8 KB) | 32 B |

Only one of these is worth switching off for space. Bluetooth brings in BTstack
twice over — the BLE and Classic stacks, because a keyboard may be either and
this cannot be determined from the outside — plus the CYW43 driver and that
chip's firmware blob, which is data and so cannot be reduced by any compiler
setting. With Bluetooth out, the entire demo — this library, its backend, the SDK
and the demo program — is 76,324 bytes of flash, so Bluetooth alone is roughly
five and a half times everything else in the binary. The other three options are
about having the hardware, not about size.

In a build without Bluetooth, `psdl_pico_input_status()` reports `NO KEYBOARD IN
THIS BUILD` rather than a link state. The serial console survives, stdin being
polled directly; only the four Bluetooth-specific commands are lost.

### Boards

All four Raspberry Pi Pico boards build, with no argument beyond the board name.
Figures are `picosdl-demo` at 8bpp with every option at its default:

| `PICO_BOARD` | part | Bluetooth | demo `.text` | demo `.bss` |
|---|---|---|---|---|
| `pico` | RP2040 | no radio, defaults off | 81,256 | 125,136 |
| `pico_w` | RP2040 | on | 523,720 | 147,080 |
| `pico2` | RP2350 | no radio, defaults off | 76,324 | 124,508 |
| **`pico2_w`** | RP2350 | on | 507,668 | 146,728 |

`pico2_w` is the default and the board this is developed and run on. The other
three are supported by construction rather than exercised: they build clean and
the arithmetic works out, but the panel, the DAC and the radio have not all been
brought up together on one. Nothing in the library is specific to either part —
the two PIO drivers assemble for both, and the backend uses no instruction or
peripheral the RP2040 lacks.

Bluetooth is the only option that depends on the board rather than on what is
soldered to it: it requires the CYW43 radio, so `pico` and `pico2` default it off
and the two `_w` boards default it on. Requesting it explicitly on a board
without a radio is a configure-time error rather than a silent downgrade.

The RP2040 boards are the ones short of room. Against a 256 KB main SRAM region,
`pico_w` leaves about 109 KB for the client's framebuffers, stack, heap and data
once the demo's `.bss` is placed. That is comfortable for an 8bpp client whose art
is in flash. A 16bpp client is the harder case, its 320×200 RGB565 framebuffer
alone being 125 KB. Turning Bluetooth off is what makes the space, and is by far
the largest lever available.

---

## The hardware backend

### Board definition

**Every pin number and the system clock are in `backend/pico/board.h`**, and
nothing else defines either. The ST7789 driver holds no pin numbers of its own:
the backend fills in a `struct dispPinout` from `board.h` and passes it to
`dispInit()`. Porting to a differently wired board, or changing the clock, is
confined to that one file.

`dispInit()` enforces the one rule the PIO program imposes — SCK must be CS + 1,
because the SPI state machine side-sets two bits based at CS — and refuses rather
than leaving the panel dark with nothing to diagnose.

### The clock

**125 MHz.** Below the RP2350 SDK's 150 MHz default, so the core is not
overclocked, and SCK is sysclk/2 = 62.5 MHz, exactly the ST7789 datasheet
maximum, so the panel is not either. Raising it takes the panel out of
specification by the same proportion, and a panel that tolerates that is not
obliged to.

The value is not arbitrary in the other direction either: it keeps the I2S
divider exact at 8 kHz (122.070312) and within 12 ppm at 22050 Hz. The comment in
`board.h` lists which reachable frequencies are worth wanting and what each does
to the panel, the frame time and the sample rate.

### Resource map

Three drivers share one chip, so the allocation is recorded in one place,
`backend/pico/psdl_pico.h`:

| | |
|---|---|
| PIO0 SM0, SM1 | display — 13 instructions |
| PIO0 SM2 | CYW43 radio — 6 instructions |
| PIO0 SM3 | I2S out — 8 instructions |
| DMA 0–3 | display — hardcoded, and **not** claimed by the driver |
| DMA 4, 5 | I2S data + control |
| DMA 6, 7 | CYW43 |
| DMA_IRQ_0 | I2S, handled on core 1 |
| GPIO 8–12, 15 | LCD: D/C, CS, SCK, MOSI, MISO, RESET |
| GPIO 2, 3, 4 | I2S BCLK, LRCLK, DIN |
| GPIO 27, 26, 22 | joystick X (ADC1), Y (ADC0), button (active low) |
| GPIO 6, 7 | I2C1 SDA, SCL — game controller (optional) |
| GPIO 0, 1 | UART console |

**Init order is load-bearing.** The display driver hardcodes DMA channels 0–3 and
does not claim them, while the CYW43 and I2S drivers both request any free
channel from the SDK. The video backend therefore claims 0–3 on the driver's
behalf and must run first. It panics if any of them is already taken, which turns
a silent corruption into a message.

### PIO allocation

All three drivers share PIO0: 27 of its 32 instruction slots and all four of its
state machines, leaving PIO1 and PIO2 free. The firmware reports this at boot
rather than the figures being maintained by hand — `psdl_pio_usage.c` snapshots
every block before each driver initialises and reports the difference:

```
picosdl: display driver acquired PIO0/SM0,SM1 (13 instructions, 13 total)
picosdl: I2S driver acquired PIO0/SM3 (8 instructions, 21 total)
picosdl: CYW43 driver acquired PIO0/SM2 (6 instructions, 27 total)
```

**Every driver goes through the SDK's allocator** — `pio_claim_unused_sm()` and
`pio_add_program()` — which is the only reason they can share a block. The
allocator can route a driver around what it knows is taken; a driver that writes
`instr_mem` and the state machine registers directly is invisible to it, and the
next driver to request a machine is handed one already in use.

**The radio has to be steered.** The SDK selects a block for the CYW43 bus program
by searching down from the highest instance and offers no way to request one; the
only configurable properties of that bus are its clock dividers. So
`pio_corral_for_radio()` claims every machine on every other block for just long
enough for the radio to initialise, and releases them once it has. Whether this
is wanted depends on the part: an RP2040 has two blocks for three drivers, and
without the corral the radio takes the second by itself while the display and I2S
share the first, which is the better arrangement of the two.

**The radio claims its PIO late.** `cyw43_arch_init()` claims none. The bus
program is added by `cyw43_ll_bus_init()` when the chip is powered, which under
the `threadsafe_background` architecture is an interrupt some time after
`hci_power_control()` has returned. It can therefore land inside another driver's
measurement, which is why a report subtracts what has already been attributed
rather than relying on its own before-and-after alone.

**Instruction memory is write-only.** `instr_mem` is `io_wo_32`, so a slot's
contents cannot be read back. The counts above come from the SDK's allocation
bitmap — reconstructed by asking `pio_can_add_program_at_offset()` about a
one-instruction program at each of the 32 offsets — unioned with the wrap range of
every running state machine.

---

## Footprint

Static RAM belonging to the library and its backend, measured by symbol from a
linked `pico2_w` image at 8bpp:

| | symbol | bytes |
|---|---|---|
| LIFO arena | `s_arena` | 16,384 |
| surface headers (192 × 64) | `s_headers` | 12,288 |
| status band staging | `s_band_pixels` | 6,400 |
| deferred log ring | `s_ring` | 4,096 |
| I2S DMA buffer | `s_dma_buffer` | 4,096 |
| event ring (64 entries) | `s_queue` | 3,584 |
| audio mix buffer | `s_mix_buffer` | 1,024 |
| palette | `s_colors` | 1,024 |
| ST7789 row scatter list | `bufs` | 964 |
| arena entry stack | `s_arena_stack` | 768 |
| **total** | | **50,628 (49.4 KB)** |

At 16bpp add `s_band565`, 12,800 bytes: the bands are composed 8bpp and expanded
separately, because the PIO does not expand their pixels at that depth.

No framebuffer appears in the table, the library allocating none. A client's
canvas is its own and costs it 62.5 KB at 8bpp or 125 KB at 16, once per buffer
it chooses to keep.

`PSDL_ARENA_BYTES`, `PSDL_MAX_SURFACES`, `PSDL_EVENT_QUEUE_LEN` and
`PSDL_AUDIO_BLOCK_FRAMES` are `#ifndef`-guarded in `src/psdl_internal.h`, so a
client may override any of them from its own build.

The library contributes **zero** heap. Two SDK functions allocate, both once at
init — `alarm_pool_create_on_timer_with_unused_hardware_alarm()` and
`cyw43_btbus_init()` — so a build that poisons `malloc` at link time must leave
those two alone.

---

## Tests

```bash
make -C test        # 66 checks
make -C test asan   # the same, under AddressSanitizer and UBSan
```

`src/` compiles with a plain host compiler. Everything touching hardware is behind
the functions declared in `psdl_internal.h`, and `test/host_backend.c` is a
complete implementation of them that runs on a PC.

The suite covers fills, opaque and keyed blits, clipping at every edge, the
mirrored blit, the XOR and offset blits, LIFO arena reclamation, client-owned
windows, the event ring, the palette and the timers. Clipping and the mirrored
blit carry the most weight: a mirrored blit clipped on its *left* edge must read
a different source column than an unclipped one, and an off-by-one there is
invisible until a sprite walks off the side of the screen.

These are 8bpp tests, because that is where the code they exercise lives — the
blitters, the surface regions and the palette are all the indexed half. The 16bpp
path has no host coverage; see `TODO.md`.

---

## Tooling

`picodev.sh` drives a board over SWD with a CMSIS-DAP probe, either a Raspberry Pi
Debugprobe or a second Pico running its firmware, on either part:

```bash
./picodev.sh flash [firmware.elf]   # program and reset
./picodev.sh reset                  # reset, program nothing
./picodev.sh halt                   # stop the cores and silence the audio
./picodev.sh logs                   # watch the console
./picodev.sh flash-and-logs [fw]    # program, reset, then watch from the first line
```

A probe is not required to get an image onto a board — a `.uf2` goes on over USB
with BOOTSEL held. What a probe provides is everything around programming: no
button to hold, the audio silenced first, a reset, and the console from its first
line.

**The part is determined rather than declared.** Three things differ between them:
the OpenOCD target config, the names the cores answer to (`rp2040.core0` against
`rp2350.cm0`, the RP2350 naming its by architecture because it also has RISC-V
cores), and the DMA `CHAN_ABORT` address, which differs with the channel count.
The answer comes from DPIDR, which OpenOCD reads with no target config at all, so
nothing is selected, halted or read and the detection cannot itself be performed
against the wrong guess. The revision nibble is masked before comparing, so a new
stepping of either part still places. A DPIDR it cannot place is an error naming
the value, never a default; `--rp2040` or `--rp2350` as the first argument settles
it by hand.

`halt` stops both cores and leaves them stopped; `reset` starts the board again.
Both cores matter, halting core 0 alone leaving a mixer running on core 1. So does
the order: halting does not by itself silence a board, because the audio DMA chain
re-triggers itself and keeps cycling its last two buffers into the DAC with no CPU
involved. Stopping the PIO state machines is what silences it, which every command
here does first.

`flash-and-logs` attaches the console reader *before* programming, a reader started
afterwards having already missed the start-up banner, and drains the port first so
the log does not open with output from the previous run. Only the board's output
goes to stdout, so `./picodev.sh flash-and-logs | tee boot.log` captures just that.

The script refuses to read a console another process already holds open. Two
readers on one tty do not take turns — each receives whichever bytes it wins, and
both see lines with pieces missing from the middle, which resembles corruption on
the wire closely enough to be worth failing loudly.

`PICOPOP_CONSOLE` overrides the device, which otherwise defaults to the probe's
UART bridge found under `/dev/serial/by-id` — stable, unlike `ttyACMn`, which moves
when anything else is plugged in. `PICOPOP_BAUD`, `OPENOCD` and `PROBE_SERIAL`
override the rest.

---

## Implementation notes

Constraints that are not evident from the code and are expensive to rediscover.

**The RP2350 pad isolation latch.** Pads come up isolated. A raw FUNCSEL write
leaves the latch set and the pin never drives, which presents as dead hardware.
Going through `gpio_set_function()` clears it.

**BTstack must not run its own loop.** Under `pico_cyw43_arch_threadsafe_background`
the run loop is driven by interrupts off the async_context, so
`btstack_run_loop_execute()` parks for ever. The usual keyboard-host example calls
it because it has nothing else to do; here the client owns the main loop, so it is
never called and BTstack progresses on its own.

**Audio runs on core 1, and everything on that path is `__not_in_flash_func`.**
BTstack persists pairings to flash, and a flash erase stops XIP: any code fetched
from flash during that window hard-faults. Core 1 is locked out while it happens,
but an interrupt at a higher priority than the lockout's spin could still preempt
it, so the handler must not require flash to run. Audio glitches for the duration
of a pairing write, which is the correct trade.

**`PioI2S_init()` is called from core 1.** `irq_set_exclusive_handler()` and
`irq_set_enabled()` act on the calling core's NVIC, so calling them from core 0
would leave the DMA interrupt firing on the wrong core and the mixer never
running.

**`SDL_LockAudio()` is a recursive cross-core lock.** It must hold off a mixer on
the other core, so it is a hardware spinlock, and those are not recursive.
`psdl_audio_render()` holds it across the client callback, so a callback that
itself called `SDL_LockAudio()` would take the same spinlock twice on one core and
hang. Tracking the owning core makes the second acquisition a counter bump.

**Timers fire from `SDL_PumpEvents()`, not from an alarm interrupt.** A timer
callback that pushed an event would otherwise give the event ring a genuine
interrupt producer and force everything downstream to be interrupt-safe, for no
benefit to a client that polls continuously.

**The audio DMA sustains itself.** The data channel chains to a control channel
holding a two-entry address ring, so finishing one buffer re-triggers the other
indefinitely and the output never glitches. The consequence is that halting the
cores does not stop the sound: a debugger halt leaves the last two buffers cycling
into the DAC.

**The I2S divider tolerance.** Upstream pio-i2s panics unless the requested
divider lands exactly on a multiple of 1/256. The divider's eight fractional bits
are a rounding target rather than a constraint, and requiring exactness rejects
almost every rate this library exists to produce: at 32-bit stereo and 125 MHz,
11025, 22050 and 44100 Hz are all fractional, and only 8000 Hz divides exactly.
22050 Hz needs 44.288549, which rounds to a real rate of 22049.744 Hz — an error
of 11.6 ppm, two orders of magnitude below the smallest audible pitch difference.
The copy here measures what the rounding costs and compares it against a
configurable tolerance, `PioI2S_MAX_CLOCK_ERROR_PPM`. It is the only divergence
from upstream in that submodule.

---

## Source layout

```
include/SDL2/SDL.h    the API: types, constants, and the PSDL_ extensions
src/                  portable - no hardware, only the backend interface
  psdl_surface.c        the three regions and the LIFO arena
  psdl_blit.c           the blitters (the only hot path)
  psdl_palette.c        the global CLUT
  psdl_video.c          window and present
  psdl_events.c         event ring and key state
  psdl_timer.c          timers, serviced from SDL_PumpEvents
  psdl_audio.c          SDL's pull-callback model over the backend
  psdl_rwops.c          memory RWops; file RWops fails cleanly
  psdl_stubs.c          joystick, and everything meaningless here
  psdl_gamecontroller.c SDL_GameController, fed by whichever backend has a pad
backend/pico/         the hardware half
  board.h               every pin number and the system clock
  psdl_pico_video.c     the ST7789 backend
  psdl_pico_audio.c     the I2S backend and the core 1 mixer
  psdl_pico_input.c     keyboard, joystick, pad and the serial console
  psdl_pio_usage.c      what each driver takes from PIO
  bt/                   Bluetooth HID keyboard
  seesaw_gamepad.c      Adafruit Gamepad QT over I2C
pio-st7789/           the ST7789 PIO driver, a submodule
pio-i2s/              the I2S output driver, a submodule
demo/                 picosdl-demo
test/                 host tests: builds with cc, runs on a PC
cmake/                the Pico SDK bootstrap, shared with embedders
```

---

## Status

Running on a Pico 2 W with the panel, the DAC, the radio and the stick live at
once. The 16bpp path runs on the same board driven by a software 3D renderer at
320×200. Known gaps are recorded in `TODO.md`.

---

## Licence and provenance

**BSD-2-Clause** — see [`LICENSE`](LICENSE).

Permissive deliberately. This library contains no code from the program it was
written for: it is an independent implementation of the slice of SDL2 that
program uses, and everything it depends on is permissive too. Being built
alongside a GPL program does not make a library derived from it, and a copyleft
licence here would contradict the one property the design keeps insisting on —
that picosdl is reusable by anything, including firmware that is not open source.

Parts originating elsewhere keep their own terms, and `LICENSE` lists them:
`backend/pico/bt` carries BSD-3-Clause material from BTstack and pico-examples,
the 9×14 font is derived from a public-domain X11 font, and `pio-st7789` and
`pio-i2s` are submodules with their own licence files.
