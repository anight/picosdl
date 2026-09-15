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

## Lock-key LEDs are not updated

`backend/pico/bt/bt_app.c`'s LED handler is static, and picosdl takes over the
keyboard decoder's callbacks after `bt_app_setup()`. Caps Lock and Num Lock
therefore never light. Cosmetic, and two lines to restore.

## The arena size is a guess

`PSDL_ARENA_BYTES` is 28 KB because that was comfortably more than anything
observed, not because 28 KB is the measured high-water mark of any particular
workload. A client should instrument a representative run - `PSDL_ReportMemory()`
prints the peak - and size it from that.

The same applies to `PSDL_MAX_SURFACES`. It is 192 because one client needed
about 105; the failure mode when it is too small is a panic at start-up, which
is at least loud.

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
