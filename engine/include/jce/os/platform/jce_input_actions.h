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


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

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

JCE_API JceInputActions *jce_actions_create(void);
JCE_API void             jce_actions_destroy(JceInputActions *a);

/* Load an action map from an editor-authored JSON file (the Input Manager
 * panel's input_actions.json — { "actions": [ { "name", "binds": [
 * {"type","code","scale","deadzone"} ] } ] }).  Returns NULL on a missing
 * / unparseable / empty file so the caller can fall back to
 * jce_actions_bind_fps_defaults(). */
JCE_API JceInputActions *jce_actions_load_file(const char *path);

/* Save an action map to a JSON file in the exact schema
 * jce_actions_load_file() reads.  Returns true on success. */
JCE_API bool jce_actions_save_file(const JceInputActions *a, const char *path);

/* ================================================================== */
/* Action registration                                                 */
/* ================================================================== */

/* Register a named action. Returns the action ID (0-based index).
   Returns -1 if the name already exists or table is full.
   Names are copied internally. */
JCE_API int  jce_action_register(JceInputActions *a, const char *name);

/* Bind a physical input to an action. Up to JCE_ACTION_MAX_BINDS per action.
   Returns true on success. */
JCE_API bool jce_action_bind(JceInputActions *a, int action_id,
                             const JceBinding *binding);

/* Unbind all bindings for an action. */
JCE_API void jce_action_unbind_all(JceInputActions *a, int action_id);

/* Look up action ID by name. Returns -1 if not found. */
JCE_API int  jce_action_find(const JceInputActions *a, const char *name);

/* ================================================================== */
/* Enumeration (read accessors)                                        */
/* ================================================================== */

/* Number of registered actions (0 when a == NULL). */
JCE_API int  jce_actions_count(const JceInputActions *a);

/* Name of an action by ID. Returns NULL when out of range.  The pointer
   stays valid until the table is destroyed. */
JCE_API const char *jce_action_name(const JceInputActions *a, int action_id);

/* Number of bindings on an action (0 when out of range). */
JCE_API int  jce_action_bind_count(const JceInputActions *a, int action_id);

/* Copy binding `bind_index` of an action into *out.
   Returns false when out of range. */
JCE_API bool jce_action_bind_at(const JceInputActions *a, int action_id,
                                int bind_index, JceBinding *out);

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* Read current input state and update all action values.
   Call once per frame after jce_input_handle_event. */
JCE_API void jce_actions_update(JceInputActions *a, const JceInput *input);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Action value this frame (-1.0 to 1.0; keys give 0 or scale). */
JCE_API float jce_action_value(const JceInputActions *a, int action_id);

/* True if action transitioned from inactive to active this frame. */
JCE_API bool  jce_action_pressed(const JceInputActions *a, int action_id);

/* True if action is currently active (value != 0). */
JCE_API bool  jce_action_down(const JceInputActions *a, int action_id);

/* True if action transitioned from active to inactive this frame. */
JCE_API bool  jce_action_released(const JceInputActions *a, int action_id);

/* ================================================================== */
/* Helper: register default FPS bindings                               */
/* ================================================================== */

/* Registers the engine's canonical default action table — the single
 * source of truth shared by the runtime fallback and the editor's Input
 * Manager seed:
 *   "move_forward"  W   / left stick -Y    "move_back"  S
 *   "move_left"     A                      "move_right" D / left stick +X
 *   "jump"          SPACE / pad South (A)
 *   "sprint"        LSHIFT / pad L3 (left-stick click)
 *   "move_up", "move_down"   registered but UNBOUND (fly-style apps)
 *   "look_x", "look_y"       right stick
 * (Convention notes: sprint was LCTRL and SPACE was move_up before the
 * table was unified with the editor's; "jump" is what gameplay consumes.)
 * Returns the first action ID (they are contiguous). */
JCE_API int jce_actions_bind_fps_defaults(JceInputActions *a);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_ACTIONS_H */
