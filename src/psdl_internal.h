/*
 * picosdl internals: the tunables, the backend interface, and the few things
 * the modules share with each other.
 *
 * "Backend" here means the platform half - the thing that owns a panel, a DAC
 * and an input device. picosdl proper never talks to hardware; it draws into a
 * framebuffer and calls these.
 */
#ifndef PICOSDL_INTERNAL_H
#define PICOSDL_INTERNAL_H

#include "SDL2/SDL.h"

/* --------------------------------------------------------------- tunables */

/* The game's canvas. VGA mode 13h minus the borders, which is what Prince of
 * Persia draws into. The panel is 320x240, so this is letterboxed by the video
 * backend rather than scaled. */
#ifndef PSDL_SCREEN_W
#define PSDL_SCREEN_W 320
#endif
#ifndef PSDL_SCREEN_H
#define PSDL_SCREEN_H 200
#endif

/*
 * The LIFO arena that backs peel surfaces (see PLAN.md section 6f). Peels are
 * created and restored in strict stack order, so a bump pointer is enough and
 * fragmentation is structurally impossible.
 *
 * 16 KB, against a 6,700-byte peak measured over 400,000 presents of one client
 * covering its title screen, attract demo and gameplay. It was 28 KB, which was
 * a guess that happened to be comfortable. Paths that wait for a human are not
 * in that measurement - a name-entry screen that holds a peel open while the
 * player types pins everything allocated above it - so the margin is there for
 * those, not for growth.
 *
 * When this runs out, resist raising it until you have read PSDL_DumpArena().
 * The arena has been exhausted twice, once by a 16.5 KB surface nothing read and
 * once by a leak, and on both occasions the total looked like ordinary demand.
 * The exhaustion message names the allocation that happened to be last, never
 * the one at fault; the dump names both.
 */
#ifndef PSDL_ARENA_BYTES
#define PSDL_ARENA_BYTES (16 * 1024)
#endif

/*
 * Every surface that exists at once: the screen buffers, whatever the caller
 * creates, and one header per live arena surface.
 *
 * The game's demand, counted rather than guessed:
 *
 *   2    the window surface and the offscreen buffer
 *   100  the built-in font, one per glyph (0x20..0x83). These are permanent
 *        and their pixels are static, but each still needs a header.
 *   ~50  peels, capped by add_peel()
 *   few  dialogs and transient blits
 *
 * 96 was sized before the font went through picosdl and is two short of the
 * font alone, which showed up as an immediate panic at start-up rather than
 * anything subtle. 192 covers the above with room to spare, at about 68 bytes
 * a header - 13 KB, against 520 KB of SRAM.
 */
#ifndef PSDL_MAX_SURFACES
#define PSDL_MAX_SURFACES 192
#endif

/* Full-screen buffers, which get their own pool rather than the arena. SDLPoP
 * wants two: the window surface and the offscreen buffer it draws into. */
#ifndef PSDL_SCREEN_BUFFERS
#define PSDL_SCREEN_BUFFERS 2
#endif

#ifndef PSDL_EVENT_QUEUE_LEN
#define PSDL_EVENT_QUEUE_LEN 64
#endif

#ifndef PSDL_MAX_TIMERS
#define PSDL_MAX_TIMERS 4
#endif

/* Audio staging: one callback's worth of signed 16-bit stereo frames. */
#ifndef PSDL_AUDIO_BLOCK_FRAMES
#define PSDL_AUDIO_BLOCK_FRAMES 256
#endif

/*
 * Master volume at startup: a tenth of PSDL_VOLUME_UNITY.
 *
 * Not unity on purpose. A MAX98357A driving a small speaker is loud, and
 * anything wrong in the mixer is loud *at full scale* - which is unpleasant and
 * makes a fault hard to work on. A tenth is enough to hear what is happening
 * without hurting; it was a quarter, which was not quiet enough in practice.
 * Raise it with PSDL_SetMasterVolume() once the audio is behaving.
 *
 * Written as a fraction of unity rather than as 25 so it stays a tenth if
 * PSDL_VOLUME_SHIFT ever changes. Integer division makes it 25/256, i.e. 9.8%.
 */
#ifndef PSDL_DEFAULT_VOLUME
#define PSDL_DEFAULT_VOLUME (PSDL_VOLUME_UNITY / 10)
#endif

/* ---------------------------------------------------------------- panics */

/* Nothing here allocates, so the failure mode for "ran out" is a bug, not a
 * condition to recover from. Say so loudly rather than returning NULL into
 * code that will not check it. */
void psdl_panic(const char *fmt, ...) __attribute__((noreturn));

#define PSDL_ASSERT(cond, msg) \
	do { if (!(cond)) psdl_panic("picosdl: %s (%s:%d)", msg, __FILE__, __LINE__); } while (0)

/* ---------------------------------------------------- module entry points */

void         psdl_surface_init(void);
SDL_Surface *psdl_surface_alloc_header(void);

/* Wrap a framebuffer that lives in .bss and outlives everything - no pool slot, no
 * arena, nothing to free. A backend uses it for buffers it owns itself. */
SDL_Surface *psdl_surface_wrap_static(void *pixels, int w, int h, int pitch);
void         psdl_surface_free_header(SDL_Surface *s);

void         psdl_palette_init(void);
void         psdl_video_init(void);
void         psdl_events_init(void);
void         psdl_timer_init(void);
void         psdl_audio_init(void);

/* The framebuffer the game draws into, for the video backend to push. */
SDL_Surface *psdl_screen_surface(void);

/* Called by input backends. The push_* functions queue an event; the
 * joystick_set_* ones update the state SDL_JoystickGetAxis/Button reports. */
void psdl_push_key(SDL_Scancode scancode, int pressed, Uint16 mod);
void psdl_push_joy_axis(int axis, Sint16 value);
void psdl_push_controller_axis(int axis, Sint16 value);
void psdl_push_controller_button(int button, int pressed);
void psdl_push_controller_device(int added);
void psdl_push_joy_button(int button, int pressed);
void psdl_push_quit(void);
void psdl_joystick_set_axis(int axis, Sint16 value);

/* Whether this build has an analog joystick at all. Together with the controller's
 * presence it is what SDL_NumJoysticks() reports, which is how a client discovers
 * there is nothing to read. */
void psdl_joystick_set_present(int present);
int  psdl_joystick_present(void);

/* Game controller state, fed by whichever backend has one. Presence drives
 * SDL_IsGameController(); the axis and button setters only emit an event when the
 * value actually changes, so a backend may call them every poll. */
void psdl_controller_set_present(int present);
int  psdl_controller_present(void);
void psdl_controller_set_axis(int axis, Sint16 value);
void psdl_controller_set_button(int button, int pressed);
void psdl_joystick_set_button(int button, int pressed);

/* Called by the audio backend from whichever core runs the mixer: fills buf
 * with `frames` stereo int16 frames by invoking the client's SDL callback.
 * Writes silence if no callback is open or audio is paused. */
void psdl_audio_render(Sint16 *buf, int frames);

/* Handles the keyboard's volume/mute keys, standing in for the window manager
 * that would take them on a desktop. Non-zero means the key was consumed and no
 * event should be queued for it. */
int  psdl_audio_volume_key(SDL_Scancode scancode, int pressed);

/* ------------------------------------------------------ backend interface */

/* Video. present() is asynchronous on hardware; sync() waits for the previous
 * one to land. The split lets a caller overlap drawing with the panel push. */
/* Console output that is safe from any core and from an interrupt. On hardware
 * printf is not: the SDK serialises stdout with a mutex, which an exception
 * handler cannot take. Backends that have no such problem may just print. */
void psdl_backend_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void psdl_backend_video_init(int w, int h);
void psdl_backend_video_present(const Uint8 *pixels, int w, int h, int pitch);
void psdl_backend_video_sync(void);
void psdl_backend_palette_set(int first, int ncolors, const SDL_Color *colors);

/* Input. poll() is called from SDL_PumpEvents and should push whatever it has. */
void psdl_backend_input_init(void);
void psdl_backend_input_poll(void);

/* Audio. The backend pulls via psdl_audio_render(). The lock is held across the
 * client callback and is what SDL_LockAudio maps to, so it has to work across
 * cores - the mixer runs on core 1. */
void psdl_backend_audio_open(int freq, int channels, int block_frames);
void psdl_backend_audio_close(void);
void psdl_backend_audio_pause(int pause_on);
void psdl_backend_audio_lock(void);
void psdl_backend_audio_unlock(void);

/* Time. */
Uint32 psdl_backend_ticks_ms(void);
Uint64 psdl_backend_ticks_us(void);
void   psdl_backend_delay_ms(Uint32 ms);

#endif /* PICOSDL_INTERNAL_H */
