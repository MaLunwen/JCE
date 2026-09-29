/*
 * jce_input_event.h  Platform-neutral input event — the value type that both
 *                    input seams speak.
 *
 * SEAM A (jce_input_sdl.c) PRODUCES these from SDL_Events and knows nothing
 * about JceInput.  SEAM B (jce_input_submit, jce_input.c) CONSUMES them and
 * knows nothing about SDL.  Everything that reaches JceInput reaches it as one
 * of these: the SDL pump, a future Android JNI bridge, a network input source,
 * a replay, or a test.  A new source is a new producer, never a new state
 * machine.
 *
 * DEVICE_ADDED deliberately carries ONLY the backend instance id.  Probing a
 * device (name, GUID, type, axis/button counts, capabilities) is the state
 * machine's job, so the translator stays a genuinely pure function of its
 * SDL_Event — which is the property that makes it testable with nothing
 * plugged in.
 *
 * Layer: OS Abstraction (Layer 1).  Ownership: by value; nothing here points
 * at anything.  Threading: values are plain data and carry no lock.
 */

#ifndef JCE_INPUT_EVENT_H
#define JCE_INPUT_EVENT_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Wire values.  A replay file or a network source stores these integers, so
 * enumerators are APPENDED before _KIND_COUNT and never reordered. */
typedef enum {
    JCE_INPUT_EVENT_NONE = 0,
    JCE_INPUT_EVENT_KEY,
    JCE_INPUT_EVENT_MOUSE_MOTION,
    JCE_INPUT_EVENT_MOUSE_BUTTON,
    JCE_INPUT_EVENT_MOUSE_WHEEL,
    JCE_INPUT_EVENT_TOUCH,
    JCE_INPUT_EVENT_DEVICE_ADDED,
    JCE_INPUT_EVENT_DEVICE_REMOVED,
    JCE_INPUT_EVENT_DEVICE_BUTTON,
    JCE_INPUT_EVENT_DEVICE_AXIS,
    JCE_INPUT_EVENT_DEVICE_HAT,
    JCE_INPUT_EVENT_DEVICE_POWER,
    /* APPENDED (never reordered -- these are wire values).  Carries the
     * composed UTF-8 the platform's text/IME layer produced, which is a
     * different thing from a keycode: one keystroke can produce several
     * bytes, an IME commit can produce several codepoints at once, and a
     * scancode cannot express either. */
    JCE_INPUT_EVENT_TEXT,
    JCE_INPUT_EVENT_KIND_COUNT
} JceInputEventKind;

/* Touch phase for JceInputTouchEvent.phase. */
#define JCE_INPUT_TOUCH_PHASE_DOWN    0u
#define JCE_INPUT_TOUCH_PHASE_MOTION  1u
#define JCE_INPUT_TOUCH_PHASE_UP      2u

/* Every payload is a NAMED top-level struct, never an inline anonymous one:
 * check_abi_snapshot.py can only guard what it can name. */

/* `scancode` is a JceKey.  `mod` is reserved for a future modifier mask and
 * is currently always 0; consumers must not read it. */
typedef struct JceInputKeyEvent {
    int32_t  scancode;
    uint8_t  down, repeat;
    uint16_t mod;
} JceInputKeyEvent;

/* Absolute position AND this event's relative motion, because a captured
 * mouse reports meaningful dx/dy with a frozen x/y. */
typedef struct JceInputMotionEvent {
    float x, y, dx, dy;
} JceInputMotionEvent;

/* `button` is 1-BASED (left = 1), matching JCE_MOUSE_BUTTON_MASK(b) in
 * jce_input.h, which is 1u << (b - 1). */
typedef struct JceInputMouseButtonEvent {
    uint8_t button, down, _pad[2];
} JceInputMouseButtonEvent;

typedef struct JceInputWheelEvent {
    float x, y;
} JceInputWheelEvent;

/* `phase` is one of JCE_INPUT_TOUCH_PHASE_*. */
typedef struct JceInputTouchEvent {
    uint64_t finger;
    float    x, y, pressure;
    uint8_t  phase, _pad[3];
} JceInputTouchEvent;

/* ADDED / REMOVED carry ONLY the backend instance id (plus the class and
 * layout the backend already knew without probing).  See the file header:
 * this is what keeps the translator pure. */
typedef struct JceInputDeviceLifecycleEvent {
    uint64_t instance;
    uint8_t  cls, layout, _pad[6];
} JceInputDeviceLifecycleEvent;

/* `semantic` is 1 when `code` is a JceGamepadButton (the device has a mapping)
 * and 0 when it is a raw ordinal. */
typedef struct JceInputDeviceButtonEvent {
    uint64_t instance;
    int32_t  code;
    uint8_t  down, semantic, _pad[2];
} JceInputDeviceButtonEvent;

/* `value` is ALREADY normalised to [-1, 1] by the producer.  Deadzone,
 * saturation and curve are the evaluator's business, never the translator's. */
typedef struct JceInputDeviceAxisEvent {
    uint64_t instance;
    int32_t  axis;
    float    value;
    uint8_t  semantic, _pad[3];
} JceInputDeviceAxisEvent;

typedef struct JceInputDeviceHatEvent {
    uint64_t instance;
    int32_t  hat;
    uint8_t  mask, _pad[3];
} JceInputDeviceHatEvent;

typedef struct JceInputDevicePowerEvent {
    uint64_t instance;
    int32_t  percent, state;
} JceInputDevicePowerEvent;

/* Composed text, NUL-terminated UTF-8.  32 bytes is SDL_TextInputEvent's own
 * payload size and fits the 56-byte union with room to spare; a longer IME
 * commit arrives as several events, which is why the consumer appends. */
typedef struct JceInputTextEvent {
    char utf8[32];
} JceInputTextEvent;

/* Size-prefix contract: the caller sets `size` to sizeof(JceInputEvent); a
 * receiver REFUSES any record whose size is below this constant rather than
 * silently accepting a short one. */
#define JCE_INPUT_EVENT_SIZE_V2 64u

/* AGENTS.md rule 9 forbids an anonymous union in a public struct unless
 * JCE_C11 is asserted.  The exemption is recorded in that file: JceEvent
 * (jce_window_event.h) has carried exactly this shape since it landed, every
 * supported compiler accepts it in C99 mode, and naming the union member would
 * put a second spelling of the same access into every consumer.  No `bool` and
 * no `enum` appears in any layout above -- rule 9's other two halves stand.
 *
 * raw[56] rather than [48]: 4 + 4 + 48 is 56, and JCE_INPUT_EVENT_SIZE_V2 is
 * 64, so a 48-byte union would make every receiver reject every event.  The
 * headroom is what absorbs gyro (12) + accel (12) + touchpad (20) later.
 * Verified at compile time with JCE_SASSERT(sizeof(JceInputEvent) == 64, ...)
 * in engine/src/os/platform/jce_input_sdl.c, the same idiom jce_gamepad.h and
 * jce_keys.h point at for their own SDL-numeric-match assertions -- kept out
 * of this header because the idiom is engine-internal, not public API. */
typedef struct JceInputEvent {
    uint32_t size;                 /* sizeof(JceInputEvent) */
    int32_t  kind;                 /* JceInputEventKind     */
    union {
        JceInputKeyEvent             key;
        JceInputMotionEvent          motion;
        JceInputMouseButtonEvent     mbutton;
        JceInputWheelEvent           wheel;
        JceInputTouchEvent           touch;
        JceInputDeviceLifecycleEvent device;
        JceInputDeviceButtonEvent    dbutton;
        JceInputDeviceAxisEvent      daxis;
        JceInputDeviceHatEvent       dhat;
        JceInputDevicePowerEvent     dpower;
        JceInputTextEvent            text;
        uint8_t                      raw[56];
    };
} JceInputEvent;

JCE_EXTERN_C_END

#endif /* JCE_INPUT_EVENT_H */
