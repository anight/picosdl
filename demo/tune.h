/*
 * The demo's own music: an original tune over three integer voices, with no
 * dependency on any game's data and no use of libm. See tune.c.
 */
#ifndef PICOPOP_DEMO_TUNE_H
#define PICOPOP_DEMO_TUNE_H

#include "SDL2/SDL.h"

#define TUNE_VOICES 3

void        tune_init(int sample_rate);
void        tune_start(void);
void        tune_stop(void);
int         tune_playing(void);
const char *tune_name(void);

/* Add one block of the tune into a mono accumulator, `frames` entries long. The
 * caller sums its own voices in and clamps once. */
void        tune_render(Sint32 *mix_mono, int frames);

#endif /* PICOPOP_DEMO_TUNE_H */
