# picosdl

A minimal SDL2 work-alike for the Raspberry Pi Pico, plus a demo that exercises
display, keyboard, joystick and sound together.

This is step one of the port described in [../PLAN.md](../PLAN.md). There is no
game code here yet. What it establishes — **now confirmed on a board, not just in
a build** — is that a program written against the SDL2 API runs on a Pico, and
that the panel, the radio and the DAC coexist on one chip.

Running on a Pico W: 43–45 fps, the game's title theme playing from boot at
38–41% of one core, a Bluetooth keyboard typing into it, and the analog stick
moving a sprite around.

## What it is

Two design decisions make it small enough to be worth having rather than
linking real SDL:

**One pixel format.** Everything is 8bpp indexed against a single global
256-entry palette. No format negotiation, no conversion, no generic blitter.
This is not a compromise imposed on the game — it is what the hardware already
does. The ST7789 driver takes a palettised buffer and a CLUT and expands indices
to RGB565 in the PIO/DMA chain, so the CPU never touches a pixel. That is VGA
mode 13h, which is exactly what Prince of Persia is.

The consequence worth naming: **the palette is the display hardware**.
`SDL_SetPaletteColors` writes the CLUT directly, so a palette fade costs 256
register writes instead of touching 64000 pixels. `USE_FADE` and `USE_FLASH`
become affordable on this target because of it.

**Nothing allocates.** Surfaces come from three fixed places and the surface
flags record which, so `SDL_FreeSurface` dispatches without a general allocator
underneath:

| region | what | freeing |
|---|---|---|
| `PSDL_SURF_EXTERN` | pixels owned by the caller — `const` sprite data in XIP | no-op |
| `PSDL_SURF_STATIC` | the screen-buffer pool, 320×200 each | returns a slot |
| `PSDL_SURF_ARENA` | a LIFO bump arena | pops the stack |

Orthogonally, `PSDL_SURF_CONST` marks a surface whose **object** is also in
flash, as the game's 741 generated sprites are. Those cannot be written to at
all — a refcount decrement, a lock counter or a colour-key change is a write to
XIP and faults — so every mutating entry point checks first. It is the single
most important invariant in `psdl_surface.c`.

The arena exists for peels. SDLPoP's `restore_peels()` walks its table
backwards, so peels are released in strict stack order and a bump pointer is all
the bookkeeping needed — fragmentation is structurally impossible. Out-of-order
frees are tolerated but do not reclaim until the top is dead, which keeps the
invariant simple and makes a non-LIFO caller show up as arena pressure rather
than corruption.

Full-screen surfaces get their own pool rather than the arena: SDLPoP asks for
two of them (the window surface and the offscreen buffer) and at 62.5 KB each
they would otherwise force the arena to be sized for them and waste it the rest
of the time.

**Scancodes need no translation.** `SDL_Scancode` values *are* USB HID usage
codes for every key a game cares about. The Bluetooth stack hands over a usage;
that usage is the scancode; it indexes the key state array directly.

## Layout

```
include/SDL2/SDL.h      the API - types, constants, and the PSDL_ extensions
src/                    portable: no hardware, only the backend interface
  psdl_surface.c          the three regions and the LIFO arena
  psdl_blit.c             the blitters (the only hot path)
  psdl_palette.c          the global CLUT
  psdl_video.c            window and present
  psdl_events.c           event ring and key state
  psdl_timer.c            timers, serviced from SDL_PumpEvents
  psdl_audio.c            SDL's pull-callback model over the backend
  psdl_rwops.c            memory RWops; file RWops fails cleanly (no FS yet)
  psdl_stubs.c            joystick, and everything meaningless on a Pico
backend/pico/           the hardware half, plus the vendored I2S and joystick
test/                   host tests: builds with cc, runs on a PC
demo/                   the audio-visual test program
```

### Extensions beyond SDL

Five blitters that exist because the operations they name are the ones SDLPoP
wraps SDL in anyway, and doing them directly avoids a scratch surface:

- `PSDL_BlitMirrored` replaces SDLPoP's `hflip()`, which allocated a whole
  surface and blitted into it **every time a sprite was drawn facing left** —
  dozens of allocate/blit/free cycles per frame. Walking the source backwards
  costs nothing extra.
- `PSDL_BlitXor` replaces `blit_xor()`, which built *two* scratch surfaces per
  call to XOR two images. XORing palette indices is what the DOS original's
  blitter modes did anyway.
- `PSDL_BlitOffset` blits with a constant added to each non-transparent index,
  for sprite sets whose 16 colours live at an offset in the global palette.
  (Build-time-converted sprites have the offset baked in and do not need it.)
- `PSDL_BlitTransp` chooses transparency per call instead of per surface.
  SDLPoP toggles it by calling `SDL_SetColorKey` on the sprite before every blit
  (`method_1_blit_rect`); that write is not available on a const flash sprite,
  so the key is baked in and this picks whether to honour it.
- `PSDL_BlitMono` draws every non-transparent pixel as one colour, ignoring the
  source index — SDLPoP's `method_3_blit_mono`, which built a whole ARGB8888
  conversion of the sprite to achieve it.

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Produces `build/picosdl-demo.uf2`. The sibling `pico-test-st7789` and
`pico-test-bt-keyboard` trees are referenced in place, not copied, so fixes
there do not have to be mirrored.

### Host tests

The whole of `src/` is portable — hardware lives behind the backend interface —
so the blitters, the arena and the event ring are testable with a plain
compiler:

```bash
make -C test
```

```bash
make -C test asan
```

56 checks, covering the places bugs actually live: clipping at every edge, the
mirrored blit clipped on its left edge (which has to read a *different* source
column than an unclipped one — an off-by-one there is invisible until a sprite
walks off the side of the screen), LIFO reclamation, screen-pool exhaustion, and
event-ring overflow. `test/host_backend.c` is also the seed of the host backend
PLAN.md phase 3 calls for; giving it a real SDL2 window is a change to that one
file.

## Board and clock

**`pico_w` (RP2040) at 128 MHz.** PLAN.md argues for the Pico 2 W on memory
grounds and that is still where this is heading — but two of the three drivers
here were only ever characterised on an RP2040 at 128 MHz, and the display
driver pokes PIO registers directly rather than going through the SDK. Getting
the integration right on the board the peripherals are known to work on comes
first. The RP2350 move is its own task with its own gotchas (PIO instruction
encodings, the GPIO isolation latch).

128 MHz is not arbitrary: it makes the I2S divider exactly 125.0 at 8 kHz and it
is the clock the ST7789 PIO timings were measured at. `set_sys_clock_khz()` must
be called **before** `stdio_init_all()` — it re-parents `clk_peri` off `clk_sys`,
and stdio derives the UART divisor from `clk_peri` when it starts.

## Resource map

Three peripherals share one chip, and two of the drivers were written without
knowing about the other. Keeping the allocation in one place (`psdl_pico.h`) is
the only way this stays debuggable.

| | |
|---|---|
| PIO0 SM0, SM1 | display — hardcoded in `dispPioSt7789.c` |
| PIO1 SM0 | I2S out (the example defaulted to pio0; moved) |
| PIO0 SM2 | CYW43 radio — the SDK claims whatever is free |
| DMA 0–3 | display — hardcoded, and **not** claimed by the driver |
| DMA 4, 5 | I2S data + control |
| DMA 6, 7 | CYW43 |
| DMA_IRQ_0 | I2S, handled on core 1 |
| GPIO 8–12, 15 | LCD | 
| GPIO 2, 3, 4 | I2S BCLK, LRCLK, DIN |
| GPIO 26, 27, 22 | joystick X, Y, button |
| GPIO 0, 1 | UART console |

**Init order is load-bearing.** The display driver hardcodes DMA channels 0–3
and does not claim them; the CYW43 and I2S drivers both ask the SDK for "any
free channel". So the video backend claims 0–3 on the driver's behalf, and must
run first. It panics if any of them is already taken, which turns a silent
corruption into a message.

## Things that had to be got right

**BTstack must not run its own loop.** With `pico_cyw43_arch_threadsafe_background`
the run loop is driven by interrupts off the async_context, so
`btstack_run_loop_execute()` parks forever. The BT example calls it because a
keyboard host has nothing else to do; here the game owns the main loop, so we
deliberately never call it and BTstack progresses on its own.

**The I2S divider check was wrong.** `PicoI2S_verifyPIOClockDivision()` rejected
any ratio whose fractional part was not a multiple of 1/16, on the grounds that
a PIO clock divider has "16 fractional bits". It has eight — the divider is 16.8
fixed point — so the representable step is 1/256. 8 kHz at 128 MHz divides
exactly, so nothing noticed. 22050 Hz needs 45.3514, which the old check
panicked on; rounded to the nearest 1/256 it gives 22049.95 Hz, an error of
0.0002%. The vendored copy now checks the range the hardware can express and
warns on the resulting error rather than on whether it is zero.

**Audio runs on core 1, and everything on that path is `__not_in_flash_func`.**
BTstack persists pairings to flash, and a flash erase stops XIP — any code
fetched from flash during that window hard-faults. Core 1 is locked out while it
happens (it calls `multicore_lockout_victim_init()`), but an interrupt at a
higher priority than the lockout's could still preempt the spin, so the handler
must not need flash to run. Audio glitches for the duration of a pairing write;
that is the correct trade.

`PioI2S_init` is called **from core 1** for a related reason:
`irq_set_exclusive_handler` and `irq_set_enabled` act on the calling core's
NVIC, so calling them from core 0 would leave the DMA interrupt firing on the
wrong core and the mixer never running.

**`SDL_LockAudio` is a recursive cross-core lock.** It has to hold off a mixer on
the other core, so it is a hardware spinlock — and an RP2040 spinlock is not
recursive. `psdl_audio_render` holds it across the client callback, so a
callback that itself called `SDL_LockAudio` would take the same spinlock twice
on one core and hang. Tracking the owning core makes the second acquisition a
counter bump.

**Timers fire from `SDL_PumpEvents`, not from an alarm interrupt.** SDLPoP's
timer callback pushes an `SDL_USEREVENT`; running that from an IRQ would give
the event ring a genuine interrupt producer and force everything downstream to
be interrupt-safe for no benefit. The game polls continuously, so servicing
timers there is close enough.

## The demo

| input | effect |
|---|---|
| stick | moves the sprite; it faces the way it is going |
| stick button | plucks a low note |
| any key | plucks a note, pitch following the scancode |
| `1`–`4` | plays one of the game's Adlib tunes; `1` is the title theme, which also starts by itself at boot |
| `0` | stops the music |
| `-` / `=` | master volume down / up (starts at 25%) |
| `T` | steady 440 Hz test tone, replacing everything else |
| space | draws the sprite with an XOR blit |
| `F` | fades the palette out and back, by CLUT writes alone |
| `M` | prints the memory report to the console |
| `Esc` | stops |

The background bands are drawn with **fixed** indices every frame; the motion
comes entirely from rotating the CLUT. That is the palette-animation path the
game's fades will use, shown working.

Console on both UART0 (GP0/GP1, what a debugprobe bridges) and USB CDC at
115200. Put the keyboard in pairing mode and wait — the firmware alternates
between LE and Classic searches. If it asks for a passkey, the number appears on
the console and is typed **on the Bluetooth keyboard**.

Typed *into the serial terminal*, not on the Bluetooth keyboard:

| key | effect |
|---|---|
| `s` | link status |
| `r` | forget the pairing and search again |
| `n` | search again, keeping the pairing |
| `?` | help |

Bluetooth is the one subsystem here whose failures nothing on screen explains,
and without these the only way to look is a debugger — which is exactly how the
two bugs below were found.

**Known problem: the pairing does not survive a reboot**, so the keyboard has to
be re-paired each time. BTstack writes bonds to flash, which needs core 1 locked
out while it happens, and core 1 is the audio mixer. Not yet diagnosed; see
PLAN.md §8. The demo starts the title theme by itself partly for this reason —
the audio path demonstrates itself without a keyboard.

Lock-key LEDs are not updated: `bt_app.c`'s LED handler is static, and picosdl
takes over the decoder's callbacks after `bt_app_setup()`. Cosmetic, and a
two-line change in `bt_app.c` when that gets folded into the game.

## Measured

```
text 1715168   bss 224344     picosdl-demo.elf, -DCMAKE_BUILD_TYPE=Release
```

*On the board*: 43–45 fps sustained, mixer load 38–41% average and 43–64% peak
with the music playing and Bluetooth connected.

Flash: 1675 KB of the 2 MB board, including the 1138 KB of game resources and
the 234 KB CYW43 firmware blob. BSS is 219 KB of the RP2040's 264 KB, leaving
about 45 KB, of which 20 KB is currently the heap `parse_midi` uses:

| | |
|---|---|
| screen pool (2 × 320×200) | 128.0 KB |
| LIFO arena | 28.0 KB |
| heap (`parse_midi` only) | 20.0 KB |
| BTstack + CYW43 state | ~20 KB |
| DBOPL wave/mul tables | 8.8 KB |
| surface headers (96) | 6.4 KB |
| DBOPL chip state | 4.3 KB |
| I2S DMA double buffer | 4.0 KB |
| event ring (64) | 3.5 KB |

The game's own globals are roughly 60 KB on top of this, and there is nothing
like that left. Three levers recover 91 KB between them — one screen buffer
instead of two (62.5 KB), pre-parsing the MIDI at build time (20 KB, and
required on this board anyway), and precomputing DBOPL's wave tables into flash
(8.8 KB) — which would make a Pico W work. See PLAN.md §7. **RAM, not CPU, is
now the binding constraint**: the CPU argument for the Pico 2 W went away when
the emulator changed.

**Heap: picosdl contributes zero, and almost nothing else does either.**
Disassembly shows three heap users in the whole firmware. Two are in the SDK and
one-shot at init — `alarm_pool_create_on_timer_with_unused_hardware_alarm` and
`cyw43_btbus_init`. The third is SDLPoP's `parse_midi()`, which is why
`PICO_HEAP_SIZE` is set at all; pre-parsing the tunes at build time removes it.
Nothing in picosdl allocates. The link-time poisoning described in PLAN.md §3.4
will have to leave those three alone.

Of the arena, the demo peaks at 6280 bytes — but that is the demo's text
rendering, not the game's peels. Sizing the arena properly still needs the
instrumented desktop run PLAN.md phase 2 calls for.

## The game's music

The demo plays Prince of Persia's own Adlib music through **SDLPoP's `midi.c`,
unmodified**, over the tunes and the instrument bank sitting in flash.
`demo/game_glue.c` supplies the ~40 lines of SDLPoP the player needs — a resource
lookup and six globals — and nothing else.

Whether an OPL emulator would fit in core 1's budget was the largest open
question in the plan. **It is now answered on hardware: 38–41% average, 43–64%
peak.** The demo still displays mixer load — the fraction of each audio block's
budget the callback used, turning red past 75% — because that headroom has to
survive the game being added on top.

The same path runs on the host, which is how this was all measured before any
hardware:

```bash
make -C test music
```

renders 30 seconds of the title theme to a WAV and reports level, silent-block
count and synthesis time. `make -C test music ID=10056` picks another tune.

**It uses DBOPL, DOSBox's emulator, not the Nuked one SDLPoP ships.** Measured on
the title theme, DBOPL costs **938 instructions per output sample against Nuked's
5583** after optimisation — about 6x cheaper, or ~21M instructions/second instead
of ~123M — and its chip state is 4380 bytes against Nuked's 20776. It is the less
exact model; judged by ear and accepted. Level-matched to 0.06 dB with a 0.9995
loudness-envelope correlation, about 1.3 dB brighter above 6 kHz.

`PICOPOP_OPL` switches: `-DPICOPOP_OPL=nuked` builds Nuked 1.7.4 instead, with
two optimisations of ours — silent-slot skipping (38% cheaper, 73 dB down, not
bit-exact) and an unfolded log-sine table (bit-exact, 15 Thumb instructions to
7). `make -C test opl3` enforces the fidelity claim; `make -C test music
OPL=nuked` renders the comparison. Do not reach for upstream Nuked v1.8 — it is
40% *more* expensive.

DBOPL is vendored in `SDLPoP/src/dbopl.{cpp,h}`, trimmed to the emulator core
(DOSBox's mixer glue, port decoding and save states removed, which is what let
its `dosbox.h`/`adlib.h` dependencies go), with `dbopl_adapter.cpp` presenting
it through Nuked's API so `midi.c` is unmodified. GPLv2-or-later, like the Nuked
code already here.

**Two bugs this shook out on first power-up**, both worth remembering:

- **`malloc` in the audio callback.** `midi.c` allocated its OPL scratch buffer
  per block *inside the I2S DMA interrupt on core 1*, and used the result with
  no NULL check. On an RP2040 address 0 is ROM: the writes are dropped and the
  mix loop then reads ROM contents as full-scale samples. That was a loud noise
  instead of music. It is a static buffer now, with an overflow counter the demo
  displays.
- **A zero identity address.** `bt_app.c` overwrote a working keyboard address
  with the all-zero "identity" BTstack reports for a device advertising a
  *static random* address — which has no identity to resolve. Every reconnect
  then failed with `gap_connect refused`. Pre-existing in
  `pico-test-bt-keyboard`, not caused by this port.

One consequence worth knowing: `parse_midi()` is the **only** thing in this
firmware that uses the heap, and with the screen buffers, the arena and BTstack
in place there are just 30 KB left for one. `PICO_HEAP_SIZE` is set to 20 KB;
asking for 32 KB overflows RAM at link time. The title theme parses into ~7 KB,
so it fits, but the bigger tunes will not — pre-parsing them at build time is on
the plan, and this is why.

## Game resources

The whole of the game's sprite data is converted at build time and lives in
flash — see [`../PR/src/bin/README.md`](../PR/src/bin/README.md) for the pipeline
and the palette layout. picosdl consumes it as `const SDL_Surface` objects it
never copies, decodes or frees.

```
707 sprites, 741 surfaces, 20 sprite sets
1138 KB .rodata, 0 bytes .data
```

Checked end to end by `make -C test verify`: every sprite blitted from its const
flash surface, mapped back through the global palette, and compared pixel for
pixel against the original BMPs rendered by a separate path. 741 sprites, 0
mismatched.

## Not done yet

- No host presentation backend — `test/host_backend.c` captures frames but does
  not display them.
- `SDL_RWFromFile` always fails; saved games need a flash key-value store.
- The demo does not draw the converted game sprites yet; it has its own.
- MIDI tunes are parsed at runtime into the heap. The title theme fits in 7 KB;
  the larger ones will not fit the 20 KB available.
- DBOPL builds its wave and multiplication tables into 8.8 KB of `.bss` at
  startup. Precomputing them at build time would move that to flash and drop
  `sin`/`pow` from the firmware — worth doing when RAM gets tight.
- The built-in font still decodes at runtime (it is compiled into `seg009.c`,
  not a DAT resource, and is drawn mono rather than through a palette row).
- The Bluetooth pairing does not survive a reboot; see above and PLAN.md §8.
