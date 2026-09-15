/*
 * The event queue and the keyboard state array.
 *
 * SDL_Scancode values are USB HID usage codes, which is exactly what a
 * Bluetooth keyboard reports, so the key state array is indexed by the usage
 * straight off the wire - no translation table anywhere in the port.
 *
 * The queue is a fixed ring. Producers are the input backend (called from
 * SDL_PumpEvents on the same core) and SDL_PushEvent; on the Pico the BT stack
 * delivers reports from an async_context callback, so pushes can also arrive
 * from an interrupt-ish context. Head and tail are volatile and only ever
 * advanced by one side each, which makes the single-producer/single-consumer
 * case safe without a lock.
 */
#include "psdl_internal.h"

static SDL_Event      s_queue[PSDL_EVENT_QUEUE_LEN];
static volatile int   s_head;   /* producer writes here */
static volatile int   s_tail;   /* consumer reads here  */
static unsigned       s_dropped;

static Uint8  s_keystate[SDL_NUM_SCANCODES];
static Uint16 s_modstate;

void psdl_events_init(void)
{
	s_head = s_tail = 0;
	s_dropped = 0;
	s_modstate = 0;
	memset(s_keystate, 0, sizeof(s_keystate));
}

static int queue_push(const SDL_Event *ev)
{
	int head = s_head;
	int next = (head + 1) % PSDL_EVENT_QUEUE_LEN;
	if (next == s_tail) {
		/* Full. Dropping is the right failure here: the alternative is
		 * blocking an input callback, and stale input is worse than lost
		 * input. Counted so it is visible rather than silent. */
		s_dropped++;
		return 0;
	}
	s_queue[head] = *ev;
	s_head = next;
	return 1;
}

/* ------------------------------------------------------- producers */

void psdl_push_key(SDL_Scancode scancode, int pressed, Uint16 mod)
{
	if ((unsigned)scancode >= SDL_NUM_SCANCODES)
		return;

	/* Volume and mute are handled here rather than delivered. See
	 * psdl_audio_volume_key(): there is no window manager on this hardware to
	 * take the media keys, so the library does it. Deliberately before
	 * s_keystate, so SDL_GetKeyboardState() never shows them held either. */
	if (psdl_audio_volume_key(scancode, pressed))
		return;

	s_keystate[scancode] = pressed ? 1 : 0;
	s_modstate = mod;

	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type          = pressed ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.timestamp = psdl_backend_ticks_ms();
	ev.key.state     = pressed ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat    = 0;
	ev.key.keysym.scancode = scancode;
	ev.key.keysym.sym      = (SDL_Keycode)scancode;
	ev.key.keysym.mod      = mod;
	queue_push(&ev);
}

void psdl_push_joy_axis(int axis, Sint16 value)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type            = SDL_JOYAXISMOTION;
	ev.jaxis.timestamp = psdl_backend_ticks_ms();
	ev.jaxis.which     = 0;
	ev.jaxis.axis      = (Uint8)axis;
	ev.jaxis.value     = value;
	queue_push(&ev);
}

void psdl_push_joy_button(int button, int pressed)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type              = pressed ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
	ev.jbutton.timestamp = psdl_backend_ticks_ms();
	ev.jbutton.which     = 0;
	ev.jbutton.button    = (Uint8)button;
	ev.jbutton.state     = pressed ? SDL_PRESSED : SDL_RELEASED;
	queue_push(&ev);
}

void psdl_push_quit(void)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_QUIT;
	queue_push(&ev);
}

/* --------------------------------------------------------- consumer */

void psdl_timer_service(void);

void SDL_PumpEvents(void)
{
	psdl_backend_input_poll();
	psdl_timer_service();
}

int SDL_PollEvent(SDL_Event *event)
{
	SDL_PumpEvents();

	int tail = s_tail;
	if (tail == s_head)
		return 0;
	if (event != NULL)
		*event = s_queue[tail];
	s_tail = (tail + 1) % PSDL_EVENT_QUEUE_LEN;
	return 1;
}

int SDL_PushEvent(SDL_Event *event)
{
	if (event == NULL)
		return -1;
	return queue_push(event) ? 1 : 0;
}

void SDL_FlushEvent(Uint32 type)
{
	/* Compact in place: copy everything that is not `type` back down. */
	int tail = s_tail, head = s_head, write = s_tail;
	while (tail != head) {
		if (s_queue[tail].type != type) {
			if (write != tail)
				s_queue[write] = s_queue[tail];
			write = (write + 1) % PSDL_EVENT_QUEUE_LEN;
		}
		tail = (tail + 1) % PSDL_EVENT_QUEUE_LEN;
	}
	s_head = write;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
	if (numkeys != NULL)
		*numkeys = SDL_NUM_SCANCODES;
	return s_keystate;
}

Uint16 SDL_GetModState(void)
{
	return s_modstate;
}

unsigned PSDL_DroppedEvents(void)
{
	return s_dropped;
}
