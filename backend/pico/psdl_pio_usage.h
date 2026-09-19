/*
 * What the PIO blocks are being used for, and by whom.
 *
 * Three drivers put programs into PIO and none of them knows about the other
 * two: the ST7789 display driver, the I2S output driver, and the CYW43 radio
 * driver inside the SDK. Between them they have to fit in each block's four
 * state machines and 32 instruction slots. This reads that out so the sharing
 * is a measurement rather than an assumption.
 *
 * Take a snapshot before a driver initialises and report against it afterwards;
 * the difference is what that driver took, printed as one line:
 *
 *     picosdl: display driver acquired PIO0/SM0,SM1 (13 instructions)
 *
 * Two things it cannot see, both worth knowing before reading a report:
 *
 *   Instruction memory is write-only in hardware (io_wo_32), so what a slot
 *   holds can never be read back. Two separate things stand in for it - the
 *   SDK's allocation bitmap, which only knows about programs added through
 *   pio_add_program(), and each state machine's wrap range, which covers any
 *   program a running machine is executing however it was loaded. A driver
 *   that writes instr_mem directly appears in the second and not the first, so
 *   the count is the union of both.
 *
 *   A state machine's wrap range is the extent of its program only while that
 *   machine is running, and two machines sharing one program report the same
 *   range. The union means such a program is counted once, not twice.
 */
#ifndef PSDL_PIO_USAGE_H
#define PSDL_PIO_USAGE_H

#include "hardware/pio.h"

struct psdl_pio_block {
	uint32_t sdk_slots;      /* bit n: slot n allocated via pio_add_program() */
	uint8_t  sm_claimed;     /* bit n: SM n claimed through the SDK           */
	uint8_t  sm_enabled;     /* bit n: SM n running, from CTRL                */
	uint8_t  wrap_lo[4];     /* EXECCTRL wrap bottom, per SM                  */
	uint8_t  wrap_hi[4];     /* EXECCTRL wrap top, per SM                     */
};

struct psdl_pio_usage {
	struct psdl_pio_block block[NUM_PIOS];
};

/* Read the current state of every PIO block. */
void psdl_pio_usage_read(struct psdl_pio_usage *out);

/*
 * Print what `who` took, attribute it, and say whether there was anything.
 *
 * `before` is the caller's own snapshot from just before its driver
 * initialised. That alone is not enough to attribute correctly, because the
 * drivers do not take their PIO in the order they are started: the radio's bus
 * program is added on an interrupt, so it can land inside another driver's
 * bracket, or after a bracket that was opened before it. So a report also
 * subtracts everything already reported, and records what it prints.
 *
 * One line per PIO block:
 *
 *     picosdl: display driver acquired PIO0/SM0,SM1 (13 instructions)
 *
 * With `announce_nothing`, a driver that took nothing says so, so a missing
 * line is a missing call rather than a silent result. Without it, nothing is
 * printed until there is something to print - which is what an asynchronous
 * acquirer needs, polled until this returns nonzero.
 */
int psdl_pio_usage_report(const char *who, const struct psdl_pio_usage *before,
                          int announce_nothing);

#endif
