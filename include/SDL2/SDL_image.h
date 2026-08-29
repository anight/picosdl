/*
 * A stub, on purpose.
 *
 * picosdl does not provide SDL_image and the game does not use it -
 * USE_SDL_IMAGE is off, and the sprites are const data in flash rather than
 * files to be loaded. But SDLPoP's types.h includes this header
 * unconditionally, outside the USE_SDL_IMAGE guard (types.h:31), so it has to
 * exist for anything that includes common.h to compile.
 *
 * Deleting that include is part of the game-side port (PLAN.md 3.1); until
 * then, this keeps the build honest without patching SDLPoP.
 */
#ifndef PICOSDL_SDL_IMAGE_H
#define PICOSDL_SDL_IMAGE_H

#include "SDL2/SDL.h"

#endif /* PICOSDL_SDL_IMAGE_H */
