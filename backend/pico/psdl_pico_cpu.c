/*
 * How busy each core is.
 *
 * Load is measured as the complement of idle, and idle is defined as the time
 * a core spends inside PSDL_CpuIdle()/PSDL_CpuBusy(). That is the whole model:
 * picosdl cannot see what a core is doing, but it can be told when a core has
 * stopped doing it, and anything that is not idle is work.
 *
 * Bracketing the waits rather than the work is what makes this usable from a
 * client. A client would have to wrap every path it has to report busy time
 * directly, and would report nothing at all for the paths it forgot; it has
 * only a handful of places where it blocks, and missing one costs an
 * overstated load rather than a meaningless number.
 *
 * picosdl brackets its own waits the same way, so a client that never calls
 * either function still gets core 0 measured - the panel wait and SDL_Delay()
 * are where a single-threaded client spends its spare frame.
 *
 * Which core a call refers to is the core it is made from. There is no handle
 * and no core argument, because a core can only describe itself: the state
 * being tracked is "is this core running", and only this core knows.
 */
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "psdl_internal.h"

/*
 * The idle total is monotonic and is never cleared, and the reader keeps its own
 * previous sample and subtracts. That is what makes the cross-core read safe
 * without a lock: core 1 only ever adds to its counter and core 0 only ever
 * reads it, where a read-and-clear would lose whatever core 1 added between the
 * read and the clear. Unsigned wrap is fine - only differences are ever used.
 */
struct core_stat {
	volatile uint32_t idle_us;      /* monotonic; written only by this core */
	volatile uint8_t  seen;         /* this core has reported at least once */

	/* Owned by the core being measured. */
	uint32_t t0;                    /* when the current idle began */
	int      depth;

	/* Owned by whoever reads the load; core 0 in practice, except for the
	 * one-time seeding below. */
	uint32_t last_idle_us;
	uint32_t last_us;
};

static struct core_stat s_core[2];

void PSDL_CpuIdle(void)
{
	struct core_stat *c = &s_core[get_core_num()];

	/*
	 * Open the reader's window here rather than on its first read, so the first
	 * interval reported is a real measurement instead of a zero standing in for
	 * one. The barrier orders it before `seen`, which is what lets the reader
	 * look at all: nobody reads this core's state until that flag is up.
	 */
	if (!c->seen) {
		c->last_us      = time_us_32();
		c->last_idle_us = 0;
		__dmb();
		c->seen = 1;
	}

	/*
	 * Nested, so a client that marks itself idle around a wait that picosdl
	 * also brackets internally is counted once rather than twice, and the
	 * inner bracket does not end the outer one.
	 */
	if (c->depth++ == 0)
		c->t0 = time_us_32();
}

void PSDL_CpuBusy(void)
{
	struct core_stat *c = &s_core[get_core_num()];

	/* Unbalanced: this core was already busy, which is what it is telling us. */
	if (c->depth == 0)
		return;

	if (--c->depth == 0)
		c->idle_us += time_us_32() - c->t0;
}

/*
 * Percentage of `core` spent working since the last call, or -1 if that core has
 * never said anything - which is how a build with core 1 unused reports nothing
 * rather than a permanent zero that looks like a measurement.
 *
 * An idle that is still in progress when this runs has not been added to the
 * total yet, so it lands in the next interval instead of this one. Over a
 * one-second window that is at most one wait's worth of misattribution and it
 * corrects itself immediately; closing the in-flight window here would mean
 * writing another core's state, which is the one thing the lock-free scheme
 * above depends on nobody doing.
 */
int psdl_backend_cpu_load(int core)
{
	struct core_stat *c = &s_core[core & 1];

	if (!c->seen)
		return -1;

	uint32_t now  = time_us_32();
	uint32_t idle = c->idle_us;

	uint32_t elapsed   = now - c->last_us;
	uint32_t idle_span = idle - c->last_idle_us;

	c->last_us      = now;
	c->last_idle_us = idle;

	if (elapsed == 0)
		return 0;
	if (idle_span > elapsed)
		idle_span = elapsed;

	return (int)(100u - (uint32_t)(((uint64_t)idle_span * 100u) / elapsed));
}
