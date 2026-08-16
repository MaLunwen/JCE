/*
 * jce_input_actions.c  Action-based input implementation.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_actions"

/* C99-safe compile-time assertion: a negative-size array typedef.  JCE_C11 is
 * not defined anywhere in this tree, so _Static_assert is not available.  Same
 * idiom as jce_input_devices.c, and it belongs in a .c rather than the public
 * header so a consumer never inherits a typedef of ours. */
#define JCE_SASSERT(cond, tag)  typedef char jce_sa_##tag[(cond) ? 1 : -1]

/* JCE_BINDING_SIZE_V2 is the prefix every v2 receiver gates on, and it is a
 * frozen number in a public header.  Append a field to JceBinding without
 * introducing a V3 and jce_action_bind() would silently start admitting
 * records too short for the struct they name.  This line makes that edit fail
 * to COMPILE on every platform, including the ones the unit suite never runs
 * on -- which is also how 76 was established rather than guessed: the design
 * wrote 56, which is offsetof(JceBinding, comp) + 12, i.e. the middle of
 * comp[1]. */
JCE_SASSERT(sizeof(JceBinding) == (size_t)JCE_BINDING_SIZE_V2, binding_v2_size);
JCE_SASSERT(sizeof(JceInputSource) == 8u, input_source_size);
#undef JCE_SASSERT

/* ================================================================== */
/* Internals                                                           */
/* ================================================================== */

typedef struct {
    char       name[32];
    JceBinding binds[JCE_ACTION_MAX_BINDS];
    int        bind_count;
    float      value;           /* this frame (scalar/magnitude) */
    float      prev_value;      /* last frame */
    float      value_x;         /* this frame, X of composite 2D vector */
    float      value_y;         /* this frame, Y of composite 2D vector */
    /* The same sum split by the device family a binding belongs to, so a
     * caller already reading one family through another route can take the
     * other without double-counting (jce_action_value_device). */
    float      value_kbm;       /* keyboard + mouse + key-composite binds */
    float      value_pad;       /* gamepad button + axis binds            */
} ActionEntry;

typedef struct {
    char     name[JCE_SCHEME_NAME_MAX];
    unsigned device_mask;       /* OR of JCE_DEVICE_BIT(group) */
} SchemeEntry;

struct JceInputActions {
    ActionEntry actions[JCE_ACTION_MAX];
    int         count;

    /* Control schemes.  scheme_count == 0 => legacy "all bindings active". */
    SchemeEntry schemes[JCE_SCHEME_MAX];
    int         scheme_count;
    int         active_scheme;  /* -1 when no schemes defined            */
    bool        auto_switch;    /* last-used-device auto switching        */
    int         last_device;    /* JceInputDeviceGroup of most recent in */

    /* Opt-out for jce_actions_merge_gamepad_defaults(): a map that carries
     * top-level "gamepad_defaults": false wants to stay pad-free even though
     * it has no gamepad binding to prove it.  Default true. */
    bool        allow_gamepad_defaults;
};

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceInputActions *jce_actions_create(void)
{
    JceInputActions *a = JCE_NEW(JceInputActions);
    if (!a) return NULL;
    a->count = 0;
    memset(a->actions, 0, sizeof(a->actions));
    memset(a->schemes, 0, sizeof(a->schemes));
    a->scheme_count  = 0;
    a->active_scheme = -1;
    a->auto_switch   = true;
    a->last_device   = JCE_DEVICE_NONE;
    a->allow_gamepad_defaults = true;
    return a;
}

void jce_actions_destroy(JceInputActions *a)
{
    JCE_FREE(a);
}

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

int jce_action_register(JceInputActions *a, const char *name)
{
    if (!a || !name || a->count >= JCE_ACTION_MAX) return -1;

    /* Check duplicates. */
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->actions[i].name, name) == 0)
            return -1;
    }

    int id = a->count++;
    ActionEntry *e = &a->actions[id];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->bind_count = 0;
    e->value      = 0;
    e->prev_value = 0;
    e->value_x    = 0;
    e->value_y    = 0;
    e->value_kbm  = 0;
    e->value_pad  = 0;
    return id;
}

void jce_binding_init(JceBinding *b, JceBindType type, int code)
{
    if (!b) return;

    memset(b, 0, sizeof *b);
    b->size           = (uint32_t)sizeof(JceBinding);
    b->type           = (int32_t)type;
    b->code           = code;
    b->scale          = 1.0f;
    b->curve          = 1.0f;
    b->deadzone_inner = 0.0f;      /* digital sources have no dead region */
    b->deadzone_outer = 1.0f;
    b->device_group   = JCE_DEVICE_NONE;
    /* JCE_INPUT_PLAYER_NONE is (-1), not 0.  The memset above would otherwise
     * put every binding on player 0, which passes every test today and
     * surfaces later as "both pads drive the same character". */
    b->player         = (int8_t)JCE_INPUT_PLAYER_NONE;
    b->side           = (uint8_t)JCE_AXIS_SIDE_FULL;
    b->channel        = (uint8_t)JCE_CHAN_SCALAR;
    b->pair_axis      = JCE_BIND_PAIR_NONE;
    b->hat_dir        = JCE_HAT_CENTERED;

    switch (type) {
    case JCE_SRC_PAD_AXIS:
        /* Deferring to the device profile is what lets one project-level
         * dead-zone setting move every stick at once. */
        b->deadzone_inner = -1.0f;
        b->deadzone_outer = -1.0f;
        if (code == JCE_GAMEPAD_AXIS_LEFT_TRIGGER ||
            code == JCE_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            b->side = (uint8_t)JCE_AXIS_SIDE_UNIPOLAR;
        } else if (code == JCE_GAMEPAD_AXIS_LEFTX) {
            b->pair_axis = (uint8_t)JCE_GAMEPAD_AXIS_LEFTY;
        } else if (code == JCE_GAMEPAD_AXIS_LEFTY) {
            b->pair_axis = (uint8_t)JCE_GAMEPAD_AXIS_LEFTX;
        } else if (code == JCE_GAMEPAD_AXIS_RIGHTX) {
            b->pair_axis = (uint8_t)JCE_GAMEPAD_AXIS_RIGHTY;
        } else if (code == JCE_GAMEPAD_AXIS_RIGHTY) {
            b->pair_axis = (uint8_t)JCE_GAMEPAD_AXIS_RIGHTX;
        }
        break;

    case JCE_SRC_PAD_STICK:
        b->deadzone_inner = -1.0f;
        b->deadzone_outer = -1.0f;
        b->channel        = (uint8_t)JCE_CHAN_X;
        break;

    case JCE_SRC_JOY_AXIS:
        /* Deliberately UNPAIRED: for a wheel, axis 0 is steering and axis 1 is
         * usually a pedal, and pairing them radially makes the pedal attenuate
         * the steering.  A HOTAS whose axes really are a stick authors it. */
        b->deadzone_inner = -1.0f;
        b->deadzone_outer = -1.0f;
        break;

    case JCE_SRC_MOUSE_AXIS:
        /* px/frame, already relative: shaping it makes no sense. */
        b->flags = JCE_BINDF_DELTA | JCE_BINDF_RAW;
        break;

    default:
        break;
    }
}

bool jce_action_bind(JceInputActions *a, int action_id,
                      const JceBinding *binding)
{
    if (!a || !binding) return false;
    if (action_id < 0 || action_id >= a->count) return false;

    /* Receiver rule.  A record whose declared size is below the v2 prefix
     * cannot be interpreted -- the callee has no way to know which fields the
     * caller believes it filled.  Refuse and say so; never copy the bytes that
     * happen to line up. */
    if (binding->size < JCE_BINDING_SIZE_V2) {
        LOG_WARN(LOG_TAG,
                 "jce_action_bind('%s'): binding size %u is below the v2 prefix "
                 "%u -- rejected.  Call jce_binding_init() first.",
                 a->actions[action_id].name, (unsigned)binding->size,
                 (unsigned)JCE_BINDING_SIZE_V2);
        return false;
    }

    ActionEntry *e = &a->actions[action_id];
    if (e->bind_count >= JCE_ACTION_MAX_BINDS) return false;

    /* Copy min(caller->size, sizeof) and zero-fill the remainder, so a future
     * larger record from a newer caller is truncated rather than misread. */
    size_t n = (size_t)binding->size;
    if (n > sizeof(JceBinding)) n = sizeof(JceBinding);
    JceBinding *dst = &e->binds[e->bind_count];
    memset(dst, 0, sizeof *dst);
    memcpy(dst, binding, n);
    dst->size = (uint32_t)sizeof(JceBinding);
    e->bind_count++;
    return true;
}

void jce_action_unbind_all(JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return;
    a->actions[action_id].bind_count = 0;
}

int jce_action_find(const JceInputActions *a, const char *name)
{
    if (!a || !name) return -1;
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->actions[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ================================================================== */
/* Control schemes                                                     */
/* ================================================================== */

int jce_action_scheme_register(JceInputActions *a, const char *name,
                               unsigned device_mask)
{
    if (!a || !name || !name[0]) return -1;
    if (a->scheme_count >= JCE_SCHEME_MAX) return -1;

    /* Reject duplicate names. */
    for (int i = 0; i < a->scheme_count; i++) {
        if (strcmp(a->schemes[i].name, name) == 0)
            return -1;
    }

    int id = a->scheme_count++;
    SchemeEntry *s = &a->schemes[id];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->device_mask = device_mask;

    /* The first scheme defined becomes the active one (transitions the map
     * out of the legacy "all bindings active" mode). */
    if (a->active_scheme < 0)
        a->active_scheme = id;

    return id;
}

int jce_action_scheme_count(const JceInputActions *a)
{
    return a ? a->scheme_count : 0;
}

int jce_action_scheme_find(const JceInputActions *a, const char *name)
{
    if (!a || !name) return -1;
    for (int i = 0; i < a->scheme_count; i++) {
        if (strcmp(a->schemes[i].name, name) == 0)
            return i;
    }
    return -1;
}

const char *jce_action_scheme_name(const JceInputActions *a, int scheme_id)
{
    if (!a || scheme_id < 0 || scheme_id >= a->scheme_count) return NULL;
    return a->schemes[scheme_id].name;
}

unsigned jce_action_scheme_mask(const JceInputActions *a, int scheme_id)
{
    if (!a || scheme_id < 0 || scheme_id >= a->scheme_count) return 0;
    return a->schemes[scheme_id].device_mask;
}

int jce_action_scheme_active(const JceInputActions *a)
{
    return a ? a->active_scheme : -1;
}

bool jce_action_scheme_set_active(JceInputActions *a, int scheme_id)
{
    if (!a || scheme_id < 0 || scheme_id >= a->scheme_count) return false;
    a->active_scheme = scheme_id;
    a->auto_switch   = false;   /* manual selection pins the scheme */
    return true;
}

void jce_action_scheme_set_auto(JceInputActions *a, bool enabled)
{
    if (!a) return;
    a->auto_switch = enabled;
}

bool jce_action_scheme_auto(const JceInputActions *a)
{
    return a ? a->auto_switch : false;
}

int jce_action_last_device(const JceInputActions *a)
{
    return a ? a->last_device : JCE_DEVICE_NONE;
}

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* MSVC's C4062 ("enumerator ... in switch of enum ... is not handled", for a
 * switch with no `default`) is OFF BY DEFAULT even at /W4, so casting the
 * operand back to JceBindType below buys nothing on the compiler that builds
 * this repo unless the warning is also turned on.  MEASURED, both ways:
 * deleting `case JCE_SRC_JOY_HAT:` from binding_device_family() and rebuilding
 * jce_platform emits no diagnostic at all without this pragma, and emits
 * "warning C4062: enumerator 'JCE_SRC_JOY_HAT' in switch of enum
 * 'JceBindType' is not handled" with it.  -Wall already carries -Wswitch on
 * Clang/GNU, which is why the cast is the whole fix there.
 *
 * Every other switch in this TU has a `default:`, so C4062 cannot reach them;
 * this enables the check for exactly the two switches that want it. */
#ifdef _MSC_VER
#pragma warning(4 : 4062)
#endif

/* Scalar contribution of a single non-composite binding.
 *
 * The switch operand is CAST to JceBindType on purpose.  JceBinding.type is
 * int32_t (the record is size-prefixed and goes to disk), and an integer
 * operand takes this switch out of -Wswitch / C4062's reach -- so the next
 * source added to JceBindType by Plan B Tasks 10-13 would resolve to 0 here
 * with nothing to say so.  The cast puts the compiler back on watch; the
 * `return 0` past the switch stays, because an int32_t read from disk can hold
 * a value no enumerator names. */
static float evaluate_binding(const JceBinding *b, const JceInput *input)
{
    switch ((JceBindType)b->type) {
    case JCE_SRC_KEY:
        return jce_input_key_down(input, (JceKey)b->code) ? b->scale : 0;

    case JCE_SRC_MOUSE_BUTTON:
        return jce_input_mouse_button(input, b->code) ? b->scale : 0;

    case JCE_SRC_PAD_BUTTON:
        /* "The gamepad" means THIS PLAYER's primary gamepad-layout device, not
         * an index among connected pads.  Under JCE_PAIRING_SINGLE_USER (the
         * default) every device is paired to slot 0, so a one-pad machine reads
         * exactly what pad index 0 read before.  The literal 0 is where the
         * querying player goes when per-player binding lands. */
        return jce_input_player_button(input, 0, (JceGamepadButton)b->code)
             ? b->scale : 0;

    case JCE_SRC_PAD_AXIS: {
        JceDeviceId pad = jce_input_player_device_of_class(
                              input, 0, JCE_DEVCLASS_GAMEPAD, 0);
        float v = jce_input_device_axis_raw(input, pad, (JceGamepadAxis)b->code);
        /* NEGATIVE means "the author did not say"; ZERO means the author said
         * zero and is honoured.  The old test was `deadzone > 0 ? deadzone :
         * 0.15f`, which made those two the same input and left `deadzone: 0`
         * -- the value an analog trigger wants -- literally unauthorable.
         * The device profile is not reachable from here.  deadzone_outer,
         * curve, side and pair_axis already HAVE their first reader --
         * jce_input_bind_eval(), Plan B Task 10, in the tree -- but this
         * function is still the one jce_actions_update() runs, so it is Plan B
         * Task 11 that replaces the arithmetic below, not Task 10.  THIS IS THE
         * SECOND COPY OF THE DEAD REGION and the line the evaluator's banner
         * points at; until Task 11 deletes it, it remaps inner..1 exactly as
         * schema 1 did. */
        float dz = b->deadzone_inner < 0.0f ? 0.15f : b->deadzone_inner;
        if (fabsf(v) < dz) return 0;
        /* Remap from deadzone..1 to 0..1 */
        float sign = v > 0 ? 1.0f : -1.0f;
        float remapped = (fabsf(v) - dz) / (1.0f - dz);
        return sign * remapped * b->scale;
    }

    case JCE_SRC_COMPOSITE:
        /* Composites resolve into (x,y) via evaluate_composite(); they make
         * no scalar contribution to the flat sum. */
        return 0;

    /* Authorable in v2, but no reader HERE: this evaluator can only reach
     * player 0's semantic gamepad and the keyboard.  jce_input_bind_eval()
     * (Plan B Task 10) evaluates every one of them and is in the tree -- but
     * evaluation has NOT moved: jce_actions_update() still calls this function,
     * so these still resolve to 0 on the action path.  Plan B Task 11 is the
     * switch that changes that.  Listed by name so that "silent 0" is a
     * decision the compiler saw, not an omission. */
    case JCE_SRC_MOUSE_AXIS:
    case JCE_SRC_PAD_STICK:
    case JCE_SRC_JOY_BUTTON:
    case JCE_SRC_JOY_AXIS:
    case JCE_SRC_JOY_HAT:
    case JCE_SRC_NONE:
    case JCE_SRC_COUNT:
        return 0;
    }
    return 0;   /* an int32_t from disk that names no enumerator */
}

/* The device family a binding INTRINSICALLY belongs to, decided by its type.
 * Distinct from JceBinding.device_group, which is the control-SCHEME tag an
 * author may or may not have set (it is JCE_DEVICE_NONE on most maps and so
 * cannot answer "is this a pad binding?").  Composites are key-built, so they
 * are keyboard.
 *
 * The switch operand is CAST to JceBindType for the reason evaluate_binding()
 * gives: `type` is int32_t, and without the cast the deliberately-absent
 * `default:` polices nothing.  Every enumerator is listed, so adding one to
 * JceBindType breaks the build here instead of classifying as
 * JCE_DEVICE_NONE. */
static int binding_device_family(const JceBinding *b)
{
    switch ((JceBindType)b->type) {
    case JCE_SRC_PAD_BUTTON:
    case JCE_SRC_PAD_AXIS:
    /* The raw-device sources are a pad family too.  Nothing in this tree
     * authors them yet, so listing them changes no behaviour today -- but
     * leaving them out would make jce_actions_merge_gamepad_defaults() think a
     * map full of raw joystick binds "has no pad binding" and overwrite it. */
    case JCE_SRC_PAD_STICK:
    case JCE_SRC_JOY_BUTTON:
    case JCE_SRC_JOY_AXIS:
    case JCE_SRC_JOY_HAT:
        return JCE_DEVICE_GAMEPAD;
    case JCE_SRC_KEY:
    case JCE_SRC_MOUSE_BUTTON:
    case JCE_SRC_MOUSE_AXIS:
    case JCE_SRC_COMPOSITE:
        return JCE_DEVICE_KBM;
    case JCE_SRC_NONE:
    case JCE_SRC_COUNT:
        return JCE_DEVICE_NONE;
    }
    return JCE_DEVICE_NONE;   /* an int32_t naming no enumerator */
}

/* (x,y) contribution of a single composite binding.  Raw (un-normalized):
 * a 2D WASD composite with up+right held yields (+1,+1).  The result is
 * pre-scaled by b->scale so callers can invert/amplify axes per-binding. */
static void evaluate_composite(const JceBinding *b, const JceInput *input,
                               float *out_x, float *out_y)
{
    float x = 0.0f, y = 0.0f;
    /* Use b->scale directly: the JSON loader defaults it to 1.0 when the key
     * is absent, so a legitimately-authored scale of 0 must be honored (it
     * silences the binding) rather than coerced back to 1.0. */
    float scale = b->scale;

    /* The sub-source TYPE decides presence now, not a non-zero code.  As four
     * bare ints these were hardwired to jce_input_key_down(), so authoring the
     * D-pad codes 11..14 silently bound the letters H, I, J and K -- and code
     * 0 could never be authored at all.  Only JCE_SRC_KEY resolves here; a
     * typed pad sub-source contributes nothing until Plan B Task 12. */

    /* X axis: positive (right / pos) minus negative (left / neg). */
    if (b->comp[JCE_COMP_POS].type == JCE_SRC_KEY &&
        jce_input_key_down(input, (JceKey)b->comp[JCE_COMP_POS].code))
        x += 1.0f;
    if (b->comp[JCE_COMP_NEG].type == JCE_SRC_KEY &&
        jce_input_key_down(input, (JceKey)b->comp[JCE_COMP_NEG].code))
        x -= 1.0f;

    /* Y axis (2D only): up minus down.  For a 1D axis up/down stay
     * JCE_SRC_NONE. */
    if (b->comp[JCE_COMP_UP].type == JCE_SRC_KEY &&
        jce_input_key_down(input, (JceKey)b->comp[JCE_COMP_UP].code))
        y += 1.0f;
    if (b->comp[JCE_COMP_DOWN].type == JCE_SRC_KEY &&
        jce_input_key_down(input, (JceKey)b->comp[JCE_COMP_DOWN].code))
        y -= 1.0f;

    *out_x = x * scale;
    *out_y = y * scale;
}

/* Scan the input frame for any activity per device group, returning a bitmask
 * of JCE_DEVICE_BIT(group) for groups that produced fresh input this frame.
 * This is independent of the action map so auto-switch reacts even to inputs
 * not currently bound. */
static unsigned scan_active_device_groups(const JceInput *input)
{
    unsigned mask = 0;

    /* Keyboard / mouse. */
    for (int sc = 0; sc < JCE_KEY_COUNT; ++sc) {
        if (jce_input_key_down(input, (JceKey)sc)) {
            mask |= JCE_DEVICE_BIT(JCE_DEVICE_KBM);
            break;
        }
    }
    if (!(mask & JCE_DEVICE_BIT(JCE_DEVICE_KBM))) {
        for (int mb = 1; mb <= 5; ++mb) {
            if (jce_input_mouse_button(input, mb)) {
                mask |= JCE_DEVICE_BIT(JCE_DEVICE_KBM);
                break;
            }
        }
    }

    /* Gamepad: any button on player 0's primary gamepad-layout device, or any
     * axis past a nominal deadzone.  The device lookup is hoisted out of the
     * axis loop -- it walks the table, and the answer cannot change between
     * iterations. */
    for (int bn = 0; bn < JCE_GAMEPAD_BUTTON_COUNT; ++bn) {
        if (jce_input_player_button(input, 0, bn)) {
            mask |= JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD);
            break;
        }
    }
    if (!(mask & JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD))) {
        JceDeviceId pad = jce_input_player_device_of_class(
                              input, 0, JCE_DEVCLASS_GAMEPAD, 0);
        for (int ax = 0; ax < JCE_GAMEPAD_AXIS_COUNT; ++ax) {
            if (fabsf(jce_input_device_axis_raw(input, pad, ax)) >= 0.5f) {
                mask |= JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD);
                break;
            }
        }
    }

    /* Touch. */
    if (jce_input_touch_count(input) > 0)
        mask |= JCE_DEVICE_BIT(JCE_DEVICE_TOUCH);

    return mask;
}

/* Decide whether a binding resolves under the current active scheme.  With no
 * schemes defined every binding resolves (legacy behavior).  Otherwise a
 * binding resolves when it is device-agnostic (JCE_DEVICE_NONE) or its device
 * group is contained in the active scheme's device mask. */
static int binding_in_active_scheme(const JceInputActions *a,
                                    const JceBinding *b)
{
    if (a->scheme_count == 0) return 1;           /* legacy: all active */
    if (b->device_group == JCE_DEVICE_NONE) return 1;
    if (a->active_scheme < 0) return 0;
    unsigned m = a->schemes[a->active_scheme].device_mask;
    return (m & JCE_DEVICE_BIT(b->device_group)) != 0;
}

/* Auto last-used-device switching.  When enabled and schemes are defined,
 * switch the active scheme to whichever scheme owns a device group that
 * produced fresh input this frame.  Preference order: keep the current scheme
 * if it still has activity (avoids thrashing when both devices are touched),
 * else pick the first scheme that owns a freshly-active group. */
static void apply_auto_switch(JceInputActions *a, unsigned active_groups)
{
    if (a->scheme_count == 0) return;

    /* Record the last device that produced input regardless of auto state, so
     * games can read jce_action_last_device() for button prompts. */
    for (int g = JCE_DEVICE_GROUP_COUNT - 1; g > JCE_DEVICE_NONE; --g) {
        if (active_groups & JCE_DEVICE_BIT(g)) {
            a->last_device = g;
            break;
        }
    }

    if (!a->auto_switch || active_groups == 0) return;

    /* If the currently-active scheme still has fresh input, keep it. */
    if (a->active_scheme >= 0) {
        unsigned m = a->schemes[a->active_scheme].device_mask;
        if (m & active_groups) return;
    }

    /* Otherwise switch to the first scheme owning a freshly-active group. */
    for (int s = 0; s < a->scheme_count; ++s) {
        if (a->schemes[s].device_mask & active_groups) {
            a->active_scheme = s;
            return;
        }
    }
}

void jce_actions_update(JceInputActions *a, const JceInput *input)
{
    if (!a || !input) return;

    /* Resolve last-used device + auto scheme switch BEFORE evaluating values,
     * so this frame's input is read through the freshly-selected scheme. */
    unsigned active_groups = scan_active_device_groups(input);
    if (a->scheme_count == 0) {
        /* Still track the last device even without schemes (button prompts). */
        for (int g = JCE_DEVICE_GROUP_COUNT - 1; g > JCE_DEVICE_NONE; --g) {
            if (active_groups & JCE_DEVICE_BIT(g)) { a->last_device = g; break; }
        }
    } else {
        apply_auto_switch(a, active_groups);
    }

    for (int i = 0; i < a->count; i++) {
        ActionEntry *e = &a->actions[i];
        e->prev_value = e->value;

        /* Accumulate scalar bindings (allows e.g. W + left-stick both) and
         * composite (x,y) bindings into a single resolved vector. */
        float total = 0;
        float vx = 0, vy = 0;
        int   has_composite = 0;
        int   has_vector_2d = 0;   /* any composite DECLARED as 2D vector */
        float total_kbm = 0, total_pad = 0;

        for (int b = 0; b < e->bind_count; b++) {
            const JceBinding *bd = &e->binds[b];
            /* Control-scheme gate: only resolve bindings that belong to the
             * active scheme (or are device-agnostic).  No-op when no schemes
             * are defined. */
            if (!binding_in_active_scheme(a, bd))
                continue;
            if (bd->type == JCE_BIND_COMPOSITE) {
                float cx = 0, cy = 0;
                evaluate_composite(bd, input, &cx, &cy);
                vx += cx;
                vy += cy;
                has_composite = 1;
                if (bd->code == JCE_COMPOSITE_VECTOR_2D)
                    has_vector_2d = 1;
                /* A composite contributes no scalar to `total`; for the
                 * per-family split it is keyboard, and the scalar it would
                 * stand for is folded in below with the same rule `total`
                 * uses (magnitude for 2D, signed x for 1D). */
            } else {
                float v = evaluate_binding(bd, input);
                total += v;
                if (binding_device_family(bd) == JCE_DEVICE_GAMEPAD)
                    total_pad += v;
                else
                    total_kbm += v;
            }
        }

        if (has_composite) {
            /* Raw (un-normalized) per-axis sum is exposed via value2.
             * The scalar `value` keeps composite actions working with
             * jce_action_down()/pressed().  Classify by the binding's
             * DECLARED kind (not by whether vy happens to be 0 this frame):
             *   - 2D vector -> vector magnitude (always >= 0), so a LEFT-only
             *     press yields +1 magnitude, never a negative scalar.
             *   - 1D axis   -> signed vx so left/right keeps its sign.
             * Scalar bindings on the same action still add into `total`. */
            e->value_x = vx;
            e->value_y = vy;

            float comp_scalar = has_vector_2d
                ? sqrtf(vx * vx + vy * vy)   /* 2D: magnitude     */
                : vx;                        /* 1D axis: keep sign */
            total     += comp_scalar;
            total_kbm += comp_scalar;        /* composites are key-built */
        } else {
            e->value_x = total;
            e->value_y = 0.0f;
        }

        /* Clamp scalar to -1..1 — the per-family splits the same way, so a
         * caller summing them back never exceeds the range either. */
        if (total > 1.0f)  total = 1.0f;
        if (total < -1.0f) total = -1.0f;
        if (total_kbm > 1.0f)  total_kbm = 1.0f;
        if (total_kbm < -1.0f) total_kbm = -1.0f;
        if (total_pad > 1.0f)  total_pad = 1.0f;
        if (total_pad < -1.0f) total_pad = -1.0f;

        e->value     = total;
        e->value_kbm = total_kbm;
        e->value_pad = total_pad;
    }
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

float jce_action_value(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return 0;
    return a->actions[action_id].value;
}

void jce_action_value2(const JceInputActions *a, int action_id,
                       JceActionVec2 *out)
{
    if (!out) return;
    if (!a || action_id < 0 || action_id >= a->count) {
        out->x = 0.0f;
        out->y = 0.0f;
        return;
    }
    const ActionEntry *e = &a->actions[action_id];
    out->x = e->value_x;
    out->y = e->value_y;
}

bool jce_action_pressed(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return false;
    const ActionEntry *e = &a->actions[action_id];
    return (e->value != 0) && (e->prev_value == 0);
}

bool jce_action_down(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return false;
    return a->actions[action_id].value != 0;
}

bool jce_action_released(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return false;
    const ActionEntry *e = &a->actions[action_id];
    return (e->value == 0) && (e->prev_value != 0);
}

float jce_action_value_device(const JceInputActions *a, int action_id,
                              int device_group)
{
    if (!a || action_id < 0 || action_id >= a->count) return 0;
    const ActionEntry *e = &a->actions[action_id];
    if (device_group == JCE_DEVICE_GAMEPAD) return e->value_pad;
    if (device_group == JCE_DEVICE_KBM)     return e->value_kbm;
    return 0;   /* NONE / TOUCH: no binding type maps here */
}

/* ================================================================== */
/* Enumeration (read accessors for tools / the editor Input Manager)   */
/* ================================================================== */

int jce_actions_count(const JceInputActions *a)
{
    return a ? a->count : 0;
}

const char *jce_action_name(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return NULL;
    return a->actions[action_id].name;
}

int jce_action_bind_count(const JceInputActions *a, int action_id)
{
    if (!a || action_id < 0 || action_id >= a->count) return 0;
    return a->actions[action_id].bind_count;
}

bool jce_action_bind_at(const JceInputActions *a, int action_id,
                        int bind_index, JceBinding *out)
{
    if (!a || !out) return false;
    if (action_id < 0 || action_id >= a->count) return false;
    const ActionEntry *e = &a->actions[action_id];
    if (bind_index < 0 || bind_index >= e->bind_count) return false;
    *out = e->binds[bind_index];
    return true;
}

/* ================================================================== */
/* Default FPS bindings                                                */
/* ================================================================== */

/*
 * Canonical default action table — the SINGLE source of truth shared by
 * the runtime fallback (jce_default_main) and the editor's Input Manager
 * seed (which copies this table instead of keeping its own).
 *
 * Convention changes (deliberate, consolidation of two divergent tables):
 *   - "sprint" moved LCTRL -> LSHIFT (matches the editor seed and the
 *     dm_act_down("sprint", JCE_KEY_LSHIFT) fallback in default_main).
 *   - SPACE moved "move_up" -> "jump" (gameplay consumes "jump":
 *     default_main and the editor Game view both read it).
 *   - "move_up"/"move_down" stay registered for fly-style apps but ship
 *     UNBOUND by default.
 */

/* The GAMEPAD half of the canonical table, lifted into data so that
 * jce_actions_bind_fps_defaults() (fresh map) and
 * jce_actions_merge_gamepad_defaults() (map loaded from disk) can never
 * describe a different pad.  One table, two readers.
 *
 * There is deliberately NO entry for "move_back" or "move_left": the stick is
 * one SIGNED axis per pair, so "move_forward" at -1 IS backward.  Adding the
 * mirror binds would make both halves of a pair report active at full deflec-
 * tion, which is worse than the gap it looks like.  Consumers must therefore
 * read movement as a signed value, never through jce_action_down(). */
typedef struct {
    const char *action;
    JceBindType type;
    int         code;
    float       scale;
    float       deadzone;   /* schema-1 inner deadzone; -> deadzone_inner */
} DefaultPadBind;

/* One binding from the table above, built through jce_binding_init() so the
 * size prefix and every v2 field it does not name get their defined value.
 *
 * The literal deadzones are then written over jce_binding_init()'s -1 ("use
 * the device profile") on purpose: this commit changes the SPELLING of the
 * canonical table, not its numbers, so a map saved from these defaults stays
 * byte-identical.  Plan B Batch 4 rewrites the table itself. */
static JceBinding default_pad_binding(const DefaultPadBind *d)
{
    JceBinding b;
    jce_binding_init(&b, d->type, d->code);
    b.scale          = d->scale;
    b.deadzone_inner = d->deadzone;
    b.deadzone_outer = 1.0f;
    b.side           = (uint8_t)JCE_AXIS_SIDE_FULL;
    b.pair_axis      = JCE_BIND_PAIR_NONE;
    return b;
}

static const DefaultPadBind k_default_pad_binds[] = {
    { "move_forward", JCE_BIND_GAMEPAD_AXIS, JCE_GAMEPAD_AXIS_LEFTY,        -1.0f, 0.15f },
    { "move_right",   JCE_BIND_GAMEPAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX,         1.0f, 0.15f },
    { "jump",         JCE_BIND_GAMEPAD_BTN,  JCE_GAMEPAD_BUTTON_SOUTH,       1.0f, 0.0f  },
    { "sprint",       JCE_BIND_GAMEPAD_BTN,  JCE_GAMEPAD_BUTTON_LEFT_STICK,  1.0f, 0.0f  },
    { "look_x",       JCE_BIND_GAMEPAD_AXIS, JCE_GAMEPAD_AXIS_RIGHTX,        1.0f, 0.15f },
    { "look_y",       JCE_BIND_GAMEPAD_AXIS, JCE_GAMEPAD_AXIS_RIGHTY,       -1.0f, 0.15f },
};
#define K_DEFAULT_PAD_BIND_COUNT \
    ((int)(sizeof(k_default_pad_binds) / sizeof(k_default_pad_binds[0])))

int jce_actions_bind_fps_defaults(JceInputActions *a)
{
    if (!a) return -1;

    int first = a->count;

    int fwd   = jce_action_register(a, "move_forward");
    int back  = jce_action_register(a, "move_back");
    int left  = jce_action_register(a, "move_left");
    int right = jce_action_register(a, "move_right");
    int jump  = jce_action_register(a, "jump");
    int sprint= jce_action_register(a, "sprint");
    int up    = jce_action_register(a, "move_up");
    int down  = jce_action_register(a, "move_down");
    int lx    = jce_action_register(a, "look_x");
    int ly    = jce_action_register(a, "look_y");

    /* Keyboard: WASD + Space jump + LShift sprint.
     *
     * Positional initialisation is gone and that is the point: with `size`
     * first, `(JceBinding){ JCE_BIND_KEY, JCE_KEY_W, 1.0f, 0 }` would put the
     * SOURCE into the size field and jce_action_bind() would refuse it. */
    JceBinding kb;
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_W);      jce_action_bind(a, fwd,    &kb);
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_S);      jce_action_bind(a, back,   &kb);
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_A);      jce_action_bind(a, left,   &kb);
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_D);      jce_action_bind(a, right,  &kb);
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_SPACE);  jce_action_bind(a, jump,   &kb);
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_LSHIFT); jce_action_bind(a, sprint, &kb);

    /* Gamepad: left stick = move, right stick = look,
     * South (A) = jump, L3 (left-stick click) = sprint.  Read from the one
     * table above so the fresh-map and merge-on-load paths cannot diverge. */
    for (int i = 0; i < K_DEFAULT_PAD_BIND_COUNT; ++i) {
        const DefaultPadBind *d = &k_default_pad_binds[i];
        int id = jce_action_find(a, d->action);
        if (id < 0) continue;
        JceBinding b = default_pad_binding(d);
        jce_action_bind(a, id, &b);
    }

    (void)fwd; (void)back; (void)left; (void)right;
    (void)jump; (void)sprint; (void)lx; (void)ly;
    (void)up; (void)down;   /* registered, deliberately unbound */

    return first;
}

int jce_actions_merge_gamepad_defaults(JceInputActions *a)
{
    if (!a) return 0;
    if (!a->allow_gamepad_defaults) return 0;   /* explicit opt-out */

    /* Hands off a map that already speaks gamepad.  This is BOTH the respect
     * for an author's deliberate omissions AND what makes the call idempotent:
     * the second call sees what the first one added. */
    for (int i = 0; i < a->count; ++i) {
        const ActionEntry *e = &a->actions[i];
        for (int b = 0; b < e->bind_count; ++b) {
            if (binding_device_family(&e->binds[b]) == JCE_DEVICE_GAMEPAD)
                return 0;
        }
    }

    int added = 0;
    for (int i = 0; i < K_DEFAULT_PAD_BIND_COUNT; ++i) {
        const DefaultPadBind *d = &k_default_pad_binds[i];
        /* Only actions the map ALREADY registered: an action the author
         * deleted stays deleted. */
        int id = jce_action_find(a, d->action);
        if (id < 0) continue;
        JceBinding b = default_pad_binding(d);
        if (jce_action_bind(a, id, &b))   /* false when the action is full */
            added++;
    }
    return added;
}

/* ── schema-1 source ints <-> JceBindType ─────────────────────────────
 *
 * Schema 1 froze the source on disk as 0 KEY, 1 MOUSE_BTN, 2 GAMEPAD_BTN,
 * 3 GAMEPAD_AXIS, 4 COMPOSITE.  v2 renumbers in memory because it needed a
 * NONE at zero.  These two functions are the ONLY place the two numberings
 * meet; Plan B Batch 4 moves them into jce_input_actions_json.c beside the v2
 * reader.  Without them every shipped map -- including both tracked space maps,
 * whose 16 binds are 100 % "type": 0 -- would load as JCE_SRC_NONE and every
 * action in them would go dead, with the determinism gates as the only
 * witness. */
static JceBindType bind_type_from_v1(int v1)
{
    switch (v1) {
    case 0:  return JCE_SRC_KEY;
    case 1:  return JCE_SRC_MOUSE_BUTTON;
    case 2:  return JCE_SRC_PAD_BUTTON;
    case 3:  return JCE_SRC_PAD_AXIS;
    case 4:  return JCE_SRC_COMPOSITE;
    /* Schema 1 defined no other value.  NONE, not KEY: an out-of-range type
     * used to fall off the end of evaluate_binding()'s switch and resolve to
     * 0, so mapping it to KEY would make malformed data MORE active than it
     * was -- it would acquire a live scancode.  NONE keeps it inert. */
    default: return JCE_SRC_NONE;
    }
}

static int bind_type_to_v1(int type)
{
    switch (type) {
    case JCE_SRC_KEY:          return 0;
    case JCE_SRC_MOUSE_BUTTON: return 1;
    case JCE_SRC_PAD_BUTTON:   return 2;
    case JCE_SRC_PAD_AXIS:     return 3;
    case JCE_SRC_COMPOSITE:    return 4;
    default:                   return -1;   /* no schema-1 spelling */
    }
}

/* The schema-1 spelling of ONE composite sub-source.
 *
 * Schema 1 stores a bare int per slot, and the reader has nothing but that int
 * to type it with -- it re-types every non-zero code as JCE_SRC_KEY.  So a key
 * is the ONLY sub-source a schema-1 file can carry faithfully.  Anything else
 * goes out as 0 ("absent"), loudly: emitting its raw code would re-create the
 * exact defect JceInputSource exists to kill, because a D-pad sub-source
 * authored as code 11..14 would load back as the letters H, I, J or K.  Inert
 * beats mistyped -- the same choice bind_type_from_v1()'s default case makes.
 *
 * A JCE_SRC_NONE slot writes 0 even when it still carries a stale code, so an
 * absent slot can never resurrect as a live key on load.
 *
 * A JCE_SRC_KEY whose code is 0 also writes 0 and returns as absent; scancode
 * 0 is SDL_SCANCODE_UNKNOWN (JceKey letters start at 4), so no authorable key
 * is lost by that. */
static int bind_sub_code_to_v1(const JceInputSource *s, const char *action,
                               int bind_index, const char *slot)
{
    if (s->type == JCE_SRC_NONE)
        return 0;                    /* absent: the code is not consulted */
    if (s->type == JCE_SRC_KEY)
        return s->code;

    LOG_WARN(LOG_TAG,
             "action '%s' bind %d %s: sub-source type %d has no schema-1 "
             "spelling and was written as ABSENT, not as key code %d",
             action, bind_index, slot, (int)s->type, (int)s->code);
    return 0;
}

/* ── JSON action-map loading (editor-authored input_actions.json) ──── */

static JceInputActions *actions_from_json(const JceJson *root)
{
    JceJson *arr = jce_json_get(root, "actions");
    if (!jce_json_is_array(arr))
        return NULL;

    JceInputActions *a = jce_actions_create();
    if (!a)
        return NULL;

    int registered = 0;
    const int n = jce_json_array_size(arr);
    for (int i = 0; i < n; ++i) {
        JceJson *act = jce_json_array_at(arr, i);
        if (!jce_json_is_object(act)) continue;

        const char *name = jce_json_get_string(act, "name", NULL);
        if (!name || !name[0]) continue;

        int id = jce_action_register(a, name);
        if (id < 0) continue;   /* duplicate / table full */
        registered++;

        JceJson *binds = jce_json_get(act, "binds");
        if (!jce_json_is_array(binds)) continue;

        const int bn = jce_json_array_size(binds);
        for (int b = 0; b < bn; ++b) {
            JceJson *bj = jce_json_array_at(binds, b);
            if (!jce_json_is_object(bj)) continue;

            /* The on-disk `type` is a SCHEMA-1 int and is translated here; the
             * default of 0 is schema-1 KEY, which is what it always meant. */
            JceBinding bind;
            jce_binding_init(&bind,
                             bind_type_from_v1(jce_json_get_int(bj, "type", 0)),
                             jce_json_get_int(bj, "code", 0));
            bind.scale = (float)jce_json_get_number(bj, "scale", 1.0);
            /* Schema 1 had ONE deadzone number and saturated at 1.0 exactly, so
             * a v1 bind reproduces (|v| - dz) / (1 - dz) byte for byte.  This
             * OVERWRITES jce_binding_init()'s -1 for a pad axis on purpose: a
             * v1 file said 0.15 (or its own number) and must keep meaning it,
             * not silently start following the device profile. */
            bind.deadzone_inner = (float)jce_json_get_number(bj, "deadzone", 0.15);
            bind.deadzone_outer = 1.0f;
            /* Schema 1 axes are signed and unpaired: a v1 file must not
             * silently acquire radial shaping or a half-axis side. */
            bind.side           = (uint8_t)JCE_AXIS_SIDE_FULL;
            bind.pair_axis      = JCE_BIND_PAIR_NONE;
            {
                /* Composite sub-keys (absent / 0 for scalar binds — backward
                 * compatible: `code` carries the JceCompositeKind for these).
                 * A bare int of 0 always meant "absent"; JCE_SRC_NONE is now
                 * the only absence test. */
                const int sub[4] = {
                    jce_json_get_int(bj, "comp_pos",  0),
                    jce_json_get_int(bj, "comp_neg",  0),
                    jce_json_get_int(bj, "comp_up",   0),
                    jce_json_get_int(bj, "comp_down", 0)
                };
                for (int c = 0; c < 4; ++c) {
                    bind.comp[c].type = (int16_t)(sub[c] ? JCE_SRC_KEY : JCE_SRC_NONE);
                    bind.comp[c].side = (int16_t)JCE_AXIS_SIDE_FULL;
                    bind.comp[c].code = sub[c];
                }
            }
            /* Control-scheme device tag (absent / 0 == JCE_DEVICE_NONE:
             * device-agnostic, identical to a pre-scheme action map). */
            bind.device_group = jce_json_get_int(bj, "device_group", 0);
            jce_action_bind(a, id, &bind);
        }
    }

    /* Optional control schemes:  "schemes": [ { "name", "device_mask" } ].
     * Absent => legacy "all bindings active" map (scheme_count stays 0). */
    JceJson *schemes = jce_json_get(root, "schemes");
    if (jce_json_is_array(schemes)) {
        const int sn = jce_json_array_size(schemes);
        for (int s = 0; s < sn; ++s) {
            JceJson *sj = jce_json_array_at(schemes, s);
            if (!jce_json_is_object(sj)) continue;
            const char *sname = jce_json_get_string(sj, "name", NULL);
            if (!sname || !sname[0]) continue;
            unsigned mask = (unsigned)jce_json_get_int(sj, "device_mask", 0);
            jce_action_scheme_register(a, sname, mask);
        }
        /* Honor an explicit active scheme selection if present. */
        int act = jce_json_get_int(root, "active_scheme", -1);
        if (act >= 0)
            jce_action_scheme_set_active(a, act);
        /* Last-used-device auto-switch.  Applied AFTER set_active (which pins
         * auto off); absent key keeps that pre-scheme_auto behavior so older
         * files load unchanged. */
        jce_action_scheme_set_auto(a,
            jce_json_get_bool(root, "scheme_auto", jce_action_scheme_auto(a)));
    }

    /* Opt-out from jce_actions_merge_gamepad_defaults().  Absent == true, so
     * every map written before this key existed still gets the pad it was
     * always meant to have. */
    a->allow_gamepad_defaults =
        jce_json_get_bool(root, "gamepad_defaults", true);

    if (registered == 0) {
        jce_actions_destroy(a);
        return NULL;
    }
    return a;
}

JceInputActions *jce_actions_load_memory(const void *data, size_t size)
{
    if (!data || size == 0)
        return NULL;

    JceJson *root = jce_json_parse((const char *)data, size);
    if (!root)
        return NULL;
    JceInputActions *a = actions_from_json(root);
    jce_json_free(root);
    return a;
}

JceInputActions *jce_actions_load_file(const char *path)
{
    if (!path || !path[0])
        return NULL;

    JceJson *root = jce_json_parse_file(path);
    if (!root)
        return NULL;
    JceInputActions *a = actions_from_json(root);
    jce_json_free(root);
    return a;
}

/* ── JSON action-map saving (same schema jce_actions_load_file reads) ── */

bool jce_actions_save_file(const JceInputActions *a, const char *path)
{
    if (!a || !path || !path[0]) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    JceJson *arr = jce_json_array();
    if (!arr) {
        jce_json_free(root);
        return false;
    }
    jce_json_set_child(root, "actions", arr);

    /* Round-trip the merge opt-out.  Written ONLY when false: the default
     * stays absent so this does not churn every existing file, and an absent
     * key already means true on load. */
    if (!a->allow_gamepad_defaults)
        jce_json_set_bool(root, "gamepad_defaults", false);

    for (int i = 0; i < a->count; ++i) {
        const ActionEntry *e = &a->actions[i];

        JceJson *act = jce_json_object();
        if (!act) continue;
        jce_json_set_string(act, "name", e->name);

        JceJson *binds = jce_json_array();
        if (binds) {
            for (int b = 0; b < e->bind_count; ++b) {
                const JceBinding *bd = &e->binds[b];
                const int v1 = bind_type_to_v1((int)bd->type);
                if (v1 < 0) {
                    /* Schema 1 has no spelling for this source.  Refusing
                     * loudly beats emitting a number the schema-1 reader would
                     * resolve to a DIFFERENT source.  Plan B Batch 4's v2
                     * writer removes the lossiness; nothing in the tree
                     * authors these sources yet, so this branch is currently
                     * unreachable in practice and is here so it can never be
                     * silent. */
                    LOG_WARN(LOG_TAG,
                             "action '%s' bind %d: source %d has no schema-1 "
                             "spelling and was NOT written",
                             e->name, b, (int)bd->type);
                    continue;
                }
                JceJson *bj = jce_json_object();
                if (!bj) continue;
                jce_json_set_int(bj, "type", v1);
                jce_json_set_int(bj, "code", bd->code);
                jce_json_set_number(bj, "scale", (double)bd->scale);
                jce_json_set_number(bj, "deadzone", (double)bd->deadzone_inner);
                /* Emit composite sub-keys only for composite binds so scalar
                 * output stays byte-for-byte compatible with the old schema.
                 * Schema 1 has only key sub-sources, so only the code goes
                 * out; a JCE_SRC_NONE slot writes 0, which is what "absent"
                 * has always been on disk.
                 *
                 * The TYPE is not expressible on disk, so it must gate what
                 * goes out -- bind_sub_code_to_v1() is the same refusal the
                 * top-level source gets above.  Writing the raw code would
                 * re-create the exact defect this record was introduced to
                 * kill: a typed pad sub-source would come back as a key,
                 * because the reader has nothing but the code to go on. */
                if (bd->type == JCE_SRC_COMPOSITE) {
                    static const int k_slot[4] = {
                        JCE_COMP_POS, JCE_COMP_NEG, JCE_COMP_UP, JCE_COMP_DOWN
                    };
                    static const char *const k_key[4] = {
                        "comp_pos", "comp_neg", "comp_up", "comp_down"
                    };
                    for (int c = 0; c < 4; ++c)
                        jce_json_set_int(bj, k_key[c],
                                         bind_sub_code_to_v1(&bd->comp[k_slot[c]],
                                                             e->name, b, k_key[c]));
                }
                /* Emit the control-scheme tag only when it is non-default so a
                 * scheme-free map stays byte-for-byte compatible. */
                if (bd->device_group != JCE_DEVICE_NONE)
                    jce_json_set_int(bj, "device_group", bd->device_group);
                jce_json_array_push(binds, bj);
            }
            jce_json_set_child(act, "binds", binds);
        }
        jce_json_array_push(arr, act);
    }

    /* Emit control schemes only when defined so a scheme-free map stays
     * byte-for-byte compatible with the old schema. */
    if (a->scheme_count > 0) {
        JceJson *sarr = jce_json_array();
        if (sarr) {
            for (int s = 0; s < a->scheme_count; ++s) {
                const SchemeEntry *se = &a->schemes[s];
                JceJson *sj = jce_json_object();
                if (!sj) continue;
                jce_json_set_string(sj, "name", se->name);
                jce_json_set_int(sj, "device_mask", (int)se->device_mask);
                jce_json_array_push(sarr, sj);
            }
            jce_json_set_child(root, "schemes", sarr);
        }
        if (a->active_scheme >= 0)
            jce_json_set_int(root, "active_scheme", a->active_scheme);
        /* Auto-switch toggle: previously round-tripped through the live
         * table only and silently reset on reload. */
        jce_json_set_bool(root, "scheme_auto", a->auto_switch);
    }

    /* take_ownership=true: root is freed regardless of write success. */
    return jce_json_write_file(path, root, /*pretty=*/true,
                               /*take_ownership=*/true);
}
