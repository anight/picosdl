/*
 * What the demo needs from game_glue.c to play the game's music.
 *
 * midi.c's own entry points (play_midi_sound, midi_callback, stop_midi) are
 * declared in SDLPoP's proto.h; these are the extra hooks the glue adds.
 */
#ifndef PICOSDL_DEMO_GAME_GLUE_H
#define PICOSDL_DEMO_GAME_GLUE_H

#include "SDL2/SDL.h"

/* Find a resource in one named DAT, without opening and closing it. */
const void *glue_find_resource(const char *datfile, int resource_id, int *out_size);

/* Publish the audio spec midi.c reads its mixing rate from. Call once, after
 * SDL_OpenAudio, before playing anything. */
void glue_set_audiospec(SDL_AudioSpec *spec);

/* Start one of the tunes below. The argument is the resource bytes as they sit
 * in flash; midi.c reads the Adlib data straight out of them. Wraps
 * play_midi_sound so callers need not name sound_buffer_type. */
void glue_play_music(const void *resource);

/*
 * Music resource ids. The game numbers resources as sound id + 10000, and the
 * sound ids are the enum in types.h:747-763. All of these live in MIDISND2.DAT.
 */
#define GLUE_SOUND_STORY_2_PRINCESS 10050  /* sound_50_story_2_princess     */
#define GLUE_SOUND_STORY_4_JAFFAR   10052  /* sound_52_story_4_Jaffar_leaves */
#define GLUE_SOUND_STORY_3_JAFFAR   10053  /* sound_53_story_3_Jaffar_comes */
#define GLUE_SOUND_MAIN_THEME       10054  /* sound_54_intro_music - the title screen */
#define GLUE_SOUND_STORY_1_ABSENCE  10055  /* sound_55_story_1_absence      */
#define GLUE_SOUND_ENDING_MUSIC     10056  /* sound_56_ending_music         */

#endif /* PICOSDL_DEMO_GAME_GLUE_H */
