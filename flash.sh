#! /bin/bash
#
# Flash a firmware image over SWD.
#
#   ./flash.sh [firmware.elf]
#
set -e

FIRMWARE="${1:-./build/picosdl-demo.elf}"

if [ ! -f "$FIRMWARE" ]; then
	echo "usage: $0 [firmware.elf]" >&2
	echo "no such firmware file: $FIRMWARE" >&2
	exit 1
fi

# Distro OpenOCD 0.12.0 has no RP2350 target. Prefer the Pico SDK's build.
SDK_OPENOCD="${HOME}/.pico-sdk/openocd/0.12.0+dev/openocd"
SDK_SCRIPTS="${HOME}/.pico-sdk/openocd/0.12.0+dev/scripts"

if [ -x "$SDK_OPENOCD" ]; then
	OPENOCD="$SDK_OPENOCD"
	OPENOCD_ARGS=(-s "$SDK_SCRIPTS")
else
	OPENOCD="${OPENOCD:-openocd}"
	OPENOCD_ARGS=()
fi

#
# Silence the audio before doing anything else.
#
# Halting the cores does not stop the board making sound, and the audio DMA is
# built so that it cannot: backend/pico/pio-i2s.c gives the data channel
# chain_to the control channel, and the control channel a two-entry address
# ring. Finishing one buffer re-triggers the other, for ever. The DMA interrupt
# only *asks* the CPU to refill the buffer it just left; nothing waits for that
# to happen, which is exactly what makes the output glitch-free in normal use.
#
# So with the cores stopped the chain keeps cycling the same two buffers into
# the DAC - whatever samples happened to be in them, a few hundred times a
# second, for as long as the flashing takes. There is no firmware-side fix for
# this: by the time it matters the firmware is not running.
#
# So the first thing to do after halting is to turn off the things that are
# still running, in the order that keeps them quiet:
#
#   1. PIO1 CTRL = 0    stops the I2S state machines, so BCLK and LRCLK stop.
#                       The MAX98357A mutes when its clocks go away, which is
#                       what actually ends the noise.
#   2. PIO0 CTRL = 0    the display, for symmetry - harmless, the panel holds
#                       its last frame.
#   3. DMA CHAN_ABORT   aborts every channel in one write, which breaks the
#                       chain without giving either half a chance to
#                       re-trigger the other. The clocks are already gone by
#                       this point, so this is about leaving the DMA in a sane
#                       state for the flash write, not about the noise.
#
# Doing it by register write rather than by reset is deliberate, and the target
# config settles the question: rp2350.cfg line 141 is
#
#     $_TARGETNAME_CM0 cortex_m reset_config sysresetreq
#
# so OpenOCD's reset goes through the core's SYSRESETREQ, not through the RUN
# pin. That resets the processor subsystem; it is not a power-on reset of the
# whole chip, and peripheral state is not something to rely on it clearing.
# Writing the registers does not depend on any of that.
#
# `program` still resets afterwards, and because it halts immediately on reset
# the old firmware never gets to start the audio up again.
#
# Addresses are RP2350's, from the SDK's hardware/regs headers:
#   PIO0_BASE 0x50200000, PIO1_BASE 0x50300000, PIO_CTRL_OFFSET 0x00
#   DMA_BASE  0x50000000, DMA_CHAN_ABORT_OFFSET 0x464
#
# Wrapped in catch so a probe that cannot do this still flashes: being unable
# to silence the board is a nuisance, not a reason to refuse to program it.
#
SILENCE='
proc picopop_silence {} {
	# Halting is best-effort: the register writes are what matter, and they
	# work whether or not the cores stopped cleanly. Selecting a core is
	# likewise best-effort - the default target can address memory too, and
	# both cores see the same bus.
	catch { targets rp2350.cm0 }
	catch { halt }
	mww 0x50300000 0
	mww 0x50200000 0
	mww 0x50000464 0xffff
	echo "picopop: audio and display PIO stopped, DMA aborted"
}
if { [catch { picopop_silence } msg] } {
	echo "picopop: could not silence the board first ($msg) - flashing anyway"
}
'

CMSIS_DAP_SERIAL=/dev/ttyACM0 "$OPENOCD" "${OPENOCD_ARGS[@]}" \
	-f interface/cmsis-dap.cfg -f target/rp2350.cfg \
	-c "adapter speed 5000" \
	-c "init" \
	-c "$SILENCE" \
	-c "program $FIRMWARE verify reset exit"
