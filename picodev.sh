#! /bin/bash
#
# Program, reset and watch a board over SWD.
#
#   ./picodev.sh flash [firmware.elf]   program and reset
#   ./picodev.sh reset                  reset the board, program nothing
#   ./picodev.sh halt                   stop the cores and silence the audio
#   ./picodev.sh logs                   watch the console (tail -f, in effect)
#   ./picodev.sh flash-and-logs [fw]    program, reset, then watch the console
#
# The part is worked out from the probe. Put --rp2040 or --rp2350 first to say so
# explicitly instead - needed only if the detection cannot place what it finds.
#
# A bare path is still accepted, so `./picodev.sh build/picopop.elf` means the same
# as `./picodev.sh flash build/picopop.elf`. With no arguments at all it prints this
# and does nothing: it used to flash a default image, which is too much to do on
# an empty command line now that there are commands that do not touch the flash.
#
# Environment:
#   OPENOCD           override the OpenOCD binary
#   PICOPOP_CONSOLE   the board's serial console (default: the Debugprobe's UART)
#   PICOPOP_BAUD      console baud rate (default 115200)
#   PROBE_SERIAL      pick one probe by serial number, if more than one is attached
#
set -e

DEFAULT_FIRMWARE=./build/picosdl-demo.elf
BAUD="${PICOPOP_BAUD:-115200}"

usage() {
	sed -n '3,16p' "$0" | sed 's/^#\{1,2\} \{0,1\}//'
	exit "${1:-1}"
}

# --------------------------------------------------------------------- chip

#
# An explicit --rp2040 / --rp2350 wins; otherwise the part is detected below,
# once the OpenOCD binary has been picked and only when a probe is actually
# needed. `logs` never touches the probe and must keep working with none
# attached, so detection cannot happen here.
#
CHIP=""
case "${1:-}" in
--rp2040|--rp2350)
	CHIP="${1#--}"
	shift
	;;
esac

# ------------------------------------------------------------------ OpenOCD

# Distro OpenOCD 0.12.0 has no RP2350 target. Prefer the Pico SDK's build.
SDK_OPENOCD="${HOME}/.pico-sdk/openocd/0.12.0+dev/openocd"
SDK_SCRIPTS="${HOME}/.pico-sdk/openocd/0.12.0+dev/scripts"

if [ -x "$SDK_OPENOCD" ]; then
	OPENOCD="${OPENOCD:-$SDK_OPENOCD}"
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
# Doing it by register write rather than by reset is deliberate, and both target
# configs settle the question the same way - rp2040.cfg line 84 and rp2350.cfg
# line 141 are each
#
#     $_TARGETNAME... cortex_m reset_config sysresetreq
#
# so OpenOCD's reset goes through the core's SYSRESETREQ, not through the RUN
# pin. That resets the processor subsystem; it is not a power-on reset of the
# whole chip, and peripheral state is not something to rely on it clearing.
# Writing the registers does not depend on any of that.
#
# Addresses, from the SDK's hardware/regs headers for each part:
#   PIO0_BASE 0x50200000, PIO1_BASE 0x50300000, PIO_CTRL_OFFSET 0x00 - both
#   DMA_BASE  0x50000000 - both
#   DMA_CHAN_ABORT_OFFSET  0x444 on RP2040, 0x464 on RP2350
#
# The 0xffff written to it is right for both: DMA_CHAN_ABORT_BITS is 0xffff on
# each, and RP2040 simply has no channels above 11 to abort.
#
# Wrapped in catch so a probe that cannot do this still flashes: being unable
# to silence the board is a nuisance, not a reason to refuse to program it.
#
silence_tcl() {
	cat <<TCL
proc picopop_silence {} {
	# Halting is best-effort: the register writes are what matter, and they
	# work whether or not the cores stopped cleanly. Selecting a core is
	# likewise best-effort - the default target can address memory too, and
	# both cores see the same bus.
	catch { targets $CORE0 }
	catch { halt }
	mww 0x50300000 0
	mww 0x50200000 0
	mww $DMA_CHAN_ABORT 0xffff
	echo "picopop: audio and display PIO stopped, DMA aborted"
}
if { [catch { picopop_silence } msg] } {
	echo "picopop: could not silence the board first (\$msg) - flashing anyway"
}
TCL
}

#
# Work out which part is on the other end, by asking the debug port and nobody
# else.
#
# OpenOCD will bring up SWD and read DPIDR with no target config at all - it
# then says "gdb services need one or more targets defined" and exits, which is
# fine, because the line we want has already been printed. That is what makes
# this safe to do first: no target is selected, nothing is halted, and no memory
# is touched, so a detection against the wrong guess is impossible rather than
# merely unlikely.
#
# The top nibble of DPIDR is the revision, so it is masked off before comparing:
# a new stepping of either part would otherwise stop being recognised.
#
#   0x0c013477   RP2350 - measured, on a Pico 2 W over a Debugprobe
#   0x0bc12477   RP2040 - from the datasheet, not measured here
#
# If the RP2040 constant is wrong the cost is small and visible: an RP2040 lands
# in the "could not place it" branch below and asks for --rp2040, rather than
# being mistaken for something else. Nothing acts on a guess.
#
detect_chip() {
	local out dpidr
	out=$("$OPENOCD" "${OPENOCD_ARGS[@]}" \
		-f interface/cmsis-dap.cfg \
		"${PROBE_ARGS[@]}" \
		-c "transport select swd" \
		-c "adapter speed 1000" \
		-c "swd newdap probe cpu -expected-id 0" \
		-c "dap create probe.dap -chain-position probe.cpu" \
		-c "init" -c "exit" 2>&1) || true

	dpidr=$(printf '%s\n' "$out" |
		sed -n 's/.*SWD DPIDR \(0x[0-9a-fA-F]*\).*/\1/p' | tail -1)

	if [ -z "$dpidr" ]; then
		echo "$0: no debug port answered - is a probe attached and powered?" >&2
		printf '%s\n' "$out" | sed -n '/Error\|error/p' | head -3 >&2
		exit 1
	fi

	case $(( dpidr & 0x0fffffff )) in
	$(( 0x0c013477 ))) echo rp2350 ;;
	$(( 0x0bc12477 ))) echo rp2040 ;;
	*)
		echo "$0: debug port $dpidr is not one this knows" >&2
		echo "  pass --rp2040 or --rp2350 to say which part it is" >&2
		exit 1
		;;
	esac
}

#
# Everything downstream of knowing the chip. Three things differ between the
# parts and nothing else does:
#
#   the OpenOCD target config
#   the names the two cores answer to - RP2040 numbers them, RP2350 names them
#     by architecture because it also has RISC-V cores at rv0/rv1
#   DMA_CHAN_ABORT, which moved when RP2350 grew from 12 channels to 16
#
# PIO0_BASE, PIO1_BASE and the PIO CTRL offset are identical on both, which is
# why the silence sequence carries only the one address.
#
chip_resolve() {
	[ -n "$TARGET_CFG" ] && return 0        # already done

	PROBE_ARGS=()
	[ -n "$PROBE_SERIAL" ] && PROBE_ARGS=(-c "cmsis_dap_serial $PROBE_SERIAL")

	if [ -z "$CHIP" ]; then
		CHIP=$(detect_chip)
		echo "picodev: detected $CHIP" >&2
	fi

	case "$CHIP" in
	rp2040)
		TARGET_CFG=target/rp2040.cfg
		CORE0=rp2040.core0
		CORE1=rp2040.core1
		DMA_CHAN_ABORT=0x50000444
		;;
	rp2350)
		TARGET_CFG=target/rp2350.cfg
		CORE0=rp2350.cm0
		CORE1=rp2350.cm1
		DMA_CHAN_ABORT=0x50000464
		;;
	esac

	SILENCE=$(silence_tcl)
}

# Run OpenOCD with the silence sequence, then whatever commands follow.
openocd_run() {
	chip_resolve
	"$OPENOCD" "${OPENOCD_ARGS[@]}" \
		-f interface/cmsis-dap.cfg -f "$TARGET_CFG" \
		"${PROBE_ARGS[@]}" \
		-c "adapter speed 5000" \
		-c "init" \
		-c "$SILENCE" \
		"$@"
}

# ------------------------------------------------------------------ console

# The Debugprobe exposes two USB interfaces: CMSIS-DAP, which OpenOCD drives, and
# a UART bridge, which is the board's console. They are independent, so flashing
# and watching at the same time is fine.
#
# The by-id path is preferred because ttyACMn is assigned in enumeration order and
# moves when anything else is plugged in.
find_console() {
	if [ -n "$PICOPOP_CONSOLE" ]; then
		echo "$PICOPOP_CONSOLE"
		return
	fi
	local p
	for p in /dev/serial/by-id/*Debugprobe*if01 /dev/serial/by-id/*CMSIS-DAP*if01; do
		[ -e "$p" ] && { echo "$p"; return; }
	done
	echo /dev/ttyACM0
}

#
# Refuse to read a console someone else is already reading.
#
# Two readers on one tty do not take turns - each gets whichever bytes it happens
# to win, so both see a stream with chunks missing from the middle of lines. It
# looks exactly like corruption on the wire, and it cost a long detour into the
# UART driver and the SDK's stdio locking before the real cause (a minicom left
# open in another window) turned up. Better to stop with a name to go and close.
#
console_check_free() {
	local dev="$1" holders=""
	if command -v fuser >/dev/null 2>&1; then
		holders=$(fuser "$dev" 2>/dev/null || true)
	elif command -v lsof >/dev/null 2>&1; then
		holders=$(lsof -t "$dev" 2>/dev/null || true)
	else
		return 0        # no way to tell; assume the user knows
	fi
	[ -z "$holders" ] && return 0

	echo "$0: $dev is already open by PID(s):$holders" >&2
	command -v ps >/dev/null 2>&1 && ps -o pid=,comm= -p $holders >&2
	cat >&2 <<-EOF

	Two readers on one serial port split the bytes between them, so both see
	lines with pieces missing. Close the other one (minicom, screen, another
	picodev.sh) and try again.
	EOF
	exit 1
}

console_watch() {
	local dev; dev=$(find_console)
	[ -e "$dev" ] || { echo "$0: no console at $dev" >&2; exit 1; }
	console_check_free "$dev"

	stty -F "$dev" "$BAUD" raw -echo -echoe -echok 2>/dev/null ||
		echo "$0: could not configure $dev, reading it anyway" >&2

	echo "--- $dev at $BAUD, Ctrl-C to stop ---" >&2
	cat "$dev"
}

# ------------------------------------------------------------------ commands

resolve_firmware() {
	FIRMWARE="${1:-$DEFAULT_FIRMWARE}"
	if [ ! -f "$FIRMWARE" ]; then
		echo "$0: no such firmware file: $FIRMWARE" >&2
		exit 1
	fi
}

cmd_flash() {
	resolve_firmware "$1"
	openocd_run -c "program $FIRMWARE verify reset exit"
}

cmd_reset() {
	openocd_run -c "reset run" -c "exit"
}

#
# Stop the board and leave it stopped.
#
# `halt` on its own is not enough to make a board go quiet, which is the whole
# reason the silence sequence above exists: the audio DMA chain re-triggers itself
# and keeps cycling the last two buffers into the DAC whether or not a CPU is
# running. openocd_run already runs that sequence, so by the time the explicit
# halts below execute the sound has already stopped.
#
# Both cores, because halting core 0 leaves the mixer running on core 1.
#
# OpenOCD leaves the target halted when it detaches, so the board stays stopped
# after this returns - `reset` starts it again.
#
cmd_halt() {
	openocd_run \
		-c "catch { targets $CORE0 }" -c "catch { halt }" \
		-c "catch { targets $CORE1 }" -c "catch { halt }" \
		-c "echo {picopop: both cores halted, board left stopped}" \
		-c "exit"
}

cmd_logs() {
	console_watch
}

#
# Start reading, then program and reset - in that order.
#
# The reader has to be attached first or the start-up banner is gone: the board is
# already well into its boot by the time a reader started afterwards gets there.
# Attaching first costs nothing, because programming halts the cores, so nothing
# arrives during it.
#
# Draining the port before attaching matters as much. Whatever the previous
# firmware printed is still sitting in the tty buffer, and without discarding it
# the log opens with a couple of dozen lines from the run before - which reads
# exactly as though the reset had been missed.
#
cmd_flash_and_logs() {
	resolve_firmware "$1"
	local dev; dev=$(find_console)
	[ -e "$dev" ] || { echo "$0: no console at $dev" >&2; exit 1; }
	console_check_free "$dev"

	stty -F "$dev" "$BAUD" raw -echo -echoe -echok 2>/dev/null || true
	timeout 0.3 cat "$dev" >/dev/null 2>&1 || true

	echo "--- $dev at $BAUD, Ctrl-C to stop ---" >&2
	cat "$dev" &
	local reader=$!
	# Ctrl-C should end the reader, not leave it holding the port.
	trap 'kill $reader 2>/dev/null; exit 0' INT TERM

	# Everything but the board's own output goes to stderr, so stdout is only the
	# console and `flash-and-logs | tee boot.log` captures just that. Programming
	# takes a while on a 2 MB image; the reader sits idle through it.
	openocd_run -c "program $FIRMWARE verify reset exit" >&2

	wait $reader
}

case "${1:-}" in
	"")             usage 1 ;;
	flash)          cmd_flash "$2" ;;
	reset)          cmd_reset ;;
	halt)           cmd_halt ;;
	logs)           cmd_logs ;;
	flash-and-logs) cmd_flash_and_logs "$2" ;;
	-h|--help|help) usage 0 ;;
	*)
		# A bare path: the old calling convention.
		if [ -f "$1" ]; then
			cmd_flash "$1"
		else
			echo "$0: unknown command '$1'" >&2
			usage 1
		fi
		;;
esac
