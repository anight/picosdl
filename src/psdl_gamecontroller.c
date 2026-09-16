/*
 * SDL_GameController.
 *
 * One controller, or none. The backend finds it and pushes readings in; this file
 * turns those into the SDL API and the SDL_CONTROLLER* events a game expects, and
 * is the whole of what the game sees.
 *
 * Portable on purpose: nothing here knows about I2C or seesaw. A backend with a
 * USB pad, or none at all, uses the same three feeders. The host backend calls
 * none of them, so SDL_IsGameController() is false there and a game falls back to
 * the joystick exactly as it did before this existed.
 *
 * Why a real controller rather than another joystick: SDL's controller API is
 * *mapped*, so a game gets named buttons instead of indices. SDLPoP uses that -
 * Y jumps, A crouches, X grabs, Start and Back open the menu - and a pad with a
 * labelled diamond then does what the labels say. Presented as a bare joystick it
 * would have six anonymous buttons and the game would ignore all of them.
 */
#include "psdl_internal.h"

/*
 * The handle. SDL's is opaque and the game only ever compares it against NULL and
 * passes it back, so one static instance is enough - and it must have non-zero
 * size to be addressable.
 */
struct SDL_GameController {
	int in_use;
};

static struct SDL_GameController s_controller;

static int    s_present;
static Sint16 s_axis[SDL_CONTROLLER_AXIS_MAX];
static Uint8  s_button[SDL_CONTROLLER_BUTTON_MAX];

/* ------------------------------------------------------- fed by the backend */

void psdl_controller_set_present(int present)
{
	present = present ? 1 : 0;
	if (present == s_present)
		return;
	s_present = present;

	if (!present) {
		/* Release everything on the way out, or a game holding a button when the
		 * pad is unplugged keeps holding it for ever. */
		for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
			if (s_button[i]) {
				s_button[i] = 0;
				psdl_push_controller_button(i, 0);
			}
		}
		for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
			if (s_axis[i]) {
				s_axis[i] = 0;
				psdl_push_controller_axis(i, 0);
			}
		}
		s_controller.in_use = 0;
	}
	psdl_push_controller_device(present);
}

void psdl_controller_set_axis(int axis, Sint16 value)
{
	if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
		return;
	if (s_axis[axis] == value)
		return;              /* only changes are events */
	s_axis[axis] = value;
	psdl_push_controller_axis(axis, value);
}

void psdl_controller_set_button(int button, int pressed)
{
	if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
		return;
	pressed = pressed ? 1 : 0;
	if (s_button[button] == pressed)
		return;
	s_button[button] = (Uint8)pressed;
	psdl_push_controller_button(button, pressed);
}

/* ------------------------------------------------------------------ the API */

SDL_bool SDL_IsGameController(int joystick_index)
{
	/* Index 0 is the controller when there is one. A game asks about 0, opens it,
	 * and otherwise falls back to the joystick - which is what should happen when
	 * no pad is attached. */
	return (joystick_index == 0 && s_present) ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController *SDL_GameControllerOpen(int joystick_index)
{
	if (joystick_index != 0 || !s_present)
		return NULL;
	s_controller.in_use = 1;
	return &s_controller;
}

void SDL_GameControllerClose(SDL_GameController *gamecontroller)
{
	if (gamecontroller != NULL)
		gamecontroller->in_use = 0;
}

/*
 * There is one controller, so any instance id refers to it.
 *
 * Worth being generous here rather than checking the id: SDLPoP reads
 * `event.cdevice.which` out of a *button* event to find the controller, which
 * happens to work only because the two structs put `which` in the same place.
 * Returning the one controller regardless keeps that harmless.
 */
SDL_GameController *SDL_GameControllerFromInstanceID(Sint32 joyid)
{
	(void)joyid;
	return s_present ? &s_controller : NULL;
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, int axis)
{
	if (gamecontroller == NULL || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
		return 0;
	return s_axis[axis];
}

Uint8 SDL_GameControllerGetButton(SDL_GameController *gamecontroller, int button)
{
	if (gamecontroller == NULL || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
		return 0;
	return s_button[button];
}

/*
 * No filesystem, so there is no mapping database to read - and none is needed:
 * the backend reports axes and buttons already in SDL's own terms, which is what
 * a mapping file exists to achieve. Returning -1 is SDL's "could not load", and
 * SDLPoP only calls this when it has been given a path.
 */
int SDL_GameControllerAddMappingsFromFile(const char *file)
{
	(void)file;
	return -1;
}

/* No force feedback on this hardware. SDL returns -1 for unsupported. */
int SDL_GameControllerRumble(SDL_GameController *gamecontroller,
                             Uint16 low, Uint16 high, Uint32 ms)
{
	(void)gamecontroller; (void)low; (void)high; (void)ms;
	return -1;
}
