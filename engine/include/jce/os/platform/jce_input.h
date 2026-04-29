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
#include <jce/os/platform/jce_keys.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MAX_GAMEPADS 4
#define JCE_MAX_TOUCHES  10

typedef struct JceInput JceInput;

/* Create / destroy. */
JCE_API JceInput *jce_input_create(void);
JCE_API void      jce_input_destroy(JceInput *input);

/* Call at the START of each frame (before processing events).
   Copies current state  previous, resets per-frame deltas. */
JCE_API void      jce_input_update(JceInput *input);

/* Feed a platform event; call from the engine event handler.
   The event pointer is backend-specific (SDL_Event* internally). */
JCE_API void      jce_input_handle_event(JceInput *input, const void *event);

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
bool      jce_input_touch_get(const JceInput *input, int index,
              JceFingerID *id, float *x, float *y, float *pressure);

/* -- Gamepad -------------------------------------------------------- */

JCE_API int       jce_input_gamepad_count(const JceInput *input);
bool      jce_input_gamepad_button(const JceInput *input, int pad,
              JceGamepadButton btn);
bool      jce_input_gamepad_button_pressed(const JceInput *input, int pad,
              JceGamepadButton btn);
float     jce_input_gamepad_axis(const JceInput *input, int pad,
              JceGamepadAxis axis);

/* -- Frame snapshot (for record / replay) --------------------------- */

/* Compact per-frame state snapshot.  Captures everything jce_input
 * exposes via its query functions; "previous" state is reconstructed
 * by the standard call to jce_input_update() between frames.
 *
 * Wire format == struct layout: keep this packed and stable across
 * minor versions.  Breaking changes must bump JCE_INPUT_FRAME_VERSION. */
#define JCE_INPUT_FRAME_VERSION 1u

typedef struct {
    uint32_t version;        /* JCE_INPUT_FRAME_VERSION */
    uint32_t key_count;      /* JCE_KEY_COUNT (sanity check) */

    /* Keyboard: 1 bit per key.  4096 bits comfortably covers
     * SDL_SCANCODE_COUNT (~512) with growth headroom. */
    uint64_t keys_bits[64];

    /* Mouse */
    float    mouse_x, mouse_y;
    float    mouse_dx, mouse_dy;
    float    mouse_wheel;
    uint32_t mouse_buttons;

    /* Gamepads — first JCE_MAX_GAMEPADS only. */
    uint32_t gamepad_count;
    struct {
        uint32_t buttons;
        float    axes[8];    /* >= SDL_GAMEPAD_AXIS_COUNT */
    } gamepads[JCE_MAX_GAMEPADS];
} JceInputFrame;

/* Capture the current input state into `out`.  Safe to call any time. */
JCE_API void jce_input_capture(const JceInput *input, JceInputFrame *out);

/* Override the current input state from a previously captured frame.
 * After this call, the next jce_input_update() will roll cur→prev as
 * usual.  Returns false if the frame version does not match. */
JCE_API bool jce_input_apply(JceInput *input, const JceInputFrame *frame);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_H */
