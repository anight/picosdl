/*
 * Timers and the clock.
 *
 * SDL_AddTimer callbacks run from SDL_PumpEvents, not from an interrupt. Real
 * SDL runs them on a thread, and SDLPoP's timer callback pushes an SDL_USEREVENT
 * - doing that from an alarm IRQ would mean the event ring had a genuine
 * interrupt producer, and everything downstream would need to be interrupt-safe
 * for no benefit. The game polls events continuously (see idle()), so servicing
 * timers there fires them close enough to on time.
 */
#include "psdl_internal.h"

typedef struct {
	int               active;
	Uint32            interval;
	Uint32            next_due;
	SDL_TimerCallback callback;
	void             *param;
} psdl_timer_t;

static psdl_timer_t s_timers[PSDL_MAX_TIMERS];

void psdl_timer_init(void)
{
	memset(s_timers, 0, sizeof(s_timers));
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param)
{
	if (callback == NULL || interval == 0)
		return 0;

	for (int i = 0; i < PSDL_MAX_TIMERS; ++i) {
		if (s_timers[i].active)
			continue;
		s_timers[i].active   = 1;
		s_timers[i].interval = interval;
		s_timers[i].next_due = psdl_backend_ticks_ms() + interval;
		s_timers[i].callback = callback;
		s_timers[i].param    = param;
		return i + 1;   /* 0 means failure, so ids are 1-based */
	}

	SDL_SetError("picosdl: out of timers - raise PSDL_MAX_TIMERS");
	return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
	if (id <= 0 || id > PSDL_MAX_TIMERS || !s_timers[id - 1].active)
		return SDL_FALSE;
	s_timers[id - 1].active = 0;
	return SDL_TRUE;
}

void psdl_timer_service(void)
{
	Uint32 now = psdl_backend_ticks_ms();

	for (int i = 0; i < PSDL_MAX_TIMERS; ++i) {
		psdl_timer_t *t = &s_timers[i];
		if (!t->active)
			continue;
		/* Unsigned wrap makes this comparison work across the 49-day
		 * rollover, which a plain `now >= next_due` would not. */
		if ((Sint32)(now - t->next_due) < 0)
			continue;

		Uint32 next = t->callback(t->interval, t->param);
		if (next == 0) {
			t->active = 0;
		} else {
			t->interval = next;
			/* Schedule from now rather than from the due time: catching
			 * up on a backlog after a long frame would just fire a burst
			 * of callbacks the game cannot use. */
			t->next_due = now + next;
		}
	}
}

Uint32 SDL_GetTicks(void)
{
	return psdl_backend_ticks_ms();
}

void SDL_Delay(Uint32 ms)
{
	psdl_backend_delay_ms(ms);
}

Uint64 SDL_GetPerformanceCounter(void)
{
	return psdl_backend_ticks_us();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	return 1000000u;
}
