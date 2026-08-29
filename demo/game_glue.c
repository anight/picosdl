/*
 * The minimum of SDLPoP the MIDI player needs, so the demo can play the game's
 * own music without dragging in the rest of the game.
 *
 * midi.c is used unmodified. It wants a resource lookup (for the instrument
 * bank and the tune itself), four globals from seg009.c, and quit(). All of
 * that is a few dozen lines, and doing it this way means what plays is the
 * real player driving the real Nuked OPL3 emulator over the real data - which
 * is the point, since the open question this answers is how much CPU that
 * costs on a Pico.
 *
 * The resource chain is the same design as seg009.c's: DAT files are pushed on
 * a list and a lookup walks it most-recent-first. Here it is a fixed array of
 * four instead of a malloc'd list, which is where the game's is heading anyway
 * (PLAN.md 3.4).
 */
#include <stdio.h>
#include <string.h>

/* SDLPoP's own headers, so every symbol defined here is checked against the
 * declaration the game (and midi.c) actually sees. data.h expands to plain
 * externs unless BODY is defined, so including it costs nothing and buys
 * type-checking on the six globals below. */
#include "common.h"
#include "resources.h"

#include "game_glue.h"

/* ------------------------------------------------------ resource chain -- */

#define MAX_OPEN_DATS 4

static dat_type  s_dats[MAX_OPEN_DATS];
static int       s_dats_used[MAX_OPEN_DATS];
static dat_type *s_dat_chain;

dat_type *open_dat(const char *filename, int optional)
{
	(void)optional;

	for (unsigned i = 0; i < datfiles_count; ++i) {
		if (strcmp(filename, datfiles[i].filename) != 0)
			continue;

		for (int slot = 0; slot < MAX_OPEN_DATS; ++slot) {
			if (s_dats_used[slot])
				continue;
			s_dats_used[slot]     = 1;
			s_dats[slot].df       = &datfiles[i];
			s_dats[slot].next_dat = s_dat_chain;
			s_dat_chain           = &s_dats[slot];
			return &s_dats[slot];
		}
		printf("game_glue: too many open DATs (raise MAX_OPEN_DATS)\n");
		return NULL;
	}
	return NULL;
}

void close_dat(dat_type *pointer)
{
	dat_type **prev = &s_dat_chain;
	for (dat_type *curr = s_dat_chain; curr != NULL; curr = curr->next_dat) {
		if (curr == pointer) {
			*prev = curr->next_dat;
			for (int slot = 0; slot < MAX_OPEN_DATS; ++slot)
				if (&s_dats[slot] == curr)
					s_dats_used[slot] = 0;
			return;
		}
		prev = &curr->next_dat;
	}
}

const char *load_from_opendats_const(int resource_id, int *out_size)
{
	for (dat_type *p = s_dat_chain; p != NULL; p = p->next_dat) {
		const struct datfile_s *df = p->df;
		for (unsigned i = 0; i < df->resources_count; ++i) {
			const struct resource_s *r = df->resources[i];
			if (r->resource_id != resource_id)
				continue;
			if (out_size)
				*out_size = (int)r->data_len;
			return (const char *)r->data;
		}
	}
	return NULL;
}

/* Convenience for the demo: find a resource in one named DAT without having to
 * open and close it around every use. */
const void *glue_find_resource(const char *datfile, int resource_id, int *out_size)
{
	for (unsigned i = 0; i < datfiles_count; ++i) {
		if (strcmp(datfile, datfiles[i].filename) != 0)
			continue;
		const struct datfile_s *df = &datfiles[i];
		for (unsigned j = 0; j < df->resources_count; ++j) {
			const struct resource_s *r = df->resources[j];
			if (r->resource_id != resource_id)
				continue;
			if (out_size)
				*out_size = (int)r->data_len;
			return r->data;
		}
	}
	return NULL;
}

/* ------------------------------------------------ globals midi.c reads -- */

/* seg009.c owns these in the real game; midi.c only reads them. */
short          midi_playing;
SDL_AudioSpec *digi_audiospec;
int            digi_unavailable;
word           current_sound = sound_54_intro_music;  /* picks the per-tune
                                                         tempo modifier */
byte           is_sound_on = 0x0F;
byte           enable_music = 1;

/* midi.c calls this before touching digi_audiospec. The demo opens the audio
 * device itself, so this only has to publish the spec. */
void init_digi(void)
{
}

void glue_set_audiospec(SDL_AudioSpec *spec)
{
	digi_audiospec   = spec;
	digi_unavailable = (spec == NULL);
}

/* midi.c calls this if parse_midi cannot allocate. There is nowhere to go. */
void quit(int exit_code)
{
	printf("game_glue: quit(%d)\n", exit_code);
	for (;;) { }
}

/* Type-safe wrapper, so the demo does not have to include types.h to name
 * sound_buffer_type. */
void glue_play_music(const void *resource)
{
	play_midi_sound((const sound_buffer_type *)resource);
}
