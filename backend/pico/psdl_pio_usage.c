/*
 * Reading PIO usage out of the hardware and out of the SDK.
 *
 * See psdl_pio_usage.h for what the two sources are and why neither alone is
 * the whole picture. Reports go to stdout, with the rest of the boot log.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/regs/pio.h"
#include "hardware/structs/pio.h"

#include "psdl_pio_usage.h"

/*
 * A one-instruction program that could go anywhere, used only as a question.
 *
 * The SDK keeps its allocation bitmap in a file-static in pio.c and exports no
 * way to read it, but pio_can_add_program_at_offset() answers "is there room
 * for this here", and for a single relocatable instruction that is the same as
 * "is slot n free". Asking 32 times reconstructs the bitmap exactly.
 *
 * origin -1 so any offset is legal, pio_version 0 so it is acceptable to both
 * PIO versions, and no GPIO ranges so the RP2350 compatibility check passes
 * whatever base a block is set to. The instruction is never executed: nothing
 * here adds the program, only asks about it.
 */
static const uint16_t s_probe_instr[1] = { 0x0000 };
static const pio_program_t s_probe = {
	.instructions = s_probe_instr,
	.length       = 1,
	.origin       = -1,
	.pio_version  = 0,
#if PICO_PIO_VERSION > 0
	.used_gpio_ranges = 0,
#endif
};

static uint32_t sdk_allocated_slots(PIO pio)
{
	uint32_t used = 0;

	for (uint off = 0; off < PIO_INSTRUCTION_COUNT; ++off) {
		if (!pio_can_add_program_at_offset(pio, &s_probe, off))
			used |= 1u << off;
	}
	return used;
}

void psdl_pio_usage_read(struct psdl_pio_usage *out)
{
	memset(out, 0, sizeof(*out));

	for (uint i = 0; i < NUM_PIOS; ++i) {
		PIO                    pio = pio_get_instance(i);
		struct psdl_pio_block *b   = &out->block[i];

		b->sdk_slots  = sdk_allocated_slots(pio);
		b->sm_enabled = pio->ctrl & PIO_CTRL_SM_ENABLE_BITS;

		for (uint sm = 0; sm < 4; ++sm) {
			if (pio_sm_is_claimed(pio, sm))
				b->sm_claimed |= 1u << sm;

			uint32_t ec = pio->sm[sm].execctrl;
			b->wrap_lo[sm] = (ec & PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS)
			                 >> PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB;
			b->wrap_hi[sm] = (ec & PIO_SM0_EXECCTRL_WRAP_TOP_BITS)
			                 >> PIO_SM0_EXECCTRL_WRAP_TOP_LSB;
		}
	}
}

/* The slots a running state machine's wrap range covers. */
static uint32_t wrap_slots(const struct psdl_pio_block *b, uint sm)
{
	uint lo = b->wrap_lo[sm], hi = b->wrap_hi[sm];

	if (lo > hi || hi >= PIO_INSTRUCTION_COUNT)
		return 0;

	/* (2 << hi) rather than (1 << (hi + 1)): hi can be 31. */
	return (uint32_t)((2ull << hi) - (1ull << lo));
}

/*
 * What has already been reported, so no two drivers are credited with the same
 * resource. Subtracted from every delta and grown by every report.
 */
static struct psdl_pio_usage s_attributed;

int psdl_pio_usage_report(const char *who, const struct psdl_pio_usage *before,
                          int announce_nothing)
{
	struct psdl_pio_usage now;
	int                   any = 0;

	psdl_pio_usage_read(&now);

	for (uint i = 0; i < NUM_PIOS; ++i) {
		const struct psdl_pio_block *a = &before->block[i];
		const struct psdl_pio_block *n = &now.block[i];
		struct psdl_pio_block       *t = &s_attributed.block[i];

		uint8_t  new_sms   = (n->sm_enabled & ~a->sm_enabled) |
		                     (n->sm_claimed & ~a->sm_claimed);
		uint32_t new_slots = n->sdk_slots & ~a->sdk_slots;

		/* Programs written straight into instr_mem never reach the SDK's
		 * bitmap, so take the extent of every machine this driver started
		 * as well. The union counts a program two machines share once. */
		for (uint sm = 0; sm < 4; ++sm) {
			if (n->sm_enabled & ~a->sm_enabled & (1u << sm))
				new_slots |= wrap_slots(n, sm);
		}

		new_sms   &= ~t->sm_enabled;
		new_slots &= ~t->sdk_slots;

		if (!new_sms && !new_slots)
			continue;

		t->sm_enabled |= new_sms;
		t->sdk_slots  |= new_slots;

		char   sms[16];
		size_t len = 0;
		sms[0] = '\0';
		for (uint sm = 0; sm < 4; ++sm) {
			if (!(new_sms & (1u << sm)))
				continue;
			len += (size_t)snprintf(sms + len, sizeof(sms) - len,
			                        "%sSM%u", len ? "," : "", sm);
		}
		if (len == 0)
			snprintf(sms, sizeof(sms), "SM-");

		printf("picosdl: %s acquired PIO%u/%s "
		       "(%u instructions, %u total)\n",
		       who, i, sms, (uint)__builtin_popcount(new_slots),
		       (uint)__builtin_popcount(t->sdk_slots));
		any = 1;
	}

	if (!any && announce_nothing)
		printf("picosdl: %s acquired no PIO resources\n", who);

	return any;
}
