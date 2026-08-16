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
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input_device.h>

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

/* key + composite + stick + trigger + a raw-device axis is legitimately more
 * than four.  This is a MACRO, and macros are outside the reach of the ABI
 * snapshot gate: a project compiled against 4 sized its own JceBinding arrays
 * at 4 and must be REBUILT (`jce.py sdk`), not relinked. */
#define JCE_ACTION_MAX_BINDS   8
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

/* Physical source of a binding.
 *
 * The enumerators are RENUMBERED from schema 1 (which froze 0 KEY, 1
 * MOUSE_BTN, 2 GAMEPAD_BTN, 3 GAMEPAD_AXIS, 4 COMPOSITE on disk) because v2
 * needs a NONE at zero -- a zero-initialised JceInputSource must mean
 * "absent", not "the letter A".  The on-disk numbering is translated by
 * bind_type_from_v1() / bind_type_to_v1() in jce_input_actions.c, which are
 * the only place the two numberings meet; it is never the in-memory
 * numbering. */
typedef enum {
    JCE_SRC_NONE = 0,
    JCE_SRC_KEY,               /* keyboard scancode (JceKey)                  */
    JCE_SRC_MOUSE_BUTTON,      /* 1 = left, 2 = middle, 3 = right             */
    JCE_SRC_MOUSE_AXIS,        /* 0 = dx, 1 = dy, 2 = wheel                   */
    JCE_SRC_PAD_BUTTON,        /* JceGamepadButton, semantic                  */
    JCE_SRC_PAD_AXIS,          /* JceGamepadAxis, semantic                    */
    JCE_SRC_PAD_STICK,         /* 2D source; `channel` picks x or y           */
    JCE_SRC_JOY_BUTTON,        /* ordinal button, valid on ANY device         */
    JCE_SRC_JOY_AXIS,          /* ordinal axis,   valid on ANY device         */
    JCE_SRC_JOY_HAT,           /* `code` = hat index, `hat_dir` = JCE_HAT_*   */
    JCE_SRC_COMPOSITE,         /* sub-bindings in comp[]                      */
    JCE_SRC_COUNT
} JceBindType;

/* Retained spellings.  Every one of these is the SAME source it always named;
 * only the integer moved, and only in memory. */
#define JCE_BIND_KEY           JCE_SRC_KEY
#define JCE_BIND_MOUSE_BTN     JCE_SRC_MOUSE_BUTTON
#define JCE_BIND_GAMEPAD_BTN   JCE_SRC_PAD_BUTTON
#define JCE_BIND_GAMEPAD_AXIS  JCE_SRC_PAD_AXIS
#define JCE_BIND_COMPOSITE     JCE_SRC_COMPOSITE

/* Which HALF of a bipolar axis feeds this action.  The evaluator in
 * jce_input_actions.c still does NOT read it; jce_input_bind_eval() (Task 10)
 * does, and is its first consumer.  jce_binding_init() and both JSON paths pin
 * it to FULL so a schema-1 map keeps schema-1 behaviour on either evaluator. */
typedef enum {
    JCE_AXIS_SIDE_FULL     = 0,   /* signed, schema-1 semantics       */
    JCE_AXIS_SIDE_POS      = 1,   /* + half only, reported as 0..1    */
    JCE_AXIS_SIDE_NEG      = 2,   /* - half only, reported as 0..1    */
    JCE_AXIS_SIDE_UNIPOLAR = 3    /* source is already 0..1 (triggers)*/
} JceAxisSide;

/* Which channel of the action this binding writes. */
typedef enum { JCE_CHAN_SCALAR = 0, JCE_CHAN_X = 1, JCE_CHAN_Y = 2 } JceBindChannel;

#define JCE_BINDF_RAW      0x0001u  /* bypass deadzone / saturation / curve   */
#define JCE_BINDF_DELTA    0x0002u  /* per-frame delta -> jce_action_delta()  */
#define JCE_BIND_PAIR_NONE 0xFFu    /* no companion axis: shaping is per-axis */

/* Composite binding flavours.  Stored in JceBinding.code when the binding
 * type is JCE_SRC_COMPOSITE. */
typedef enum {
    JCE_COMPOSITE_AXIS_1D   = 0, /* pos/neg keys  -> scalar (-1..+1)       */
    JCE_COMPOSITE_VECTOR_2D = 1, /* up/down/left/right keys -> (x,y) raw   */
} JceCompositeKind;

/* A composite sub-binding.  These were four bare ints hardwired to
 * jce_input_key_down(), so authoring the D-pad codes 11..14 silently bound
 * the letters H, I, J and K.  `type` is what makes "absent" distinguishable
 * from "code 0", which is what lets pad button 0 (SOUTH) be authored at all.
 *
 * IN MEMORY ONLY, and the distinction stops at the disk edge.  Schema 1 stores
 * one bare int per slot and has no room for a type, so jce_actions_save_file()
 * writes a sub-source that is not JCE_SRC_KEY as 0 ("absent") and logs WARN --
 * it does NOT write the code, because the loader would re-type it as a key and
 * reproduce the very bug above.  Until Plan B Batch 4 ships the v2 writer, a
 * typed pad sub-source therefore survives jce_action_bind/_bind_at but NOT a
 * save/load round trip, and pad button 0 is authorable in memory only.
 *
 * evaluate_composite() in jce_input_actions.c currently resolves ONLY
 * JCE_SRC_KEY sub-sources and contributes nothing for any other type; the
 * typed pad / axis sub-sources arrive with Plan B Task 12. */
typedef struct JceInputSource {
    int16_t type;      /* JceBindType, never JCE_SRC_COMPOSITE */
    int16_t side;      /* JceAxisSide, for axis sub-sources     */
    int32_t code;
} JceInputSource;

#define JCE_COMP_POS  0
#define JCE_COMP_NEG  1
#define JCE_COMP_UP   2
#define JCE_COMP_DOWN 3

/* The size prefix a v2 receiver requires.  Every v2 field is required by a v2
 * receiver, so the prefix IS the whole record -- it is not an offset into it.
 * jce_input_actions.c carries a JCE_SASSERT that breaks the BUILD if this
 * stops equalling sizeof(JceBinding); do not compute it by adding members. */
#define JCE_BINDING_SIZE_V2 ((uint32_t)76)

/* A single physical binding.
 *
 * WHICH FIELDS ARE READ TODAY, stated plainly so this comment is not a
 * contract nothing enforces.  There are now TWO evaluators and they read
 * different sets, so naming only one of them is how this paragraph goes stale:
 *
 *   jce_input_actions.c's evaluate_binding() -- the one jce_actions_update()
 *     runs, and therefore the only one anything a player touches goes through
 *     -- reads `type`, `code`, `scale`, `deadzone_inner`, `device_group` and
 *     `comp[]`.  That set has not grown.
 *   jce_input_bind_eval() (engine/src/os/platform/jce_input_bind_eval.c, Plan B
 *     Task 10, IN THE TREE) reads all of the above plus `side`, `channel`,
 *     `pair_axis`, `hat_dir`, `curve`, `flags`, `player`, `device_slot` and
 *     `deadzone_outer`.  So those nine are no longer read by nothing; what they
 *     still lack is a reader on the ACTION path, and that is Task 11's switch of
 *     jce_actions_update() onto jce_input_bind_eval().
 *
 * Until that switch lands, authoring one of the nine changes what
 * jce_input_bind_eval() reports and does NOT change what jce_action_value()
 * returns.  jce_binding_init() gives every one of them a defined value, so
 * nothing depends on a caller's stack garbage either way. */
typedef struct JceBinding {
    uint32_t       size;             /* sizeof(JceBinding); set by the caller.
                                        jce_binding_init() stamps it, and
                                        jce_action_bind() REFUSES anything
                                        below JCE_BINDING_SIZE_V2            */
    int32_t        type;             /* JceBindType                           */
    int32_t        code;             /* scancode / button / axis / hat index;
                                        for COMPOSITE a JceCompositeKind      */
    float          scale;            /* multiplier; EXACTLY 0 silences the
                                        binding on both the analog and the
                                        digital path                          */
    float          deadzone_inner;   /* < 0 == "use the device profile".  ZERO
                                        IS AUTHORABLE and is never rewritten  */
    float          deadzone_outer;   /* saturation; < 0 == device profile     */
    float          curve;            /* response exponent, 1.0 == linear      */
    uint32_t       flags;            /* JCE_BINDF_*                           */
    int32_t        device_group;     /* JceInputDeviceGroup -- the SCHEME tag.
                                        Not a device class: KEYBOARD|MOUSE map
                                        to KBM, GAMEPAD|JOYSTICK to GAMEPAD,
                                        TOUCH to TOUCH                        */
    int8_t         player;           /* JCE_INPUT_PLAYER_NONE == the querying
                                        player.  NOTE it is (-1), not 0: a
                                        memset-and-fill caller would put every
                                        binding on player 0                   */
    uint8_t        device_slot;      /* Nth device of the matching class in
                                        that player's set; 0 == primary       */
    uint8_t        side;             /* JceAxisSide                           */
    uint8_t        channel;          /* JceBindChannel                        */
    uint8_t        pair_axis;        /* companion axis for radial shaping, or
                                        JCE_BIND_PAIR_NONE                    */
    uint8_t        hat_dir;          /* JCE_HAT_* for JCE_SRC_JOY_HAT         */
    uint8_t        reserved[2];
    JceInputSource comp[4];          /* JCE_COMP_POS / _NEG / _UP / _DOWN     */
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
   Returns true on success.

   REFUSES a binding whose `size` is below JCE_BINDING_SIZE_V2 -- a record the
   callee cannot interpret, because it has no way to know which fields the
   caller believes it filled.  `memset(&b, 0, sizeof b)` followed by two field
   assignments is exactly that record, and it is now rejected rather than
   half-read.  Call jce_binding_init() first. */
JCE_API bool jce_action_bind(JceInputActions *a, int action_id,
                             const JceBinding *binding);

/* Initialise a binding to the sane defaults for its source.  THE ONLY PLACE A
 * DEFAULT IS INVENTED -- the evaluator substitutes nothing, which is what
 * makes `deadzone_inner = 0` mean zero instead of meaning "unset".
 *   key / mouse button : no dead region, scale 1, curve 1
 *   pad stick axis     : deadzone -1 (device profile), paired with its
 *                        companion axis so shaping is radial
 *   pad trigger axis   : side UNIPOLAR, deadzone -1 (device profile)
 *   raw joystick axis  : deadzone -1, UNPAIRED (a wheel's axis 1 is a pedal)
 *   mouse axis         : DELTA | RAW -- px/frame
 * `b->size` is set to sizeof(JceBinding).  A NULL `b` is a no-op. */
JCE_API void jce_binding_init(JceBinding *b, JceBindType type, int code);

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
   Returns false when out of range.

   BUFFER CONTRACT, and it is NOT the one jce_action_bind() above follows.
   `out` is a pure OUT parameter: the caller does NOT pre-set `out->size`, the
   callee never reads it, and the callee writes exactly sizeof(JceBinding)
   bytes.  `*out` must therefore be a JceBinding from THIS header.

   Why bind_at is exempt from the §3.0 receiver rule that its sibling
   JceInputDeviceInfo applies in both directions (jce_input_device.h):
     - The rule protects a caller that can DECLARE its size.  The pre-v2
       JceBinding has no `size` member at all -- its first member is `type` --
       so reading `out->size` from a stale caller reads its binding TYPE, and
       the "contract" would be a coincidence, not a declaration.
     - Applying it here would change the calling convention of an API that
       predates Plan B WITHOUT changing its signature, so no existing caller
       would be told by the compiler; an un-updated caller would hand over
       whatever its stack held.  jce_input_device_info() could adopt the rule
       cleanly because it shipped new, with every caller written to it.
   A caller compiled against a different JceBinding must be REBUILT
   (`jce.py sdk`), not relinked -- the same requirement JCE_ACTION_MAX_BINDS
   states above, and for the same reason: this record's size is baked into the
   caller's own arrays. */
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
 * For a JCE_SRC_COMPOSITE 2D-vector action this returns the magnitude of the
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

/* The part of this frame's value contributed by ONE device family.
 *
 * jce_action_value() sums every binding on the action, so a caller that
 * already reads the keyboard through some other route (the editor reads it
 * through ImGui, so that Play only moves while the Game window has focus)
 * cannot use it without double-counting.  This splits the same sum by the
 * family a binding INTRINSICALLY belongs to -- decided by JceBinding.type,
 * not by the `device_group` scheme tag, which is a separate concept and is
 * JCE_DEVICE_NONE on most maps:
 *   JCE_DEVICE_GAMEPAD  <- JCE_SRC_PAD_BUTTON, JCE_SRC_PAD_AXIS,
 *                          JCE_SRC_PAD_STICK, JCE_SRC_JOY_BUTTON,
 *                          JCE_SRC_JOY_AXIS, JCE_SRC_JOY_HAT
 *   JCE_DEVICE_KBM      <- JCE_SRC_KEY, JCE_SRC_MOUSE_BUTTON,
 *                          JCE_SRC_MOUSE_AXIS, JCE_SRC_COMPOSITE
 * JCE_SRC_NONE resolves to neither.  Note that the raw-device sources are
 * classified here but contribute 0, because evaluate_binding() in
 * jce_input_actions.c has no arm for them; jce_input_bind_eval() (Task 10) does
 * evaluate them, and Task 11's switch is what makes that reach this function.
 * Nothing in this tree authors them.
 * Any other `group` (including JCE_DEVICE_NONE and JCE_DEVICE_TOUCH) resolves
 * to 0.  Values keep their SIGN and their analog magnitude, so an axis bound
 * with scale -1 reports -1 when the stick is pulled the other way; the result
 * is clamped to -1..1 exactly like jce_action_value().  Bindings excluded by
 * the active control scheme contribute 0 here too. */
JCE_API float jce_action_value_device(const JceInputActions *a, int action_id,
                                      int device_group);

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

/* Add the canonical GAMEPAD bindings to an action map that has none.
 *
 * jce_actions_bind_fps_defaults() only ever runs when every load path fails,
 * so a map authored on disk -- and every map the editor's Input Manager has
 * ever written, because its bind editor is keyboard-only -- reaches the
 * runtime with keyboard bindings and nothing else.  A plugged-in pad is then
 * dead through no fault of the hardware.  This closes that gap IN MEMORY,
 * after load; it never writes to the user's file and never touches keyboard
 * bindings.
 *
 * Deliberately conservative -- it does nothing at all when:
 *   - the map ALREADY has any gamepad binding on any action.  An author who
 *     bound a pad knows pads exist, so their omissions are choices; this is
 *     also what makes the call IDEMPOTENT (the second call sees the bindings
 *     the first one added and returns 0).
 *   - the map opted out with top-level "gamepad_defaults": false in its JSON.
 *     That is the escape hatch for a map that wants to be pad-free even
 *     though it has no pad binding to prove it.
 *   - an action would exceed JCE_ACTION_MAX_BINDS.
 * Bindings are added ONLY to canonical actions the map already registered:
 * an action the author deleted stays deleted, never resurrected.
 *
 * Movement note: only "move_forward" and "move_right" get a stick axis, and
 * they are SIGNED (pulling left/back gives a negative value) -- there is no
 * separate "move_back"/"move_left" stick binding, because a boolean reader
 * would then see both halves active at once.  Read movement with
 * jce_action_value()/jce_action_value_device(), NOT jce_action_down(), whose
 * `value != 0` test reports a full-back stick as "forward is down".
 *
 * Returns the number of bindings added (0 when it declined). */
JCE_API int jce_actions_merge_gamepad_defaults(JceInputActions *a);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_ACTIONS_H */
