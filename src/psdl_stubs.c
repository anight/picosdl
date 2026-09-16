/*
 * The parts of SDL that have no meaning on a microcontroller with one panel and
 * no window manager. They exist so callers compile unchanged.
 *
 * The joystick here is the analog stick, presented through SDL's joystick API.
 * Its state is fed by the input backend; the game can either read the axes
 * directly or take the key events the backend synthesises from them.
 */
#include <stdio.h>

#include "psdl_internal.h"

/* ------------------------------------------------------------- joystick */

struct SDL_Joystick { int opened; };
static struct SDL_Joystick s_joystick;

static Sint16 s_axes[2];
static Uint8  s_buttons[4];

void psdl_joystick_set_axis(int axis, Sint16 value)
{
	if (axis >= 0 && axis < 2)
		s_axes[axis] = value;
}

void psdl_joystick_set_button(int button, int pressed)
{
	if (button >= 0 && button < 4)
		s_buttons[button] = pressed ? 1 : 0;
}

int SDL_NumJoysticks(void)
{
	return 1;
}

SDL_Joystick *SDL_JoystickOpen(int device_index)
{
	if (device_index != 0)
		return NULL;
	s_joystick.opened = 1;
	return &s_joystick;
}

void SDL_JoystickClose(SDL_Joystick *joystick)
{
	if (joystick)
		joystick->opened = 0;
}

Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis)
{
	(void)joystick;
	return (axis >= 0 && axis < 2) ? s_axes[axis] : 0;
}

Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button)
{
	(void)joystick;
	return (button >= 0 && button < 4) ? s_buttons[button] : 0;
}

int SDL_JoystickRumble(SDL_Joystick *joystick, Uint16 low, Uint16 high, Uint32 ms)
{
	(void)joystick; (void)low; (void)high; (void)ms;
	return -1;   /* no haptics */
}

/*
 * Game controller: implemented, not stubbed - see psdl_gamecontroller.c. The
 * analog stick stays a joystick, and the two coexist: a game that finds a
 * controller uses it, one that does not falls back to the calls above.
 *
 * Haptics remain inert. There is nothing on this hardware to shake.
 */
SDL_Haptic *SDL_HapticOpen(int device_index)
{
	(void)device_index;
	return NULL;
}

void SDL_HapticClose(SDL_Haptic *haptic)
{
	(void)haptic;
}

int SDL_HapticRumbleInit(SDL_Haptic *haptic)
{
	(void)haptic;
	return -1;
}

int SDL_HapticRumblePlay(SDL_Haptic *haptic, float strength, Uint32 length)
{
	(void)haptic; (void)strength; (void)length;
	return -1;
}

/* ---------------------------------------------------------------- misc */

int SDL_ShowCursor(int toggle)
{
	(void)toggle;
	return SDL_DISABLE;   /* there is no cursor */
}

void SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
	(void)window; (void)title;
}

void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
	(void)window; (void)icon;
}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
	(void)name; (void)value;
	return SDL_TRUE;   /* every hint here concerns a renderer we do not have */
}

void SDL_StartTextInput(void) { }
void SDL_StopTextInput(void)  { }
void SDL_SetTextInputRect(SDL_Rect *rect) { (void)rect; }

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                             const char *message, SDL_Window *window)
{
	(void)flags; (void)window;
	printf("[%s] %s\n", title ? title : "message", message ? message : "");
	return 0;
}

/* Nothing in picosdl ever hands out memory the caller must release, so this is
 * a no-op rather than a free() - calling free() on a pointer into flash or into
 * the arena would be far worse than doing nothing. */
void SDL_free(void *mem)
{
	(void)mem;
}

void SDL_GetVersion(SDL_version *ver)
{
	if (ver != NULL) {
		ver->major = SDL_MAJOR_VERSION;
		ver->minor = SDL_MINOR_VERSION;
		ver->patch = SDL_PATCHLEVEL;
	}
}

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
	static char buf[16];

	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
		buf[0] = (char)('A' + (scancode - SDL_SCANCODE_A));
		buf[1] = '\0';
		return buf;
	}
	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
		buf[0] = (char)('1' + (scancode - SDL_SCANCODE_1));
		buf[1] = '\0';
		return buf;
	}
	switch (scancode) {
	case SDL_SCANCODE_0:         return "0";
	case SDL_SCANCODE_RETURN:    return "Return";
	case SDL_SCANCODE_ESCAPE:    return "Escape";
	case SDL_SCANCODE_BACKSPACE: return "Backspace";
	case SDL_SCANCODE_TAB:       return "Tab";
	case SDL_SCANCODE_SPACE:     return "Space";
	case SDL_SCANCODE_UP:        return "Up";
	case SDL_SCANCODE_DOWN:      return "Down";
	case SDL_SCANCODE_LEFT:      return "Left";
	case SDL_SCANCODE_RIGHT:     return "Right";
	case SDL_SCANCODE_LSHIFT:    return "Left Shift";
	case SDL_SCANCODE_RSHIFT:    return "Right Shift";
	case SDL_SCANCODE_LCTRL:     return "Left Ctrl";
	case SDL_SCANCODE_RCTRL:     return "Right Ctrl";
	case SDL_SCANCODE_LALT:      return "Left Alt";
	case SDL_SCANCODE_RALT:      return "Right Alt";
	case SDL_SCANCODE_HOME:      return "Home";
	case SDL_SCANCODE_END:       return "End";
	case SDL_SCANCODE_PAGEUP:    return "PageUp";
	case SDL_SCANCODE_PAGEDOWN:  return "PageDown";
	default:
		snprintf(buf, sizeof(buf), "0x%02X", (unsigned)scancode);
		return buf;
	}
}
