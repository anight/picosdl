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
 * sleep_ms() is the SDK's WFE-based sleep, so a client waiting out a frame here
 * is genuinely asleep rather than spinning - SDLPoP's do_simple_wait() loops on
 * SDL_Delay(1) exactly this way. It is one of the two places picosdl can see a
 * single-threaded client stop working; the panel wait is the other.
 */
void psdl_backend_delay_ms(Uint32 ms)
{
	PSDL_CpuIdle();
	sleep_ms(ms);
	PSDL_CpuBusy();
}
