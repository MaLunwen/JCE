/*
 * jce_input_actions.h  Action-based input abstraction.
 *
 * Maps logical actions (e.g. "move_forward", "jump") to physical
 * inputs (keyboard scancodes, gamepad buttons/axes, touch zones).
 * Game code queries actions, not raw hardware.
 *
 * Maximum JCE_ACTION_MAX actions, each with up to JCE_ACTION_MAX_BINDS
 * physical bindings. Bindings can be changed at runtime (rebinding).
 *
 * Layer: OS Abstraction (Layer 1).
 */

#ifndef JCE_INPUT_ACTIONS_H
#define JCE_INPUT_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceInput        JceInput;
typedef struct JceInputActions JceInputActions;

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JCE_ACTION_MAX        64
#define JCE_ACTION_MAX_BINDS   4

/* ================================================================== */
/* Binding source types                                                */
/* ================================================================== */

typedef enum {
    JCE_BIND_KEY,           /* Keyboard scancode               */
    JCE_BIND_MOUSE_BTN,     /* Mouse button (1=left,2=mid,3=right) */
    JCE_BIND_GAMEPAD_BTN,   /* Gamepad button (SDL_GamepadButton) */
    JCE_BIND_GAMEPAD_AXIS,  /* Gamepad axis (SDL_GamepadAxis)     */
} JceBindType;

typedef struct {
    JceBindType type;
    int         code;       /* scancode / button / axis code   */
    float       scale;      /* multiplier (e.g. -1 to invert axis) */
    float       deadzone;   /* axis deadzone (0-1), default 0.15 */
} JceBinding;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceInputActions *jce_actions_create(void);
void             jce_actions_destroy(JceInputActions *a);

/* ================================================================== */
/* Action registration                                                 */
/* ================================================================== */

/* Register a named action. Returns the action ID (0-based index).
   Returns -1 if the name already exists or table is full.
   Names are copied internally. */
int  jce_action_register(JceInputActions *a, const char *name);

/* Bind a physical input to an action. Up to JCE_ACTION_MAX_BINDS per action.
   Returns true on success. */
bool jce_action_bind(JceInputActions *a, int action_id,
                      const JceBinding *binding);

/* Unbind all bindings for an action. */
void jce_action_unbind_all(JceInputActions *a, int action_id);

/* Look up action ID by name. Returns -1 if not found. */
int  jce_action_find(const JceInputActions *a, const char *name);

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* Read current input state and update all action values.
   Call once per frame after jce_input_handle_event. */
void jce_actions_update(JceInputActions *a, const JceInput *input);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Action value this frame (-1.0 to 1.0; keys give 0 or scale). */
float jce_action_value(const JceInputActions *a, int action_id);

/* True if action transitioned from inactive to active this frame. */
bool  jce_action_pressed(const JceInputActions *a, int action_id);

/* True if action is currently active (value != 0). */
bool  jce_action_down(const JceInputActions *a, int action_id);

/* True if action transitioned from active to inactive this frame. */
bool  jce_action_released(const JceInputActions *a, int action_id);

/* ================================================================== */
/* Helper: register default FPS bindings                               */
/* ================================================================== */

/* Registers standard actions and binds WASD+mouse+gamepad defaults:
 *   "move_forward", "move_back", "move_left", "move_right",
 *   "move_up", "move_down", "sprint", "look_x", "look_y"
 * Returns the first action ID (they are contiguous). */
int jce_actions_bind_fps_defaults(JceInputActions *a);

#ifdef __cplusplus
}
#endif

#endif /* JCE_INPUT_ACTIONS_H */
