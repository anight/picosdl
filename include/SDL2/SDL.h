/*
 * picosdl - a minimal SDL2 work-alike for the Raspberry Pi Pico.
 *
 * This is not SDL. It is the subset of the SDL2 API that SDLPoP actually calls,
 * reimplemented on top of a Pico: an ST7789 panel over PIO/DMA, an I2S DAC, a
 * Bluetooth keyboard and an analog stick.
 *
 * Two deliberate restrictions make it small enough to be worth having:
 *
 *   1. There is exactly one pixel format: 8bpp indexed against a single global
 *      256-entry palette. No conversion, no format negotiation, no generic
 *      blitter. The panel's PIO/DMA chain expands indices to RGB565 through a
 *      hardware CLUT, so the palette IS the display hardware - SDL_SetPaletteColors
 *      writes straight to it, which makes palette fades free.
 *
 *   2. Nothing allocates. Surfaces come from three fixed places (flash-resident
 *      read-only, named statics, and a LIFO arena); everything else is a static
 *      singleton or a fixed pool. There is no heap.
 *
 * Scancodes are USB HID usage codes, which is what SDL_Scancode already is, so
 * the Bluetooth keyboard's decoded usages index the key state array directly.
 */
#ifndef PICOSDL_SDL_H
#define PICOSDL_SDL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;

#define SDL_MAX_SINT32 0x7FFFFFFF

/* Byte order. The Pico is little-endian; so is every host we build for. */
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER  SDL_LIL_ENDIAN

#define SDL_SwapLE16(x) ((Uint16)(x))
#define SDL_SwapLE32(x) ((Uint32)(x))
#define SDL_SwapBE16(x) ((Uint16)__builtin_bswap16((Uint16)(x)))
#define SDL_SwapBE32(x) ((Uint32)__builtin_bswap32((Uint32)(x)))

#define SDL_COMPILE_TIME_ASSERT(name, x) typedef int SDL_dummy_##name[(x) * 2 - 1]

/* ------------------------------------------------------------ init flags */

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_HAPTIC         0x00001000u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_NOPARACHUTE    0x00100000u
#define SDL_INIT_EVERYTHING     0x0000f231u

int         SDL_Init(Uint32 flags);
int         SDL_InitSubSystem(Uint32 flags);
void        SDL_Quit(void);
const char *SDL_GetError(void);
void        SDL_SetError(const char *fmt, ...);
void        SDL_ClearError(void);

/* --------------------------------------------------------------- pixels */

typedef struct SDL_Color {
	Uint8 r, g, b, a;
} SDL_Color;

typedef struct SDL_Palette {
	int        ncolors;
	SDL_Color *colors;
	Uint32     version;
	int        refcount;
} SDL_Palette;

/* Only one format exists. The fields are here because callers read them. */
#define SDL_PIXELFORMAT_INDEX8   0x13000001u
#define SDL_PIXELFORMAT_RGB24    0x17101803u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u

#define SDL_ISPIXELFORMAT_INDEXED(fmt) ((fmt) == SDL_PIXELFORMAT_INDEX8)

typedef struct SDL_PixelFormat {
	Uint32       format;
	SDL_Palette *palette;
	Uint8        BitsPerPixel;
	Uint8        BytesPerPixel;
	Uint8        padding[2];
	Uint32       Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormat;

typedef struct SDL_Rect {
	int x, y, w, h;
} SDL_Rect;

/* Surface flags. The low bits mirror SDL's; the high ones are ours and record
 * which of the three fixed regions a surface came from, so SDL_FreeSurface can
 * dispatch without a general allocator behind it. */
#define SDL_SWSURFACE     0x00000000u
#define SDL_PREALLOC      0x00000001u
#define SDL_RLEACCEL      0x00000002u
#define SDL_DONTFREE      0x00000004u

/* Where a surface's *pixels* came from. */
#define PSDL_SURF_STATIC  0x01000000u  /* named static, never freed           */
#define PSDL_SURF_EXTERN  0x02000000u  /* owned by the caller, never freed    */
#define PSDL_SURF_ARENA   0x04000000u  /* from the LIFO arena, freed in order */
#define PSDL_SURF_REGION  0x07000000u

/*
 * The surface *object itself* is const and lives in flash - as the game's
 * build-time-converted sprites do. Every field is read-only: a refcount
 * decrement, a lock counter or a colour-key change would be a write to XIP and
 * would fault, so each entry point that mutates a surface checks this first.
 */
#define PSDL_SURF_CONST   0x08000000u

/* What a generated sprite carries. */
#define PSDL_SURF_FLASH   (PSDL_SURF_EXTERN | PSDL_SURF_CONST)

typedef struct SDL_Surface {
	Uint32           flags;
	SDL_PixelFormat *format;
	int              w, h;
	int              pitch;
	void            *pixels;
	void            *userdata;
	int              locked;
	SDL_Rect         clip_rect;
	int              refcount;

	/* private */
	Uint32           colorkey;
	SDL_bool         has_colorkey;
	Uint8            alpha_mod;
	int              blend_mode;
	int              pool_slot;   /* screen-buffer pool index + 1, else 0 */
} SDL_Surface;

/*
 * The one and only pixel format, shared by every surface.
 *
 * It is a single shared object rather than a copy per surface for a concrete
 * reason: the game's sprites are `const SDL_Surface` objects generated into
 * flash, 673 of them, and an embedded SDL_PixelFormat would add 28 bytes to
 * each. It is also simply true - there is one format here, and one palette.
 */
extern SDL_PixelFormat psdl_pixel_format;

typedef enum {
	SDL_BLENDMODE_NONE  = 0x00000000,
	SDL_BLENDMODE_BLEND = 0x00000001,
	SDL_BLENDMODE_ADD   = 0x00000002,
	SDL_BLENDMODE_MOD   = 0x00000004
} SDL_BlendMode;

#define SDL_ALPHA_OPAQUE      255
#define SDL_ALPHA_TRANSPARENT 0

/* Surfaces. Width/height are the only meaningful arguments to CreateRGBSurface;
 * depth and the masks are ignored, since everything is 8bpp indexed. */
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth,
                                      int pitch, Uint32 Rmask, Uint32 Gmask,
                                      Uint32 Bmask, Uint32 Amask);
void         SDL_FreeSurface(SDL_Surface *surface);
int          SDL_LockSurface(SDL_Surface *surface);
void         SDL_UnlockSurface(SDL_Surface *surface);
SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags);
SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 pixel_format, Uint32 flags);

int      SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);
int      SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode blendMode);
int      SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha);
SDL_bool SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect);
void     SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect);
int      SDL_SetSurfacePalette(SDL_Surface *surface, SDL_Palette *palette);

int      SDL_FillRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
int      SDL_UpperBlit(SDL_Surface *src, const SDL_Rect *srcrect,
                       SDL_Surface *dst, SDL_Rect *dstrect);
#define  SDL_BlitSurface SDL_UpperBlit
int      SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect,
                        SDL_Surface *dst, SDL_Rect *dstrect);

/* Extensions. These exist because the operations they name are the ones SDLPoP
 * wraps SDL in anyway, and doing them directly avoids a scratch surface and a
 * round trip through a generic blitter. */
int PSDL_BlitMirrored(SDL_Surface *src, const SDL_Rect *srcrect,
                      SDL_Surface *dst, SDL_Rect *dstrect);
int PSDL_BlitXor(SDL_Surface *src, const SDL_Rect *srcrect,
                 SDL_Surface *dst, SDL_Rect *dstrect);
/* Blit with a constant added to each non-transparent source index, for sprite
 * sets whose 16-colour palette lives at an offset in the global palette.
 * Build-time-converted sprites have the offset baked in and do not need this. */
int PSDL_BlitOffset(SDL_Surface *src, const SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect, Uint8 index_offset);

/*
 * Blit choosing transparency per call instead of per surface.
 *
 * SDLPoP switches transparency by calling SDL_SetColorKey on the sprite before
 * every blit (see method_1_blit_rect). That cannot work here: sprites are
 * `const` objects in flash and writing to one faults. They carry a baked
 * colour key - the transparent index for their palette row - and this chooses
 * whether to honour it. Pass 0 for what SDLPoP calls blitters_0_no_transp.
 */
int PSDL_BlitTransp(SDL_Surface *src, const SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect, int transparent);

/* Draw every non-transparent source pixel as `color`, ignoring the source
 * index. This is SDLPoP's method_3_blit_mono, which the built-in font and the
 * depth-1 images use to be drawn in an arbitrary colour. */
int PSDL_BlitMono(SDL_Surface *src, const SDL_Rect *srcrect,
                  SDL_Surface *dst, SDL_Rect *dstrect, Uint8 color);

/* -------------------------------------------------------------- palette */

/* There is one palette, shared by every surface, and it is the display CLUT.
 * SDL_SetPaletteColors on it updates the hardware. */
SDL_Palette *PSDL_GlobalPalette(void);

int    SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors,
                            int firstcolor, int ncolors);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
Uint32 SDL_MapRGBA(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void   SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat *format,
                  Uint8 *r, Uint8 *g, Uint8 *b);
const char *SDL_GetPixelFormatName(Uint32 format);

/* --------------------------------------------------------------- video */

typedef struct SDL_Window SDL_Window;

#define SDL_WINDOWPOS_UNDEFINED     0x1FFF0000u
#define SDL_WINDOWPOS_CENTERED      0x2FFF0000u
#define SDL_WINDOW_FULLSCREEN       0x00000001u
#define SDL_WINDOW_RESIZABLE        0x00000020u
#define SDL_WINDOW_ALLOW_HIGHDPI    0x00002000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP (SDL_WINDOW_FULLSCREEN | 0x00001000u)

SDL_Window  *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void         SDL_DestroyWindow(SDL_Window *window);
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
int          SDL_UpdateWindowSurface(SDL_Window *window);
void         SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
Uint32       SDL_GetWindowFlags(SDL_Window *window);
int          SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
void         SDL_SetWindowTitle(SDL_Window *window, const char *title);
void         SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);
int          SDL_ShowCursor(int toggle);

#define SDL_ENABLE  1
#define SDL_DISABLE 0
#define SDL_QUERY  -1

/* ----------------------------------------------------------- scancodes */

/* SDL_Scancode values are USB HID usage codes (page 0x07), which is exactly
 * what the Bluetooth keyboard hands us. No translation table. */
typedef enum {
	SDL_SCANCODE_UNKNOWN = 0,
	SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
	SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
	SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
	SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P,
	SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
	SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X,
	SDL_SCANCODE_Y, SDL_SCANCODE_Z,
	SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
	SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
	SDL_SCANCODE_9, SDL_SCANCODE_0,
	SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE,
	SDL_SCANCODE_TAB, SDL_SCANCODE_SPACE,
	SDL_SCANCODE_MINUS = 45, SDL_SCANCODE_EQUALS, SDL_SCANCODE_LEFTBRACKET,
	SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_NONUSHASH,
	SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE,
	SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH,
	SDL_SCANCODE_CAPSLOCK = 57,
	SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
	SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
	SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
	SDL_SCANCODE_PRINTSCREEN = 70, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_PAUSE,
	SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME, SDL_SCANCODE_PAGEUP,
	SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
	SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
	SDL_SCANCODE_NUMLOCKCLEAR = 83,
	SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_MINUS,
	SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_ENTER,
	SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4,
	SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8,
	SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD,
	SDL_SCANCODE_NONUSBACKSLASH = 100,
	SDL_SCANCODE_APPLICATION = 101,
	SDL_SCANCODE_CLEAR = 156,
	SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT,
	SDL_SCANCODE_LGUI, SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT,
	SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI,
	SDL_SCANCODE_MUTE = 262, SDL_SCANCODE_VOLUMEUP, SDL_SCANCODE_VOLUMEDOWN,
	SDL_SCANCODE_AUDIOMUTE = 262,
	SDL_NUM_SCANCODES = 288
} SDL_Scancode;

typedef Sint32 SDL_Keycode;

typedef struct SDL_Keysym {
	SDL_Scancode scancode;
	SDL_Keycode  sym;
	Uint16       mod;
	Uint32       unused;
} SDL_Keysym;

#define KMOD_NONE   0x0000
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_LGUI   0x0400
#define KMOD_RGUI   0x0800
#define KMOD_CTRL   (KMOD_LCTRL  | KMOD_RCTRL)
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_ALT    (KMOD_LALT   | KMOD_RALT)
#define KMOD_GUI    (KMOD_LGUI   | KMOD_RGUI)

const Uint8 *SDL_GetKeyboardState(int *numkeys);
const char  *SDL_GetScancodeName(SDL_Scancode scancode);
Uint16       SDL_GetModState(void);

/* --------------------------------------------------------------- events */

typedef enum {
	SDL_FIRSTEVENT = 0,
	SDL_QUIT = 0x100,
	SDL_WINDOWEVENT = 0x200,
	SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTEDITING, SDL_TEXTINPUT,
	SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL,
	SDL_JOYAXISMOTION = 0x600, SDL_JOYBALLMOTION, SDL_JOYHATMOTION,
	SDL_JOYBUTTONDOWN, SDL_JOYBUTTONUP, SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED,
	SDL_CONTROLLERAXISMOTION = 0x650, SDL_CONTROLLERBUTTONDOWN,
	SDL_CONTROLLERBUTTONUP, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED,
	SDL_CONTROLLERDEVICEREMAPPED,
	SDL_USEREVENT = 0x8000,
	SDL_LASTEVENT = 0xFFFF
} SDL_EventType;

#define SDL_WINDOWEVENT_EXPOSED      3
#define SDL_WINDOWEVENT_MOVED        4
#define SDL_WINDOWEVENT_SIZE_CHANGED 6
#define SDL_WINDOWEVENT_MINIMIZED    7
#define SDL_WINDOWEVENT_RESTORED     9
#define SDL_WINDOWEVENT_FOCUS_GAINED 12

#define SDL_PRESSED  1
#define SDL_RELEASED 0

typedef struct SDL_CommonEvent   { Uint32 type; Uint32 timestamp; } SDL_CommonEvent;
typedef struct SDL_KeyboardEvent {
	Uint32 type; Uint32 timestamp; Uint32 windowID;
	Uint8 state; Uint8 repeat; Uint8 padding2, padding3;
	SDL_Keysym keysym;
} SDL_KeyboardEvent;
typedef struct SDL_JoyAxisEvent {
	Uint32 type; Uint32 timestamp; Sint32 which;
	Uint8 axis; Uint8 padding1, padding2, padding3;
	Sint16 value; Uint16 padding4;
} SDL_JoyAxisEvent;
typedef struct SDL_JoyButtonEvent {
	Uint32 type; Uint32 timestamp; Sint32 which;
	Uint8 button; Uint8 state; Uint8 padding1, padding2;
} SDL_JoyButtonEvent;
typedef struct SDL_WindowEvent {
	Uint32 type; Uint32 timestamp; Uint32 windowID;
	Uint8 event; Uint8 padding1, padding2, padding3;
	Sint32 data1, data2;
} SDL_WindowEvent;
typedef struct SDL_UserEvent {
	Uint32 type; Uint32 timestamp; Uint32 windowID;
	Sint32 code; void *data1; void *data2;
} SDL_UserEvent;
typedef struct SDL_QuitEvent { Uint32 type; Uint32 timestamp; } SDL_QuitEvent;

typedef union SDL_Event {
	Uint32            type;
	SDL_CommonEvent   common;
	SDL_WindowEvent   window;
	SDL_KeyboardEvent key;
	SDL_JoyAxisEvent  jaxis;
	SDL_JoyButtonEvent jbutton;
	SDL_UserEvent     user;
	SDL_QuitEvent     quit;
	Uint8             padding[56];
} SDL_Event;

int  SDL_PollEvent(SDL_Event *event);
int  SDL_PushEvent(SDL_Event *event);
void SDL_PumpEvents(void);
void SDL_FlushEvent(Uint32 type);

/* ------------------------------------------------------------ joystick */

typedef struct SDL_Joystick SDL_Joystick;

/* Declared but never defined: the game has pointer members of these types
 * (data.h:638, :646) and passes them to functions that do nothing here.
 * There is no game controller and no force feedback on this hardware. */
typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_Haptic SDL_Haptic;

/* Note: SDL_JOYSTICK_X_AXIS, SDL_JOYSTICK_Y_AXIS, SDL_JOYSTICK_BUTTON_X and
 * SDL_JOYSTICK_BUTTON_Y are *not* SDL API despite the spelling - they are
 * SDLPoP's own control mapping, defined in its config.h:383-386. They do not
 * belong here. */

int           SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int device_index);
void          SDL_JoystickClose(SDL_Joystick *joystick);
Sint16        SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);
Uint8         SDL_JoystickGetButton(SDL_Joystick *joystick, int button);
int           SDL_JoystickRumble(SDL_Joystick *joystick, Uint16 low, Uint16 high, Uint32 ms);
SDL_bool      SDL_IsGameController(int joystick_index);

/* --------------------------------------------------------------- timer */

Uint32 SDL_GetTicks(void);
void   SDL_Delay(Uint32 ms);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);

typedef int SDL_TimerID;
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *param);
SDL_bool    SDL_RemoveTimer(SDL_TimerID id);

/* --------------------------------------------------------------- audio */

typedef Uint16 SDL_AudioFormat;

#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_S16LSB 0x8010
#define AUDIO_S16SYS AUDIO_S16LSB
#define AUDIO_S16    AUDIO_S16LSB

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);

typedef struct SDL_AudioSpec {
	int               freq;
	SDL_AudioFormat   format;
	Uint8             channels;
	Uint8             silence;
	Uint16            samples;
	Uint16            padding;
	Uint32            size;
	SDL_AudioCallback callback;
	void             *userdata;
} SDL_AudioSpec;

typedef enum {
	SDL_AUDIO_STOPPED = 0,
	SDL_AUDIO_PLAYING,
	SDL_AUDIO_PAUSED
} SDL_AudioStatus;

/*
 * Master volume, applied after everything has been mixed and just before the
 * samples reach the DAC - so it covers music, effects and anything else at
 * once. PSDL_VOLUME_UNITY is 1:1; below that attenuates.
 *
 * Not an SDL API. SDL has no master volume, and this one exists because the
 * hardware is loud enough that debugging at full scale is genuinely unpleasant.
 */
#define PSDL_VOLUME_SHIFT 8
#define PSDL_VOLUME_UNITY (1 << PSDL_VOLUME_SHIFT)

void PSDL_SetMasterVolume(int volume);
int  PSDL_GetMasterVolume(void);

int             SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void            SDL_CloseAudio(void);
void            SDL_PauseAudio(int pause_on);
void            SDL_LockAudio(void);
void            SDL_UnlockAudio(void);
SDL_AudioStatus SDL_GetAudioStatus(void);

/* --------------------------------------------------------------- RWops */

typedef struct SDL_RWops SDL_RWops;
struct SDL_RWops {
	Sint64 (*size)(SDL_RWops *ctx);
	Sint64 (*seek)(SDL_RWops *ctx, Sint64 offset, int whence);
	size_t (*read)(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum);
	size_t (*write)(SDL_RWops *ctx, const void *ptr, size_t size, size_t num);
	int    (*close)(SDL_RWops *ctx);
	Uint32 type;
	struct { Uint8 *base, *here, *stop; } mem;
};

#define RW_SEEK_SET 0
#define RW_SEEK_CUR 1
#define RW_SEEK_END 2

SDL_RWops *SDL_RWFromMem(void *mem, int size);
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size);
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
#define    SDL_RWsize(ctx)  (ctx)->size(ctx)
#define    SDL_RWseek(ctx, offset, whence) (ctx)->seek(ctx, offset, whence)
#define    SDL_RWtell(ctx)  (ctx)->seek(ctx, 0, RW_SEEK_CUR)
#define    SDL_RWread(ctx, ptr, size, n)  (ctx)->read(ctx, ptr, size, n)
#define    SDL_RWwrite(ctx, ptr, size, n) (ctx)->write(ctx, ptr, size, n)
#define    SDL_RWclose(ctx) (ctx)->close(ctx)

/* ---------------------------------------------------------- diagnostics */

/* Print surface-pool and arena occupancy, including peaks. This is how the
 * arena gets sized: run the real workload, read the high-water mark. */
void     PSDL_ReportMemory(void);
/* Events lost to a full queue. Should stay at zero; anything else means the
 * consumer is not polling often enough. */
unsigned PSDL_DroppedEvents(void);

/* --------------------------------------------------------------- stubs */

/* Present so callers compile unchanged; they do nothing on a Pico. */
#define SDL_HINT_RENDER_SCALE_QUALITY          "SDL_RENDER_SCALE_QUALITY"
#define SDL_HINT_RENDER_VSYNC                  "SDL_RENDER_VSYNC"
#define SDL_HINT_IME_SHOW_UI                   "SDL_IME_SHOW_UI"
#define SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING "SDL_WINDOWS_DISABLE_THREAD_NAMING"
#define SDL_MESSAGEBOX_ERROR 0x00000010

SDL_bool SDL_SetHint(const char *name, const char *value);
void     SDL_StartTextInput(void);
void     SDL_StopTextInput(void);
void     SDL_SetTextInputRect(SDL_Rect *rect);
int      SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                                  const char *message, SDL_Window *window);
void     SDL_free(void *mem);

typedef struct SDL_version { Uint8 major, minor, patch; } SDL_version;
void SDL_GetVersion(SDL_version *ver);
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    0
#define SDL_VERSION(x) do { (x)->major = 2; (x)->minor = 0; (x)->patch = 0; } while (0)
#define SDL_VERSION_ATLEAST(X, Y, Z) \
	((SDL_MAJOR_VERSION >= (X)) && (SDL_MINOR_VERSION >= (Y)) && (SDL_PATCHLEVEL >= (Z)))

#ifdef __cplusplus
}
#endif

#endif /* PICOSDL_SDL_H */
