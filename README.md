# picosdl

A small subset of SDL2 for the RP2350, with a real backend: an ST7789 panel, an
I2S DAC, a Bluetooth HID keyboard and an analog stick.

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

### The volume keys do not reach the game

Volume up, volume down and mute are handled by the library and consumed: no
event is queued and `SDL_GetKeyboardState()` never shows them held. They step the
master volume on a 3 dB ladder, and mute toggles, remembering where it was.

This is a deliberate departure from SDL2, where a game does see them - but on a
desktop it never has to care, because the window manager takes the media keys
first. There is no window manager here, so the library stands in for it. Without
that, every game would have to implement volume control or have those keys do
nothing.

## Layout

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
backend/pico/         the hardware half
  bt/                   Bluetooth HID keyboard
vendor/pio-st7789/    the ST7789 PIO driver, a submodule
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

## The hardware

**`pico2_w` (RP2350) at 128 MHz.** The clock is not arbitrary: it makes the I2S
divider exactly 125.0 at 8 kHz, and it is the clock the ST7789 PIO timings were
measured at. Override the board with `-DPICO_BOARD=pico_w`; that part still
builds, but it is short of RAM and flash.

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

**The I2S divider check was wrong** in the driver this vendored. It rejected any
ratio whose fractional part was not a multiple of 1/16, on the grounds that a
PIO divider has "16 fractional bits". It has eight — the divider is 16.8 fixed
point — so the step is 1/256. 8 kHz at 128 MHz divides exactly, so nothing had
noticed. The vendored copy checks the range the hardware can express and warns
on the resulting error rather than on whether it is zero.

## Footprint

Static RAM, measured from a linked image:

| | |
|---|---|
| screen pool (2 × 320×200) | 128000 |
| LIFO arena | 28672 |
| surface headers (192) | 13056 |
| event ring (64) | 3584 |
| palette | 1024 |
| audio mix buffer | 1024 |
| **total** | **170752 (166.8 KB)** |

All of it is tunable: `PSDL_SCREEN_BUFFERS`, `PSDL_ARENA_BYTES`,
`PSDL_MAX_SURFACES`, `PSDL_EVENT_QUEUE_LEN` and `PSDL_AUDIO_BLOCK_FRAMES` are
`#ifndef`-guarded in `src/psdl_internal.h`, so a client can override any of them
from its own build. The screen pool dominates, and a client that composites
directly into the framebuffer rather than into an offscreen buffer can halve it.

The library contributes **zero** heap. Two SDK functions do allocate, both
one-shot at init — `alarm_pool_create_on_timer_with_unused_hardware_alarm` and
`cyw43_btbus_init` — so a build that poisons `malloc` at link time has to leave
those two alone.

## Status

Running on a Pico 2 W with the panel, the DAC, the radio and the stick all live
at once. Known gaps are in `TODO.md`.

## Licence and provenance

The library is GPLv2-or-later, matching the code it was extracted alongside.

`vendor/pio-st7789` is a separate repository, included as a submodule.
`backend/pico/bt` and `backend/pico/pio-i2s.*` are vendored from the author's own
earlier bring-up projects and carry their original terms.
