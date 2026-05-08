/*
 * jce_window_event.h  Backend-neutral input/window event type for JCE.
 *
 * Replaces the leaky  on_event(const void*)  contract that used to
 * forward raw SDL_Event pointers to applications. With JceEvent the
 * application sees a stable, SDL-free struct that is safe to depend on
 * across SDL major-version bumps (or, eventually, alternative window
 * backends).
 *
 * Modifier-flag values (JCE_KMOD_*) and mouse-button codes
 * (JCE_MOUSE_BUTTON_*) match SDL3 numerically so the engine-internal
 * translator can copy them through unchanged.
 */

#ifndef JCE_PLATFORM_WINDOW_EVENT_H
#define JCE_PLATFORM_WINDOW_EVENT_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_keys.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── Mouse buttons (match SDL3) ────────────────────────────────── */
#define JCE_MOUSE_BUTTON_LEFT    1
#define JCE_MOUSE_BUTTON_MIDDLE  2
#define JCE_MOUSE_BUTTON_RIGHT   3
#define JCE_MOUSE_BUTTON_X1      4
#define JCE_MOUSE_BUTTON_X2      5

/* ── Key modifiers (match SDL_KMOD_*) ──────────────────────────── */
#define JCE_KMOD_NONE     0x0000u
#define JCE_KMOD_LSHIFT   0x0001u
#define JCE_KMOD_RSHIFT   0x0002u
#define JCE_KMOD_LCTRL    0x0040u
#define JCE_KMOD_RCTRL    0x0080u
#define JCE_KMOD_LALT     0x0100u
#define JCE_KMOD_RALT     0x0200u
#define JCE_KMOD_LGUI     0x0400u
#define JCE_KMOD_RGUI     0x0800u
#define JCE_KMOD_NUM      0x1000u
#define JCE_KMOD_CAPS     0x2000u
#define JCE_KMOD_MODE     0x4000u
#define JCE_KMOD_SCROLL   0x8000u

#define JCE_KMOD_CTRL    (JCE_KMOD_LCTRL  | JCE_KMOD_RCTRL)
#define JCE_KMOD_SHIFT   (JCE_KMOD_LSHIFT | JCE_KMOD_RSHIFT)
#define JCE_KMOD_ALT     (JCE_KMOD_LALT   | JCE_KMOD_RALT)
#define JCE_KMOD_GUI     (JCE_KMOD_LGUI   | JCE_KMOD_RGUI)

/* ── Event type tag ────────────────────────────────────────────── */

typedef enum JceEventType {
    JCE_EVENT_NONE = 0,
    JCE_EVENT_QUIT,

    JCE_EVENT_KEY_DOWN,
    JCE_EVENT_KEY_UP,
    JCE_EVENT_TEXT_INPUT,

    JCE_EVENT_MOUSE_MOTION,
    JCE_EVENT_MOUSE_BUTTON_DOWN,
    JCE_EVENT_MOUSE_BUTTON_UP,
    JCE_EVENT_MOUSE_WHEEL,

    JCE_EVENT_WINDOW_FOCUS_GAINED,
    JCE_EVENT_WINDOW_FOCUS_LOST,
    JCE_EVENT_WINDOW_RESIZED,
    JCE_EVENT_WINDOW_CLOSE,
    JCE_EVENT_WINDOW_EXPOSED,

    JCE_EVENT_OTHER     /* untranslated backend event */
} JceEventType;

/* ── Per-category payload structs ──────────────────────────────── */

typedef struct JceKeyboardEvent {
    JceKey   scancode;   /* JCE_KEY_* (matches SDL_Scancode value) */
    uint16_t mod;        /* OR of JCE_KMOD_* */
    bool     repeat;
} JceKeyboardEvent;

typedef struct JceMouseMotionEvent {
    float x, y;          /* window-relative position */
    float xrel, yrel;    /* movement since last event */
} JceMouseMotionEvent;

typedef struct JceMouseButtonEvent {
    uint8_t button;      /* JCE_MOUSE_BUTTON_* */
    uint8_t clicks;      /* 1 = single, 2 = double, ... */
    float   x, y;
} JceMouseButtonEvent;

typedef struct JceMouseWheelEvent {
    float x, y;          /* +y = scroll up */
} JceMouseWheelEvent;

typedef struct JceTextInputEvent {
    /* UTF-8, null-terminated. Matches SDL3's 32-byte buffer + slack. */
    char text[64];
} JceTextInputEvent;

typedef struct JceWindowResizeEvent {
    uint32_t w, h;       /* new size in pixels */
} JceWindowResizeEvent;

/* ── Tagged union ──────────────────────────────────────────────── */

typedef struct JceEvent {
    JceEventType type;
    union {
        JceKeyboardEvent      key;
        JceMouseMotionEvent   motion;
        JceMouseButtonEvent   button;
        JceMouseWheelEvent    wheel;
        JceTextInputEvent     text;
        JceWindowResizeEvent  resize;
    };
} JceEvent;

JCE_EXTERN_C_END

#endif /* JCE_PLATFORM_WINDOW_EVENT_H */
