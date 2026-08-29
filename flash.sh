#! /bin/bash

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

CMSIS_DAP_SERIAL=/dev/ttyACM0 "$OPENOCD" "${OPENOCD_ARGS[@]}" \
	-f interface/cmsis-dap.cfg -f target/rp2350.cfg \
	-c "adapter speed 5000" -c "program $FIRMWARE verify reset exit"
