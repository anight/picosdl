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

/*
 * Two formats exist here, and PSDL_COLOR_DEPTH picks which one a build uses:
 * INDEX8 at depth 8, RGB565 at depth 16. The others are declared because callers
 * name them; nothing in this library produces or consumes them.
 *
 * The values are SDL2's own, so a client that compares against its own headers or
 * prints them gets what it expects.
 */
#define SDL_PIXELFORMAT_INDEX8   0x13000001u
#define SDL_PIXELFORMAT_RGB565   0x15151002u
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
} SDL_Surface;

/*
 * The pixel formats, as shared objects rather than a copy per surface.
 *
 * Shared for a concrete reason: a client's sprites are `const SDL_Surface`
 * objects generated into flash - hundreds of them - and an embedded
 * SDL_PixelFormat would add 28 bytes to each.
 *
 * `psdl_pixel_format` is this build's format, which is what PSDL_COLOR_DEPTH
 * selects and what SDL_CreateRGBSurface() and the window surface use.
 *
 * `psdl_pixel_format_index8` is always 8bpp indexed, whatever the build depth.
 * At 8bpp it is the same object. At 16bpp it exists because the blitters and the
 * font are defined on palette indices and remain useful on their own terms - the
 * status bands draw through them into an 8bpp buffer and expand it on the way to
 * the panel. A surface has to be able to say which of the two it is rather than
 * inherit an assumption.
 */
extern SDL_PixelFormat psdl_pixel_format;
extern SDL_PixelFormat psdl_pixel_format_index8;

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

/*
 * Create the window over a framebuffer the caller owns.
 *
 * picosdl allocates no pixels anywhere, so there is no SDL_CreateWindow() here:
 * its signature has nowhere to put the memory, and a library that answered it
 * would have to keep a full-screen buffer of its own for every client, whether or
 * not that client already had one.
 *
 * `pixels` is the canvas. SDL_GetWindowSurface() returns a surface wrapping it,
 * which the blitters and SDL_FillRect() can target, and SDL_UpdateWindowSurface()
 * pushes it to the panel. `pitch` is in bytes.
 *
 * The buffer must be in this build's pixel format - 8bpp indices, or RGB565 at
 * PSDL_COLOR_DEPTH=16 - and outlive the window. One window at a time; a second
 * call returns the same one.
 *
 * A client that wants no surface at all can skip this entirely and call
 * PSDL_PresentBuffer().
 */
SDL_Window  *PSDL_CreateWindow(void *pixels, int w, int h, int pitch);
void         SDL_DestroyWindow(SDL_Window *window);
SDL_Surface *SDL_GetWindowSurface(SDL_Window *window);
int          SDL_UpdateWindowSurface(SDL_Window *window);
/* Push only these rectangles. The panel keeps what it was last sent, so whatever
 * is left out still shows the previous frame. NULL/0 means the whole surface. */
int          SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects,
                                          int numrects);
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
	/* SDL2's own values. MUTE and AUDIOMUTE are genuinely two different
	 * scancodes there - 127 is the HID keyboard-page Mute key, 262 is the
	 * consumer-page one - and the game switches on both in the same statement,
	 * so collapsing them to one value is a duplicate-case error. */
	SDL_SCANCODE_MUTE = 127, SDL_SCANCODE_VOLUMEUP = 128,
	SDL_SCANCODE_VOLUMEDOWN = 129,
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

/*
 * Game-controller and text-input events. The controller ones are real when a
 * backend has found a pad - see psdl_gamecontroller.c. Text input is not produced
 * by anything, but the game switches on it, so the members have to exist.
 */
typedef struct SDL_ControllerAxisEvent {
	Uint32 type; Uint32 timestamp; Sint32 which;
	Uint8 axis; Uint8 padding1, padding2, padding3;
	Sint16 value; Uint16 padding4;
} SDL_ControllerAxisEvent;
typedef struct SDL_ControllerButtonEvent {
	Uint32 type; Uint32 timestamp; Sint32 which;
	Uint8 button; Uint8 state; Uint8 padding1, padding2;
} SDL_ControllerButtonEvent;
typedef struct SDL_ControllerDeviceEvent {
	Uint32 type; Uint32 timestamp; Sint32 which;
} SDL_ControllerDeviceEvent;
typedef struct SDL_TextInputEvent {
	Uint32 type; Uint32 timestamp; Uint32 windowID;
	char text[32];
} SDL_TextInputEvent;

typedef union SDL_Event {
	Uint32            type;
	SDL_CommonEvent   common;
	SDL_WindowEvent   window;
	SDL_KeyboardEvent key;
	SDL_JoyAxisEvent  jaxis;
	SDL_JoyButtonEvent jbutton;
	SDL_ControllerAxisEvent   caxis;
	SDL_ControllerButtonEvent cbutton;
	SDL_ControllerDeviceEvent cdevice;
	SDL_TextInputEvent text;
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

/* SDL_GameController is real and defined in psdl_gamecontroller.c when a backend
 * finds a pad. SDL_Haptic is declared and never defined: there is no force
 * feedback on this hardware, and the game only holds a pointer to one. */
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

/*
 * Game controller and haptics.
 *
 * These exist so the game compiles and links unmodified; none of them do
 * anything for haptics. SDL_IsGameController() reports a real pad when one is
 * attached; with none it returns false, so a game never opens a
 * controller and falls through to its joystick path, which is what the board's
 * analog stick actually is. The enumerators are SDL's own values, in SDL's
 * order, so that a future mapping layer can be dropped in without touching the
 * game's switch statements.
 */
typedef enum {
	SDL_CONTROLLER_AXIS_INVALID = -1,
	SDL_CONTROLLER_AXIS_LEFTX = 0,
	SDL_CONTROLLER_AXIS_LEFTY,
	SDL_CONTROLLER_AXIS_RIGHTX,
	SDL_CONTROLLER_AXIS_RIGHTY,
	SDL_CONTROLLER_AXIS_TRIGGERLEFT,
	SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
	SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum {
	SDL_CONTROLLER_BUTTON_INVALID = -1,
	SDL_CONTROLLER_BUTTON_A = 0,
	SDL_CONTROLLER_BUTTON_B,
	SDL_CONTROLLER_BUTTON_X,
	SDL_CONTROLLER_BUTTON_Y,
	SDL_CONTROLLER_BUTTON_BACK,
	SDL_CONTROLLER_BUTTON_GUIDE,
	SDL_CONTROLLER_BUTTON_START,
	SDL_CONTROLLER_BUTTON_LEFTSTICK,
	SDL_CONTROLLER_BUTTON_RIGHTSTICK,
	SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
	SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	SDL_CONTROLLER_BUTTON_DPAD_UP,
	SDL_CONTROLLER_BUTTON_DPAD_DOWN,
	SDL_CONTROLLER_BUTTON_DPAD_LEFT,
	SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

SDL_bool           SDL_IsGameController(int joystick_index);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void               SDL_GameControllerClose(SDL_GameController *gamecontroller);
SDL_GameController *SDL_GameControllerFromInstanceID(Sint32 joyid);
int                SDL_GameControllerAddMappingsFromFile(const char *file);
int                SDL_GameControllerRumble(SDL_GameController *gamecontroller,
                                            Uint16 low, Uint16 high, Uint32 ms);
Sint16             SDL_GameControllerGetAxis(SDL_GameController *gamecontroller,
                                             int axis);
Uint8              SDL_GameControllerGetButton(SDL_GameController *gamecontroller,
                                               int button);

SDL_Haptic *SDL_HapticOpen(int device_index);
void        SDL_HapticClose(SDL_Haptic *haptic);
int         SDL_HapticRumbleInit(SDL_Haptic *haptic);
int         SDL_HapticRumblePlay(SDL_Haptic *haptic, float strength, Uint32 length);

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

/* ------------------------------------------------------------ presenting */

/*
 * Present a framebuffer the caller owns.
 *
 * picosdl never allocates pixels: every buffer it touches belongs to the client,
 * which is what lets the library have no full-screen memory of its own and lets a
 * client size and place its own to suit.
 *
 * Two ways to push one, and they differ only in whether you also want an
 * SDL_Surface over it:
 *
 *   - PSDL_CreateWindow() + SDL_GetWindowSurface() + SDL_UpdateWindowSurface(),
 *     when you want the blitters, SDL_FillRect() or partial-rectangle updates.
 *
 *   - PSDL_PresentBuffer(), when you have a framebuffer and want it on the panel.
 *     A client with its own renderer wants this; there is nothing for a surface
 *     to add.
 *
 * Both work at either PSDL_COLOR_DEPTH. `pixels` must be in this build's format,
 * and `pitch` is in BYTES, as everywhere else in SDL.
 *
 * The push is asynchronous: it returns once the transfer has started, so drawing
 * the next frame overlaps it.
 *
 * There is one panel transfer at a time - one DMA chain drives the ST7789 - and a
 * present drains the previous one before arming its own. So the buffer being read
 * is always the one most recently presented, and never more than one. That is the
 * invariant to reason from:
 *
 *   - Two buffers: draw into the one you did not just present. Nothing is reading
 *     it, so there is nothing to wait for, and the next present drains the other
 *     for you. A double-buffered client never calls PSDL_PresentSync() at all.
 *
 *   - One buffer: the buffer you want to draw into is the one in flight, so call
 *     PSDL_PresentSync() first. You then wait out the panel each frame and the
 *     panel holds the visible image, which is the trade one buffer makes.
 *
 * PSDL_PresentSync() waits for whatever is in flight, which by the invariant is
 * the frame you last presented. It is the client's call to make because only the
 * client knows which buffer it is about to touch.
 *
 * What it waits for exactly: the DMA finishing its read of your framebuffer, not
 * the pixels reaching the glass. A few are still in the PIO's FIFO and shifter
 * when it returns. That is the right guarantee for reusing the memory and the
 * wrong one for timing anything visual.
 */
void     PSDL_PresentBuffer(const void *pixels, int w, int h, int pitch);
void     PSDL_PresentSync(void);

/*
 * Is the panel still reading `pixels`? Non-blocking.
 *
 * SDL_FALSE means that buffer is yours to write. Ask about the one you are about
 * to draw into rather than tracking what you last presented - a buffer that is
 * not the one in flight answers SDL_FALSE immediately, so the question is always
 * the one worth asking and the answer never depends on your bookkeeping matching
 * the library's.
 *
 * Passing NULL asks about the panel rather than a buffer: SDL_TRUE while any
 * transfer is outstanding.
 *
 * This is what makes a two-buffer client able to avoid blocking entirely. Filling
 * the free buffer is always safe, but presenting it is not free - a present
 * drains the previous transfer first - so a client that wants that time back
 * waits here, doing its own work, rather than inside the present:
 *
 *     PSDL_PresentBuffer(a, w, h, pitch);
 *     draw_into(b);                                 // safe: a is the one in flight
 *     while (PSDL_BufferBusy(a)) do_something();    // instead of stalling below
 *     PSDL_PresentBuffer(b, w, h, pitch);           // returns at once
 *
 * Compare pointers by the base address given to PSDL_PresentBuffer() or
 * PSDL_CreateWindow(); an interior pointer is a different buffer as far as this
 * is concerned. Call it from the core that presents.
 */
SDL_bool PSDL_BufferBusy(const void *pixels);

/* ---------------------------------------------------------- diagnostics */

/* Print surface-pool and arena occupancy, including peaks. This is how the
 * arena gets sized: run the real workload, read the high-water mark. */
void     PSDL_ReportMemory(void);

/*
 * The letterbox status bands.
 *
 * When the panel is taller than the canvas, the strips above and below it are
 * otherwise unused. PSDL_StatusBands turns them on.
 *
 * The colours are RGB rather than palette indices, and that is the whole point:
 * the bands are picosdl's overlay, not part of the client's indexed world, and
 * they keep the colours asked for whatever the client does to its palette. An
 * earlier version took indices and the letterbox turned red whenever Prince of
 * Persia flashed the screen - that game's damage flash is a write to palette
 * entry 0, and the band was using entry 0 for its background.
 *
 * The header is picosdl's own: frame rate and the load on both cores, numbers only
 * the library is in a position to measure. The footer is whatever
 * PSDL_SetFooterText was given, centred. Both are repainted once a second.
 *
 * No effect on a backend whose panel is exactly the canvas size.
 */
void     PSDL_StatusBands(SDL_bool on, SDL_Color fg, SDL_Color bg);
void     PSDL_SetFooterText(const char *text);
void     PSDL_DumpArena(void);
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
/*
 * The SDL2 API level this implements.
 *
 * The patch level matters to callers, not just to humans: SDLPoP's init_digi()
 * checks for SDL older than 2.0.4 and, if it finds it, asks for AUDIO_U8 to
 * work around a resampling bug in those versions
 * (https://bugzilla.libsdl.org/show_bug.cgi?id=2389). picosdl does not have
 * that bug - it does no resampling at all, and takes S16 stereo straight to
 * the I2S DMA - so claiming 2.0.0 made the game request a format that is not
 * supported here and lose its audio entirely.
 */
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL    22
#define SDL_VERSION(x) do { (x)->major = 2; (x)->minor = 0; (x)->patch = 0; } while (0)
#define SDL_VERSION_ATLEAST(X, Y, Z) \
	((SDL_MAJOR_VERSION >= (X)) && (SDL_MINOR_VERSION >= (Y)) && (SDL_PATCHLEVEL >= (Z)))

#ifdef __cplusplus
}
#endif

#endif /* PICOSDL_SDL_H */
