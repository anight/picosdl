# TODO

Known gaps in picosdl, roughly in the order they get in the way.

## No host presentation

`test/host_backend.c` implements the backend interface and captures what would
have been pushed to the panel, which is enough for the unit tests, but nothing
displays it. Giving it a real window is a change to that one file.

Until then, seeing what the library actually draws means either a board or a
harness of your own that writes the captured frame to disk.

## The 16bpp path has no host coverage

`test/host_backend.c` implements `psdl_backend_video_present_rgb565()` as a
counter and nothing else, so a host build at `PSDL_COLOR_DEPTH=16` links and the
existing tests still run - but none of them exercises that depth, and the ones
that exist could not: they test the blitters, the surface regions and the
palette, which is the indexed half and is exactly what 16bpp removes.

What is missing is small but real. The depth's whole surface is two functions and
a rectangle push, and the parts worth asserting are the parts with arithmetic in
them: that `pitch` is honoured in pixels rather than bytes, that a frame whose
height is not the canvas height is still centred in the letterbox, and that
`PSDL_PresentSync()` after `PSDL_PresentBuffer()` orders correctly against the
next present. The host backend can capture an RGB565 frame as easily as it
captures an indexed one.

Below that, `dispDrawBuffer16()` has no coverage at all on either host or board
beyond the fact that a picture appears. Its clipping and its row-scatter path
(source stride wider than the rectangle) are the same shapes that needed tests in
the 8bpp blitter, and for the same reason.

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

## `PSDL_MAX_SURFACES` is one client's measurement

The arena is now sized from a measurement (see `PSDL_ARENA_BYTES` in
`src/psdl_internal.h`). `PSDL_MAX_SURFACES` is the other fixed budget and has not
had the same treatment: 192 headers, against a peak of 112 in the only client
that has ever run, on the paths that run without a human. It fails kindly - too
small is a panic at start-up, loud and immediate - so it is left alone rather
than tuned on one data point.

## The controller path has no host coverage

`SDL_GameController` is real now - `psdl_gamecontroller.c`, fed by whichever
backend has a pad - but nothing exercises it except a board and a thumb. The host
backend never calls the three feeders, so `SDL_IsGameController()` is false there
and every controller branch is unreachable in the one build that has tests.

That is not a theoretical gap. Bringing up the first pad produced six bugs and
every one of them was found by pressing a button on hardware: a presence probe that
used a read where the device wanted a write, an axis wired backwards, button
bindings in the wrong places, a restart guard that also fired at the title screen,
and a pause that could not be undone because the only key the pad could send was
the one that re-paused.

What is wanted is scripted pad input in the host harness, beside the `PICOPOP_KEYS`
it already has - `PICOPOP_PAD=start@1500,x@1600` - so that "after a death, Start
restarts the level" can be asserted before flashing. The feeders exist and take
plain values, so this is a change to one file.

## Haptics are inert stubs

Every `SDL_Haptic*` entry point does nothing. They exist so a game written against
SDL2 links unmodified. There is nothing on this hardware to shake, and
`SDL_GameControllerRumble()` returns -1 for the same reason - which is what SDL
itself returns when a device cannot do it.

## One backend, one board

`backend/pico` assumes this hardware: an ST7789 panel on PIO0, I2S on PIO1, a
CYW43 radio, and an ADC stick. The split between `src/` and `backend/` is real
- `src/` compiles with a plain host compiler and the unit tests prove it - but
nothing has yet been ported to a second board, so the interface has only been
tested against the shape of the first one.
