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
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceInput        JceInput;
typedef struct JceInputActions JceInputActions;

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

#define JCE_ACTION_MAX        64
#define JCE_ACTION_MAX_BINDS   4
#define JCE_SCHEME_MAX         8     /* named control schemes per action map  */
#define JCE_SCHEME_NAME_MAX    32

/* ================================================================== */
/* Device groups & control schemes                                     */
/* ================================================================== */

/* Physical device family a binding belongs to.  Used to partition bindings
 * into named control schemes (e.g. "KeyboardMouse" vs "Gamepad").  A binding
 * tagged JCE_DEVICE_NONE belongs to EVERY scheme (it is device-agnostic) so
 * that an action map with no device tags behaves exactly like an untagged one:
 * all bindings resolve regardless of the active scheme. */
typedef enum {
    JCE_DEVICE_NONE     = 0,   /* untagged: active in every scheme         */
    JCE_DEVICE_KBM      = 1,   /* keyboard + mouse                         */
    JCE_DEVICE_GAMEPAD  = 2,   /* any gamepad                              */
    JCE_DEVICE_TOUCH    = 3,   /* touch screen                             */
    JCE_DEVICE_GROUP_COUNT
} JceInputDeviceGroup;

/* A device-group bitmask (1 << JceInputDeviceGroup).  A scheme owns the set
 * of device groups whose bindings it resolves. */
#define JCE_DEVICE_BIT(g)   (1u << (g))

/* ================================================================== */
/* Binding source types                                                */
/* ================================================================== */

typedef enum {
    JCE_BIND_KEY,           /* Keyboard scancode               */
    JCE_BIND_MOUSE_BTN,     /* Mouse button (1=left,2=mid,3=right) */
    JCE_BIND_GAMEPAD_BTN,   /* Gamepad button (SDL_GamepadButton) */
    JCE_BIND_GAMEPAD_AXIS,  /* Gamepad axis (SDL_GamepadAxis)     */
    JCE_BIND_COMPOSITE,     /* Multi-key composite (1D axis / 2D vector) */
} JceBindType;

/* Composite binding flavours.  Stored in JceBinding.code when the binding
 * type is JCE_BIND_COMPOSITE. */
typedef enum {
    JCE_COMPOSITE_AXIS_1D   = 0, /* pos/neg keys  -> scalar (-1..+1)       */
    JCE_COMPOSITE_VECTOR_2D = 1, /* up/down/left/right keys -> (x,y) raw   */
} JceCompositeKind;

typedef struct {
    JceBindType type;
    int         code;       /* scancode / button / axis code; for COMPOSITE
                               this holds a JceCompositeKind value          */
    float       scale;      /* multiplier (e.g. -1 to invert axis) */
    float       deadzone;   /* axis deadzone (0-1), default 0.15 */

    /* Composite sub-key scancodes (JCE_BIND_KEY codes).  Used only when
     * type == JCE_BIND_COMPOSITE; ignored (and may stay 0) otherwise so the
     * struct and file format remain backward-compatible with scalar binds.
     *   1D axis : comp_pos = positive key, comp_neg = negative key.
     *   2D vec  : comp_pos = +x (right), comp_neg = -x (left),
     *             comp_up  = +y (up),    comp_down = -y (down).            */
    int         comp_pos;
    int         comp_neg;
    int         comp_up;
    int         comp_down;

    /* Control-scheme tag (JceInputDeviceGroup).  0 == JCE_DEVICE_NONE keeps
     * the binding device-agnostic (active in every scheme), so a struct left
     * zero-initialized behaves exactly like the pre-scheme engine.  When the
     * action map defines named schemes, only bindings whose group is either
     * JCE_DEVICE_NONE or contained in the active scheme's mask resolve. */
    int         device_group;
} JceBinding;

/* Typed 2D query result for composite (2D vector) actions. */
typedef struct {
    float x;
    float y;
} JceActionVec2;

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

/* Load the same editor-authored schema from a bounded memory block.  The
 * bytes need not be NUL-terminated.  This is the canonical parser used by
 * packed single-file applications; malformed, empty, or action-free input
 * returns NULL. */
JCE_API JceInputActions *jce_actions_load_memory(const void *data, size_t size);

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
/* Control schemes (named device groups + active-scheme switching)     */
/* ================================================================== */

/* Define a named control scheme that resolves the given device-group mask.
 * `device_mask` is an OR of JCE_DEVICE_BIT(JCE_DEVICE_*) values, e.g.
 *   jce_action_scheme_register(a, "KeyboardMouse", JCE_DEVICE_BIT(JCE_DEVICE_KBM));
 *   jce_action_scheme_register(a, "Gamepad",       JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD));
 * Bindings tagged JCE_DEVICE_NONE always resolve (they belong to every
 * scheme).  The FIRST registered scheme becomes the active one.  Returns the
 * scheme ID (0-based), or -1 on duplicate name / full table / bad args.
 *
 * With zero schemes defined the map keeps the legacy behavior: ALL bindings
 * resolve regardless of their device_group tag. */
JCE_API int  jce_action_scheme_register(JceInputActions *a, const char *name,
                                        unsigned device_mask);

/* Number of defined control schemes (0 == legacy "all bindings active"). */
JCE_API int  jce_action_scheme_count(const JceInputActions *a);

/* Look up a scheme ID by name. Returns -1 if not found. */
JCE_API int  jce_action_scheme_find(const JceInputActions *a, const char *name);

/* Name / device mask of a scheme by ID (NULL / 0 when out of range). */
JCE_API const char *jce_action_scheme_name(const JceInputActions *a, int scheme_id);
JCE_API unsigned    jce_action_scheme_mask(const JceInputActions *a, int scheme_id);

/* Currently-active scheme ID (-1 when no schemes are defined). */
JCE_API int  jce_action_scheme_active(const JceInputActions *a);

/* Manually select the active scheme by ID.  Passing a valid ID also DISABLES
 * auto last-used-device switching until jce_action_scheme_set_auto(a,true) is
 * called, so a game's settings menu can pin a scheme.  Returns true on
 * success (valid ID, schemes defined). */
JCE_API bool jce_action_scheme_set_active(JceInputActions *a, int scheme_id);

/* Enable / disable automatic last-used-device scheme switching (default ON).
 * When ON, jce_actions_update() switches the active scheme to whichever one
 * owns the device group that produced fresh input this frame. */
JCE_API void jce_action_scheme_set_auto(JceInputActions *a, bool enabled);
JCE_API bool jce_action_scheme_auto(const JceInputActions *a);

/* The device group that most recently produced input (JCE_DEVICE_NONE before
 * any input is seen).  Updated by jce_actions_update() whether or not schemes
 * are defined, so games can show the right button prompts. */
JCE_API int  jce_action_last_device(const JceInputActions *a);

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

/* Action value this frame (-1.0 to 1.0; keys give 0 or scale).
 * For a JCE_BIND_COMPOSITE 2D-vector action this returns the magnitude of the
 * resolved (x,y) vector clamped to 0..1 (so jce_action_down() still reports
 * "active"); use jce_action_value2() to read the typed vector itself. */
JCE_API float jce_action_value(const JceInputActions *a, int action_id);

/* Typed 2D value this frame, for composite (2D vector) actions.
 *
 * Returns the RAW per-axis sum (NOT normalized): with a WASD composite,
 * holding W (up) + D (right) yields (+1, +1).  Callers that want a
 * unit-length direction should normalize the result themselves.
 *
 * Scalar / 1D-axis / non-composite actions resolve to (value, 0).  Out of
 * range action ids resolve to (0, 0).  `out` must be non-NULL. */
JCE_API void jce_action_value2(const JceInputActions *a, int action_id,
                               JceActionVec2 *out);

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
