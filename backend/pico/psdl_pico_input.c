/*
 * Input backend: a Bluetooth keyboard and an analog stick.
 *
 * The keyboard half is nearly free, and for a reason worth stating: SDL_Scancode
 * values *are* USB HID usage codes for every key a game cares about. The BT
 * stack hands us a usage; that usage is the scancode; it indexes the key state
 * array directly. There is no translation table anywhere in this port.
 *
 * BTstack runs off the CYW43 async_context, which in threadsafe_background mode
 * is interrupt-driven - so we never call btstack_run_loop_execute() (it would
 * park forever and the game would never run). Reports arrive on a low-priority
 * interrupt and are pushed onto the event ring from there.
 */
#include <stdio.h>

#include "btstack.h"
#include "btstack_stdin.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "bt_app.h"
#include "joystick.h"
#include "kbd_decode.h"

#include "psdl_internal.h"
#include "psdl_pico.h"

static int      s_ready;
static int      s_bt_up;
static unsigned s_keys_seen;

/* Joystick state, so we only emit events on a real change. */
static Sint16 s_last_axis[2];
static int    s_last_button;
static Uint32 s_last_poll_ms;

#define PSDL_JOY_POLL_INTERVAL_MS 5
/* An axis has to move this far (out of 32767) before it counts as a change.
 * Without it, ADC noise on a stick at rest produces a continuous event stream. */
#define PSDL_JOY_AXIS_EPSILON     512

/* -------------------------------------------------------- serial console */

/*
 * Typed into the serial terminal, not on the Bluetooth keyboard. Bluetooth is
 * the one subsystem here that can fail in ways nothing on screen explains - a
 * link that drops, a stale pairing, a keyboard on the wrong channel - and
 * without this the only way to look is a debugger.
 */
static void console_command(char cmd)
{
	switch (cmd) {
	case 's':
		bt_app_print_status();
		break;
	case 'r':
		printf("[console] forgetting the pairing and searching again\n");
		bt_app_forget_pairing();
		bt_app_search_again();
		break;
	case 'n':
		printf("[console] searching again\n");
		bt_app_search_again();
		break;
	case '?':
	case 'h':
		printf("[console] s = status, r = forget pairing and re-search, "
		       "n = re-search\n");
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------- keyboard */

static Uint16 sdl_mod_from_hid(uint8_t hid_modifiers)
{
	Uint16 mod = 0;
	if (hid_modifiers & KBD_MOD_LEFT_CTRL)   mod |= KMOD_LCTRL;
	if (hid_modifiers & KBD_MOD_LEFT_SHIFT)  mod |= KMOD_LSHIFT;
	if (hid_modifiers & KBD_MOD_LEFT_ALT)    mod |= KMOD_LALT;
	if (hid_modifiers & KBD_MOD_LEFT_GUI)    mod |= KMOD_LGUI;
	if (hid_modifiers & KBD_MOD_RIGHT_CTRL)  mod |= KMOD_RCTRL;
	if (hid_modifiers & KBD_MOD_RIGHT_SHIFT) mod |= KMOD_RSHIFT;
	if (hid_modifiers & KBD_MOD_RIGHT_ALT)   mod |= KMOD_RALT;
	if (hid_modifiers & KBD_MOD_RIGHT_GUI)   mod |= KMOD_RGUI;
	return mod;
}

static void on_key_event(const kbd_event_t *event)
{
	s_keys_seen++;
	psdl_push_key((SDL_Scancode)event->usage, event->pressed,
	              sdl_mod_from_hid(event->modifiers));
}

/* ------------------------------------------------------------- joystick */

static Sint16 axis_to_sdl(float v)
{
	/* joystick.c reports -1.0 .. +1.0 with a deadzone already applied. */
	if (v > 1.0f)  v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (Sint16)(v * 32767.0f);
}

static void poll_joystick(void)
{
	Uint32 now = psdl_backend_ticks_ms();
	if ((Uint32)(now - s_last_poll_ms) < PSDL_JOY_POLL_INTERVAL_MS)
		return;
	s_last_poll_ms = now;

	struct Joystick joy;
	joystickRead(&joy);

	/* joystick.c's Y is positive-up; SDL's is positive-down. */
	Sint16 axis[2];
	axis[0] = axis_to_sdl(joy.x);
	axis[1] = axis_to_sdl(-joy.y);

	for (int i = 0; i < 2; ++i) {
		int delta = (int)axis[i] - (int)s_last_axis[i];
		if (delta < 0)
			delta = -delta;
		/* Always report a return to exactly centre, however small the step:
		 * otherwise the stick can be left reading slightly deflected forever. */
		if (delta < PSDL_JOY_AXIS_EPSILON && !(axis[i] == 0 && s_last_axis[i] != 0))
			continue;
		s_last_axis[i] = axis[i];
		psdl_joystick_set_axis(i, axis[i]);
		psdl_push_joy_axis(i, axis[i]);
	}

	int button = joy.pressed ? 1 : 0;
	if (button != s_last_button) {
		s_last_button = button;
		psdl_joystick_set_button(0, button);
		psdl_push_joy_button(0, button);
	}
}

/* ------------------------------------------------------ backend interface */

void psdl_backend_input_init(void)
{
	if (s_ready)
		return;
	s_ready = 1;

	joystickInit();

	if (cyw43_arch_init() != 0) {
		printf("picosdl: cyw43_arch_init failed - is PICO_BOARD a wireless board?\n"
		       "         continuing with the joystick only\n");
		return;
	}

	bt_app_setup();
	btstack_stdin_setup(&console_command);

	/* bt_app_setup wired its own handler, which prints to the console. Take over
	 * the key events - the same decoder now feeds the SDL event queue instead -
	 * but hand bt_app.c's LED handler straight back. kbd_decode_init() sets both
	 * callbacks at once, so passing NULL here used to silently disable the
	 * keyboard's Caps and Num Lock lights. */
	kbd_decode_init(&on_key_event, &bt_app_set_keyboard_leds);

	hci_power_control(HCI_POWER_ON);

	/* Deliberately no btstack_run_loop_execute(): with the threadsafe_background
	 * arch the run loop is driven by interrupts off the async_context, and
	 * calling execute() here would never return. */
	s_bt_up = 1;

	printf("picosdl: input ready - Bluetooth searching, joystick calibrated\n");
}

void psdl_backend_input_poll(void)
{
	if (!s_ready)
		return;
	poll_joystick();
	/* Keyboard reports arrive on their own interrupt; nothing to poll. */
}

/* ---------------------------------------------------------------- status */

/*
 * Derived from key traffic rather than from the link state: bt_app.c keeps that
 * private, and inferring it from reports is honest about what we actually know
 * - that keys are arriving.
 */
bool psdl_pico_keyboard_connected(void)
{
	return s_keys_seen > 0;
}

const char *psdl_pico_input_status(void)
{
	static char buf[48];
	if (!s_bt_up)
		snprintf(buf, sizeof(buf), "BT OFF - JOYSTICK ONLY");
	else if (s_keys_seen == 0)
		snprintf(buf, sizeof(buf), "BT SEARCHING FOR KEYBOARD");
	else
		snprintf(buf, sizeof(buf), "KEYBOARD OK - %u KEYS", s_keys_seen);
	return buf;
}
