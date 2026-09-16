/*
 * Time. The RP2040 timer is a free-running 64-bit microsecond counter, which is
 * both of the clocks SDL asks for.
 */
#include "pico/stdlib.h"
#include "pico/time.h"

#include "psdl_internal.h"

Uint32 psdl_backend_ticks_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}

Uint64 psdl_backend_ticks_us(void)
{
	return time_us_64();
}

/*
 * Core 0's idle time, in microseconds since the last read.
 *
 * Two things idle core 0 and both are picosdl's to see. This is one: sleep_ms() is
 * the SDK's WFE-based sleep, so a client waiting out a frame here is genuinely
 * asleep rather than spinning - SDLPoP's do_simple_wait() loops on SDL_Delay(1)
 * exactly this way. The other is time blocked waiting for the panel transfer, which
 * the video backend adds to the same counter before reading it for the status band.
 *
 * Both writers run on core 0, so the read-and-clear needs no lock.
 */
volatile uint32_t psdl_pico_core0_idle_us;

void psdl_backend_delay_ms(Uint32 ms)
{
	absolute_time_t t0 = get_absolute_time();
	sleep_ms(ms);
	psdl_pico_core0_idle_us +=
		(uint32_t)absolute_time_diff_us(t0, get_absolute_time());
}
