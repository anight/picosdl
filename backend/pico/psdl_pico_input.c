/*
 * Input backend: a Bluetooth keyboard, an analog stick and an I2C game pad.
 *
 * All three are optional and independent - PICOSDL_INPUT_BT_KEYBOARD,
 * PICOSDL_INPUT_JOYSTICK and PICOSDL_INPUT_GAMEPAD - and none is required. With
 * all three off this file still compiles to a working backend that reports no
 * input, which is a legitimate build: a game with an attract mode runs it for ever.
 * Nothing below assumes any particular device is present, and nothing insists on at
 * least one.
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

#include "pico/stdlib.h"

#if PSDL_HAVE_BT_KEYBOARD
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "bt_app.h"
#include "bt/debug.h"
#include "kbd_decode.h"
#endif

#if PSDL_HAVE_JOYSTICK
#include "joystick.h"
#endif

#if PSDL_HAVE_GAMEPAD
#include "seesaw_gamepad.h"
#endif

#include "psdl_internal.h"
#include "psdl_pico.h"
#include "psdl_pico_log.h"

static int      s_ready;
#if PSDL_HAVE_BT_KEYBOARD
static int      s_bt_up;
static unsigned s_keys_seen;
#endif

#if PSDL_HAVE_JOYSTICK
/* Joystick state, so we only emit events on a real change. */
static Sint16 s_last_axis[2];
static int    s_last_button;
static Uint32 s_last_poll_ms;
#endif

#if PSDL_HAVE_GAMEPAD
/*
 * The pad needs its own rate gate, and used to borrow the joystick's by accident.
 *
 * SDL_PumpEvents() calls this backend, and SDL_PollEvent() calls SDL_PumpEvents -
 * so a client draining its event queue with `while (SDL_PollEvent(&e))` pumps
 * several times a frame. The joystick has always been gated, but the pad was read
 * on every pump at 1.03 ms of I2C a go, which with the joystick compiled out had
 * nothing slowing it down at all. Same interval, its own timestamp.
 */
static Uint32 s_pad_poll_ms;
#endif

#define PSDL_JOY_POLL_INTERVAL_MS 5
/* An axis has to move this far (out of 32767) before it counts as a change.
 * Without it, ADC noise on a stick at rest produces a continuous event stream. */
#define PSDL_JOY_AXIS_EPSILON     512

/* -------------------------------------------------------- serial console */

/*
 * The serial console.
 *
 * Typed into the serial terminal, not on the Bluetooth keyboard. It used to be
 * wired through btstack_stdin_setup(), which hooks stdin onto BTstack's run loop -
 * so a build without Bluetooth had no console at all, including the commands that
 * have nothing to do with Bluetooth. On a board with a stick and a pad and no
 * keyboard that left a volume which could be neither read nor changed, since the
 * volume keys arrive over Bluetooth too.
 *
 * So stdin is polled here instead. getchar_timeout_us(0) returns immediately when
 * there is nothing waiting, and this runs on core 0 outside any interrupt, which is
 * the context stdio wants. Nothing about it needs BTstack, and dropping
 * btstack_stdin_setup() removes a dependency rather than adding one.
 *
 * The Bluetooth commands are still compiled out with Bluetooth; the rest are always
 * there.
 */
static void console_help(void)
{
	printf("[console]"
#if PSDL_HAVE_BT_KEYBOARD
	       " s = bluetooth status, r = forget pairing and re-search,"
	       " n = re-search, d = bluetooth log,"
#endif
	       " v = volume, +/- = louder/quieter, m = mute, h = this\n");
}

static void console_command(char cmd)
{
	switch (cmd) {
#if PSDL_HAVE_BT_KEYBOARD
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
	case 'd':
		pico_test_bt_keyboard_verbose = !pico_test_bt_keyboard_verbose;
		printf("[console] bluetooth logging %s\n",
		       pico_test_bt_keyboard_verbose ? "on" : "off");
		break;
#endif
	case 'v':
		printf("[console] master volume %d/%d\n",
		       PSDL_GetMasterVolume(), PSDL_VOLUME_UNITY);
		break;

	/*
	 * Volume, by handing the audio layer the same scancodes the keyboard's media
	 * keys produce. Reuse rather than a second implementation: the 3 dB ladder and
	 * the mute memory then cannot drift between the two ways of reaching them, and
	 * a board with no keyboard gets exactly the keyboard's behaviour.
	 */
	case '+':
	case '=':
		psdl_audio_volume_key(SDL_SCANCODE_VOLUMEUP, 1);
		break;
	case '-':
	case '_':
		psdl_audio_volume_key(SDL_SCANCODE_VOLUMEDOWN, 1);
		break;
	case 'm':
		psdl_audio_volume_key(SDL_SCANCODE_MUTE, 1);
		break;

	case '?':
	case 'h':
		console_help();
		break;
	default:
		break;
	}
}

/*
 * Drain whatever has been typed, a few times a second.
 *
 * The rate limit is not politeness, it is necessary. This is reached from
 * SDL_PumpEvents(), and SDL_PollEvent() calls that - so a client draining its queue
 * pumps several times a frame and an ungated poll ran ~840 times a second. Each call
 * is usually 13-30 us, which sounds free, but two things make it not:
 *
 *   * it is stdio, so with USB stdio enabled it can drive the TinyUSB device task.
 *     Measured, one early call took 48.5 ms - and an audio block is 11.6 ms.
 *   * 840 calls a second of stdio code is steady XIP traffic, and core 1 runs its
 *     OPL synthesis out of flash. The audio *path* is __not_in_flash_func, but the
 *     synthesis is not, so bus contention there eats into a block budget already
 *     peaking above 50%.
 *
 * Five times a second is far quicker than anyone can type and cuts both effects by
 * ~99%. The inner loop stays, so a pasted run of characters is still consumed at
 * once rather than one per poll.
 */
#define PSDL_CONSOLE_POLL_INTERVAL_MS 200

static void poll_console(void)
{
	static Uint32 last_ms;
	Uint32 now = psdl_backend_ticks_ms();
	if ((Uint32)(now - last_ms) < PSDL_CONSOLE_POLL_INTERVAL_MS)
		return;
	last_ms = now;

	for (;;) {
		int c = getchar_timeout_us(0);
		if (c == PICO_ERROR_TIMEOUT)
			return;
		if (c == '\r' || c == '\n')
			continue;         /* terminals send these; they are not commands */
		console_command((char)c);
	}
}

/* ------------------------------------------------------------- keyboard */

#if PSDL_HAVE_BT_KEYBOARD
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

#endif /* PSDL_HAVE_BT_KEYBOARD */

#if PSDL_HAVE_JOYSTICK
static Sint16 axis_to_sdl(float v)
{
	/* joystick.c reports -1.0 .. +1.0 with a deadzone already applied. */
	if (v > 1.0f)  v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (Sint16)(v * 32767.0f);
}

#endif /* PSDL_HAVE_JOYSTICK */

/*
 * Only needed when there is at least one stick to publish. With both the
 * joystick and the pad off nothing calls this, and an unused static would be a
 * warning in the one configuration that has to be clean: no inputs at all.
 */
#if PSDL_HAVE_JOYSTICK || PSDL_HAVE_GAMEPAD
/*
 * The controller's left stick has two possible sources, and both have to work.
 *
 * A client that finds a game controller stops listening to the joystick - SDLPoP
 * does exactly that, discarding every SDL_JOYAXISMOTION once
 * using_sdl_joystick_interface is clear - so simply adding a pad would make the
 * board's own analog stick go dead. Instead both feed the controller's left stick
 * and the larger deflection wins, per axis.
 *
 * Larger-magnitude rather than most-recent, because both sources are polled every
 * frame and a source sitting at rest would otherwise overwrite a deflected one a
 * few milliseconds later. Neither stick rests at exactly zero, so "wins" has to
 * mean "is pushed further", not "changed last".
 *
 * The stick also stays on the joystick device for clients that want it there -
 * picosdl's own demo reads it that way.
 */
static Sint16 s_stick_axis[2];   /* the board's ADC stick */
static Sint16 s_pad_axis[2];     /* the I2C game pad */

static void publish_left_stick(void)
{
	static const int axis_id[2] = { SDL_CONTROLLER_AXIS_LEFTX,
	                                SDL_CONTROLLER_AXIS_LEFTY };
	for (int i = 0; i < 2; ++i) {
		int a = s_stick_axis[i], b = s_pad_axis[i];
		int mag_a = a < 0 ? -a : a;
		int mag_b = b < 0 ? -b : b;
		psdl_controller_set_axis(axis_id[i], (Sint16)(mag_a >= mag_b ? a : b));
	}
}
#endif /* PSDL_HAVE_JOYSTICK || PSDL_HAVE_GAMEPAD */

#if PSDL_HAVE_JOYSTICK
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
	int moved = 0;
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
		s_stick_axis[i] = axis[i];
		moved = 1;
	}
	if (moved)
		publish_left_stick();

	int button = joy.pressed ? 1 : 0;
	if (button != s_last_button) {
		s_last_button = button;
		psdl_joystick_set_button(0, button);
		psdl_push_joy_button(0, button);
		/* Pressing the stick down is a stick click, so report it as one. SDLPoP
		 * ignores that button, which is the honest outcome: inventing a mapping
		 * onto A or X would collide with the pad's own buttons of that name. */
		psdl_controller_set_button(SDL_CONTROLLER_BUTTON_LEFTSTICK, button);
	}
}

/* ------------------------------------------------------ backend interface */

#endif /* PSDL_HAVE_JOYSTICK */

void psdl_backend_input_init(void)
{
	if (s_ready)
		return;
	s_ready = 1;

	psdl_log_init();

#if PSDL_HAVE_JOYSTICK
	joystickInit();
	psdl_joystick_set_present(1);
#endif

#if PSDL_HAVE_GAMEPAD
	/* Absent hardware is not an error - a build that can take a pad need not have
	 * one plugged in. Before anything reports input capability, because this is
	 * what decides whether SDL_IsGameController() is true. */
	psdl_controller_set_present(seesaw_gamepad_init());
#endif

#if PSDL_HAVE_BT_KEYBOARD
	if (cyw43_arch_init() != 0) {
		printf("picosdl: cyw43_arch_init failed - is PICO_BOARD a wireless board?\n"
		       "         carrying on without a keyboard\n");
	} else {
		bt_app_setup();

		/* bt_app_setup wired its own handler, which prints to the console. Take
		 * over the key events - the same decoder now feeds the SDL event queue
		 * instead - but hand bt_app.c's LED handler straight back.
		 * kbd_decode_init() sets both callbacks at once, so passing NULL here used
		 * to silently disable the keyboard's Caps and Num Lock lights. */
		kbd_decode_init(&on_key_event, &bt_app_set_keyboard_leds);

		hci_power_control(HCI_POWER_ON);

		/* Deliberately no btstack_run_loop_execute(): with the
		 * threadsafe_background arch the run loop is driven by interrupts off the
		 * async_context, and calling execute() here would never return. */
		s_bt_up = 1;
	}
#endif

	/*
	 * Say what there is, including when there is nothing. A build with no input is
	 * legitimate, but it is also indistinguishable from one whose hardware failed
	 * to come up, so it has to announce itself.
	 */
	printf("picosdl: input ready -"
#if PSDL_HAVE_BT_KEYBOARD
	       " bluetooth"
#endif
#if PSDL_HAVE_JOYSTICK
	       " joystick"
#endif
#if PSDL_HAVE_GAMEPAD
	       " gamepad"
#endif
#if !PSDL_HAVE_BT_KEYBOARD && !PSDL_HAVE_JOYSTICK && !PSDL_HAVE_GAMEPAD
	       " no input devices built in, so nothing can be pressed"
#endif
	       "\n");
	console_help();
}


/*
 * The game controller, if one is attached.
 *
 * Readings go in as SDL controller axes and buttons, which is what makes the pad's
 * labels mean anything: a client using SDL's mapped controller API gets "button Y"
 * rather than "button 3". SDLPoP turns that into Y jumping, A crouching, X
 * grabbing and Start or Back opening the menu, with no mapping table anywhere.
 *
 * Polled on the stick's schedule. A full read is three I2C transfers, about
 * 1.03 ms at 400 kHz - see board.h.
 */
#if PSDL_HAVE_GAMEPAD
/*
 * A pad axis to SDL's range.
 *
 * 0..1023 around a nominal 512, scaled to -32768..32767. The rest position is near
 * but not exactly centre and the deadzone belongs to the client, so there is no
 * calibration here - only the scale and, if the stick is installed that way round,
 * the inversion.
 *
 * Clamped rather than just negated: a reading of 0 scales to -32768, whose negation
 * does not fit in a Sint16.
 */
static Sint16 pad_axis_to_sdl(uint16_t raw, bool invert)
{
	int v = ((int)raw - 512) * 64;
	if (invert)
		v = -v;
	if (v >  32767) v =  32767;
	if (v < -32768) v = -32768;
	return (Sint16)v;
}

static void poll_gamepad(void)
{
	if (!seesaw_gamepad_present())
		return;

	Uint32 now = psdl_backend_ticks_ms();
	if ((Uint32)(now - s_pad_poll_ms) < PSDL_JOY_POLL_INTERVAL_MS)
		return;
	s_pad_poll_ms = now;

	seesaw_gamepad_state_t g;
	if (!seesaw_gamepad_read(&g))
		return;        /* a dropped transfer: keep the last state rather than
		                * inventing a centred stick */

	s_pad_axis[0] = pad_axis_to_sdl(g.x, PSDL_BOARD_GAMEPAD_INVERT_X);
	s_pad_axis[1] = pad_axis_to_sdl(g.y, PSDL_BOARD_GAMEPAD_INVERT_Y);
	publish_left_stick();

	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_A,     g.a);
	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_B,     g.b);
	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_X,     g.x_btn);
	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_Y,     g.y_btn);
	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_BACK,  g.select);
	psdl_controller_set_button(SDL_CONTROLLER_BUTTON_START, g.start);
}

#endif /* PSDL_HAVE_GAMEPAD */

void psdl_backend_input_poll(void)
{
	/* Core 0, outside any interrupt: the one place the queued log can be printed.
	 * Drained before the early return so messages still come out when the input
	 * backend failed to start. */
	psdl_log_drain();

	if (!s_ready)
		return;

	/* Typed commands, whatever this build has. Core 0, outside any interrupt. */
	poll_console();

#if PSDL_HAVE_JOYSTICK
	poll_joystick();
#endif
#if PSDL_HAVE_GAMEPAD
	poll_gamepad();
#endif
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
#if PSDL_HAVE_BT_KEYBOARD
	return s_keys_seen > 0;
#else
	return false;
#endif
}

const char *psdl_pico_input_status(void)
{
	static char buf[48];
#if PSDL_HAVE_BT_KEYBOARD
	if (!s_bt_up)
		snprintf(buf, sizeof(buf), "BT FAILED TO START");
	else if (s_keys_seen == 0)
		snprintf(buf, sizeof(buf), "BT SEARCHING FOR KEYBOARD");
	else
		snprintf(buf, sizeof(buf), "KEYBOARD OK - %u KEYS", s_keys_seen);
#else
	snprintf(buf, sizeof(buf), "NO KEYBOARD IN THIS BUILD");
#endif
	return buf;
}
