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

JCE_EXTERN_C_END

#endif /* JCE_INPUT_H */
