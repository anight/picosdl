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

void psdl_backend_delay_ms(Uint32 ms)
{
	sleep_ms(ms);
}
