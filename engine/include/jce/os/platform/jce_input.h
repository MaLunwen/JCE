/*
 * jce_input.h  Cross-platform input management for JCE.
 *
 * Tracks keyboard, mouse, touch, and gamepad state with current/previous
 * frame semantics so callers can query pressed/released transitions.
 *
 * This header is SDL-free; game/application code does not need SDL.
 */

#ifndef JCE_INPUT_H
#define JCE_INPUT_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_keys.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MAX_TOUCHES  10

/* Mouse buttons are numbered from 1 (left = 1, middle = 2, right = 3), so the
 * bit for button b is 1u << (b - 1) and button 1 is bit 0.  That single
 * subtraction is shared by three things that must agree byte for byte: the
 * query API, JceInputFrame.mouse_buttons (the .jirc record/replay wire format)
 * and the action evaluator's JCE_SRC_MOUSE_BUTTON case.  It is spelled once,
 * here, and pinned by tests/os/platform/test_jce_input_mouse_mask.c.
 *
 * `b` must be in 1..32; the query functions bound it before shifting. */
#define JCE_MOUSE_BUTTON_MASK(b)  (1u << ((b) - 1))

/* C99 forbids a repeated typedef and jce_input_device.h forward-declares the
 * same handle.  This header now INCLUDES that one -- JceInputFrame is built
 * from its limits -- so in practice the device header's copy is the one that
 * wins here; the guard is what lets either be included first. */
#ifndef JCE_INPUT_TYPEDEF_DEFINED
#define JCE_INPUT_TYPEDEF_DEFINED
typedef struct JceInput JceInput;
#endif

/* Create / destroy. */
JCE_API JceInput *jce_input_create(void);
JCE_API void      jce_input_destroy(JceInput *input);

/* Call at the START of each frame (before processing events).
   Copies current state  previous, resets per-frame deltas. */
JCE_API void      jce_input_update(JceInput *input);

/* Feed a platform event; call from the engine event handler.
   The event pointer is backend-specific (SDL_Event* internally).

   This is now a two-line function: translate the platform event into
   JceInputEvents, then submit them.  It is kept because it is what the
   engine's event loop calls, and its signature has not changed. */
JCE_API void      jce_input_handle_event(JceInput *input, const void *event);

/* SEAM B.  Feed `count` platform-neutral events into the state machine.
 *
 * This is the ONLY way state enters JceInput from an event source, and it
 * knows nothing about SDL.  Every source is a producer of JceInputEvents --
 * the SDL pump, a future Android JNI bridge, a network input source, a replay,
 * or a test -- so a new source is a new producer, never a new state machine.
 *
 * Size-prefix contract: an event whose `size` is below
 * JCE_INPUT_EVENT_SIZE_V2 is REFUSED (skipped), never partially read.  Events
 * are applied in order; a rejected one does not stop the rest.  NULL `events`
 * or count <= 0 is a no-op. */
JCE_API void      jce_input_submit(JceInput *input,
                                   const JceInputEvent *events, int count);

/* -- Keyboard ------------------------------------------------------- */

JCE_API bool      jce_input_key_down(const JceInput *input, JceKey key);
JCE_API bool      jce_input_key_pressed(const JceInput *input, JceKey key);
JCE_API bool      jce_input_key_released(const JceInput *input, JceKey key);

/* -- Mouse ---------------------------------------------------------- */

JCE_API void      jce_input_mouse_pos(const JceInput *input, float *x, float *y);
JCE_API void      jce_input_mouse_delta(const JceInput *input, float *dx, float *dy);
JCE_API bool      jce_input_mouse_button(const JceInput *input, int button);
JCE_API bool      jce_input_mouse_button_pressed(const JceInput *input, int button);
JCE_API bool      jce_input_mouse_button_released(const JceInput *input, int button);
JCE_API float     jce_input_mouse_wheel(const JceInput *input);

/* -- Touch (mobile) ------------------------------------------------- */

JCE_API int       jce_input_touch_count(const JceInput *input);
JCE_API bool      jce_input_touch_get(const JceInput *input, int index,
              JceFingerID *id, float *x, float *y, float *pressure);

/* -- Gamepad -------------------------------------------------------- */

/* REMOVED in schema v2: JCE_MAX_GAMEPADS and
 *   jce_input_gamepad_count / _button / _button_pressed / _axis
 *
 * An "index among connected pads" IS the defect, so it stops being spellable
 * rather than being redefined.  The array behind it compacted on removal, so
 * unplugging one pad silently handed a different controller to whoever was
 * addressing that index; keeping the calls under a new definition would
 * re-enshrine exactly that.
 *
 * Callers move to jce/os/platform/jce_input_device.h, which this header now
 * includes:
 *   jce_input_gamepad_count(in)          -> jce_input_player_count(in), or
 *                                           jce_input_device_ids(in, ...) for
 *                                           the hardware enumeration
 *   jce_input_gamepad_button(in, 0, b)   -> jce_input_player_button(in, 0, b)
 *   jce_input_gamepad_axis(in, 0, a)     -> jce_input_player_stick /
 *                                           jce_input_player_trigger, or
 *                                           jce_input_device_axis_raw() with
 *                                           jce_input_player_device_of_class()
 *
 * Behaviour under one player is unchanged: JCE_PAIRING_SINGLE_USER (the
 * default) pairs every attached device to slot 0, so "player 0's primary
 * gamepad-layout device" and "pad 0" name the same hardware.
 *
 * THE SEPARATION THAT USED TO LIVE IN THIS FILE STILL HOLDS, and it is worth
 * saying where it moved.  A JCE_INPUT_LAYOUT_RAW device -- a wheel, a HOTAS --
 * publishes ORDINALS, and the deleted array had no ordinal spelling: axes[0]
 * meant LEFTX to everything that read it, so a wheel in slot 0 steered by
 * strafing.  jce_input.c guarded that with a layout gate on the slot; with the
 * array gone, semantic_rec() in jce_input_devices.c is what refuses a raw
 * device's ordinals to every semantic query, and jce_input_devices_apply()
 * replays a device into the slot the frame names WITH its layout, never an
 * ordinal into a semantic one. */

/* -- Frame snapshot (for record / replay) --------------------------- */

/* Compact per-frame state snapshot.  Captures everything jce_input exposes via
 * its query functions; "previous" state is reconstructed by the standard call
 * to jce_input_update() between frames.
 *
 * Wire format == struct layout.  A .jirc file is a 16-byte header followed by a
 * raw memcpy of this struct, once per frame, so every member here is fixed
 * width and every sub-struct is NAMED -- check_abi_snapshot.py can
 * only guard a record it can name, and the anonymous gamepads[] member of v1
 * made the whole frame invisible to it for the format's entire existence.
 * Breaking changes bump JCE_INPUT_FRAME_VERSION.
 *
 * Pinned by sizeof and offsetof in
 * tests/os/platform/test_jce_input_frame_wire.c. */
#define JCE_INPUT_FRAME_VERSION 2u

#define JCE_INPUT_DEVFRAME_FLAG_ACTIVE    0x01u
#define JCE_INPUT_DEVFRAME_FLAG_SEMANTIC  0x02u   /* layout == GAMEPAD */

/* One device's replayable state.  A frame carries no name, GUID or capability
 * bits: a replayed device is anonymous by design, because identity belongs to
 * hardware that is not present during a replay.
 *
 * v1 held a uint32_t button mask and axes[8] per pad, so a 55-button stick, a
 * 16-axis rig and every hat were not mis-slotted in a recording -- they were
 * UNREPRESENTABLE.  The widths below are the device table's own. */
typedef struct JceInputDeviceFrame {
    uint32_t device_id;                       /* 0 == vacated slot          */
    uint32_t buttons[JCE_INPUT_BUTTON_WORDS]; /* 128 raw buttons            */
    float    axes[JCE_INPUT_MAX_AXES];        /* 16                         */
    uint8_t  hats[JCE_INPUT_MAX_HATS];
    uint8_t  cls, layout, style;
    int8_t   player;
    uint8_t  flags;                           /* JCE_INPUT_DEVFRAME_FLAG_*  */
    uint8_t  reserved[35];                    /* gyro(12) + accel(12) fit   */
} JceInputDeviceFrame;                        /* 128 B */

/* Touch was ABSENT from v1 entirely: a channel that is not in the frame cannot
 * be replayed, and so cannot be regression-tested at all. */
typedef struct JceInputTouchFrame {
    uint64_t id;
    float    x, y, pressure;
} JceInputTouchFrame;                         /* 24 B */

typedef struct JceInputFrame {
    uint32_t version, key_count;

    /* Keyboard: 1 bit per key.  4096 bits covers JCE_KEY_COUNT with room. */
    uint64_t keys_bits[64];

    /* Mouse */
    float    mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_wheel;
    uint32_t mouse_buttons;

    uint32_t touch_count, _pad;
    JceInputTouchFrame  touches[JCE_MAX_TOUCHES];

    /* device_count is a HIGH-WATER mark over the device table, not a count of
     * live devices: a vacated INNER slot is carried as device_id 0 so slot
     * identity round-trips.  Slots at or above it are ZEROED by apply(). */
    uint32_t device_count, _pad2;
    JceInputDeviceFrame devices[JCE_INPUT_MAX_DEVICES];
} JceInputFrame;                              /* 2336 B; ~137 KiB/s @ 60fps */

/* Capture the current input state into `out`.  Safe to call any time. */
JCE_API void jce_input_capture(const JceInput *input, JceInputFrame *out);

/* Override the current input state from a previously captured frame.  After
 * this call, the next jce_input_update() rolls cur->prev as usual.  Returns
 * false if the frame version does not match.
 *
 * v2 SEMANTICS: this SHRINKS.  Device slots at or above frame->device_count
 * are zeroed and marked inactive.  v1 never did, which left phantom slots
 * behind holding id 0 -- the exact value the old SDL handle lookup fell back
 * to when SDL_GetGamepadFromID returned NULL, so a real pad's events could
 * route into a ghost.
 *
 * A replayed device is NOT reachable by live device events: it carries no
 * backend instance, and jce_input_devices_find_instance() refuses to match a
 * replayed record.  That is the direct successor of the `present` flag the
 * deleted pad-index array used for the same purpose. */
JCE_API bool jce_input_apply(JceInput *input, const JceInputFrame *frame);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_H */
