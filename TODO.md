# TODO

Known gaps in picosdl, roughly in the order they get in the way.

## No example program

The library has no demo of its own. There was one, but it played Prince of
Persia's Adlib tunes and blitted its sprites, so it belonged to the game rather
than to the library and moved out with it.

A library that cannot be run without supplying a game is hard to evaluate. What
is wanted is something self-contained: a palette animation, a blit of a sprite
generated at build time, a synthesised tone, a key and a stick reading — enough
to prove a board is wired correctly and nothing more.

## No host presentation

`test/host_backend.c` implements the backend interface and captures what would
have been pushed to the panel, which is enough for the unit tests, but nothing
displays it. Giving it a real window is a change to that one file.

Until then, seeing what the library actually draws means either a board or a
harness of your own that writes the captured frame to disk.

## `SDL_RWFromFile` always fails

`psdl_rwops.c` implements memory RWops and fails cleanly on files. There is no
filesystem: saved games, configuration and high scores all need somewhere to
live, and on this hardware that means a small key-value store in flash.

Whatever writes it has to go through `flash_safe_execute` with the second core
locked out, because a flash erase stops XIP and any code fetched from flash
during that window hard-faults. The audio path is already `__not_in_flash_func`
for the same reason, so the machinery is half there.

## The Bluetooth pairing does not survive a reboot

After a fresh pair the keyboard works and the stored address is right in RAM,
but on the next boot nothing is found and the search starts again. BTstack
persists bonds through `btstack_tlv_flash_bank`, which erases and programs
flash, which requires core 1 to be locked out — and core 1 is the mixer.

picosdl does its half: `multicore_lockout_victim_init()` is called and the whole
audio path avoids flash. What has not been established is whether the write
reaches flash at all, fails silently, or is refused. Not yet diagnosed, and the
same mechanism will govern the key-value store above.

## `SDL_UpdateWindowSurfaceRects` is not implemented

Only `SDL_UpdateWindowSurface` exists, so every present pushes the whole canvas.
That is the largest thing left on this list for anyone who cares about frame rate,
and it is a gap rather than a design decision - the layer underneath already has
what it needs.

`psdl_backend_video_present()` takes `(pixels, w, h, pitch)` with no rectangle,
but `dispDrawBuffer()` in `vendor/pio-st7789` is already
`(framebuffer, size, const struct Rect *rect, stride)`. The panel link is the
bottleneck, not the CPU: the ST7789 program shifts 2 cycles per bit at 16bpp, so a
320x200 frame is 64000 x 32 cycles, or **16 ms at 128 MHz** - against a measured
22 ms frame in the client that drives it hardest. Most of that is spent resending
pixels that did not change.

The work is to add the rectangle to the backend interface, implement the SDL2
entry point on top of it, and pass the rects through. Games that track dirty
rectangles already compute exactly the right ones; a game that does not can keep
calling `SDL_UpdateWindowSurface` and see no change.

## The arena and surface-table sizes are one client's measurement

`PSDL_ARENA_BYTES` is 28 KB and `PSDL_MAX_SURFACES` is 192 because both were
comfortably more than anything observed, not because either is the measured
high-water mark of a workload. One client has now been measured properly, over
400,000 presents covering its title screen, its attract demo and gameplay:

| | limit | peak | |
|---|---|---|---|
| `PSDL_ARENA_BYTES` | 28,672 | 6,700 | 23% |
| `PSDL_MAX_SURFACES` | 192 | 112 | 58% |

So the arena looks about four times larger than it needs to be. Both figures are
one client's, on paths that run start to finish - that client's modal dialogs and
its name-entry screen, which holds a surface across a wait for the user, are not
in the measurement. Instrument the paths that matter to *you* before trusting
either number.

Note what "the peak" does *not* tell you. The arena is LIFO, so one long-lived
allocation near the bottom pins everything above it whether or not those are
still in use, and the total then reads as healthy demand rather than the dead
weight it is. The same client exhausted the arena twice from opposite causes - a
16.5 KB surface allocated at start-up that nothing ever read, and then a leak of
small ones - and in both cases `PSDL_ReportMemory()`'s total looked like ordinary
use. **Neither was a sizing problem.** Raising `PSDL_ARENA_BYTES` would have
postponed both and diagnosed neither, which is the trap a fixed budget sets: the
message names the allocation that happened to be last, never the one at fault.

`PSDL_DumpArena()` is the answer to that. It prints every entry with its base,
size, dimensions and whether it is dead - dead meaning freed but not yet
reclaimable, which only happens when a caller released out of stack order - and
`SDL_CreateRGBSurface` calls it itself on exhaustion, so the failure names its own
cause. Reach for it before reaching for a bigger number.

`PSDL_MAX_SURFACES` fails more kindly: too small is a panic at start-up, which is
at least loud and immediate.

## Game controller and haptics are inert stubs

`SDL_IsGameController()` returns false and every `SDL_GameController*` and
`SDL_Haptic*` entry point does nothing. They exist so that a game written
against SDL2 compiles and links unmodified, and an analog stick reaches it
through the joystick calls instead.

Mapping the stick onto the controller API would let a game use either, at the
cost of a mapping table the library currently does without.

## One backend, one board

`backend/pico` assumes this hardware: an ST7789 panel on PIO0, I2S on PIO1, a
CYW43 radio, and an ADC stick. The split between `src/` and `backend/` is real
- `src/` compiles with a plain host compiler and the unit tests prove it - but
nothing has yet been ported to a second board, so the interface has only been
tested against the shape of the first one.
