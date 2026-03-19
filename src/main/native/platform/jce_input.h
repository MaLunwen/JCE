/*
 * jce_input.h  Cross-platform input management for JCE.
 *
 * Tracks keyboard, mouse, touch, and gamepad state with current/previous
 * frame semantics so callers can query pressed/released transitions.
 */

#ifndef JCE_INPUT_H
#define JCE_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define JCE_MAX_GAMEPADS 4
#define JCE_MAX_TOUCHES  10

typedef struct JceInput JceInput;

/* Create / destroy. */
JceInput *jce_input_create(void);
void      jce_input_destroy(JceInput *input);

/* Call at the START of each frame (before processing events).
   Copies current state  previous, resets per-frame deltas. */
void      jce_input_update(JceInput *input);

/* Feed an SDL_Event; call from SDL_AppEvent for every event. */
void      jce_input_handle_event(JceInput *input, const SDL_Event *event);

/* -- Keyboard ------------------------------------------------------- */

bool      jce_input_key_down(const JceInput *input, SDL_Scancode key);
bool      jce_input_key_pressed(const JceInput *input, SDL_Scancode key);
bool      jce_input_key_released(const JceInput *input, SDL_Scancode key);

/* -- Mouse ---------------------------------------------------------- */

void      jce_input_mouse_pos(const JceInput *input, float *x, float *y);
void      jce_input_mouse_delta(const JceInput *input, float *dx, float *dy);
bool      jce_input_mouse_button(const JceInput *input, int button);
bool      jce_input_mouse_button_pressed(const JceInput *input, int button);
bool      jce_input_mouse_button_released(const JceInput *input, int button);
float     jce_input_mouse_wheel(const JceInput *input);

/* -- Touch (mobile) ------------------------------------------------- */

int       jce_input_touch_count(const JceInput *input);
bool      jce_input_touch_get(const JceInput *input, int index,
              SDL_FingerID *id, float *x, float *y, float *pressure);

/* -- Gamepad -------------------------------------------------------- */

int       jce_input_gamepad_count(const JceInput *input);
bool      jce_input_gamepad_button(const JceInput *input, int pad,
              SDL_GamepadButton btn);
bool      jce_input_gamepad_button_pressed(const JceInput *input, int pad,
              SDL_GamepadButton btn);
float     jce_input_gamepad_axis(const JceInput *input, int pad,
              SDL_GamepadAxis axis);

#endif /* JCE_INPUT_H */
