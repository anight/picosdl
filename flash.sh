#! /bin/bash

set -e

FIRMWARE="${1:-./build/picosdl-demo.elf}"

if [ ! -f "$FIRMWARE" ]; then
	echo "usage: $0 [firmware.elf]" >&2
	echo "no such firmware file: $FIRMWARE" >&2
	exit 1
fi

CMSIS_DAP_SERIAL=/dev/ttyACM0 openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "program $FIRMWARE verify reset exit"
