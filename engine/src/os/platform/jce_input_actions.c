/*
 * jce_input_actions.c  Action-based input implementation.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/core/jce_json.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

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
    return id;
}

bool jce_action_bind(JceInputActions *a, int action_id,
                      const JceBinding *binding)
{
    if (!a || !binding) return false;
    if (action_id < 0 || action_id >= a->count) return false;

    ActionEntry *e = &a->actions[action_id];
    if (e->bind_count >= JCE_ACTION_MAX_BINDS) return false;

    e->binds[e->bind_count++] = *binding;
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

/* Scalar contribution of a single non-composite binding. */
static float evaluate_binding(const JceBinding *b, const JceInput *input)
{
    switch (b->type) {
    case JCE_BIND_KEY:
        return jce_input_key_down(input, (SDL_Scancode)b->code) ? b->scale : 0;

    case JCE_BIND_MOUSE_BTN:
        return jce_input_mouse_button(input, b->code) ? b->scale : 0;

    case JCE_BIND_GAMEPAD_BTN:
        return jce_input_gamepad_button(input, 0, (SDL_GamepadButton)b->code)
             ? b->scale : 0;

    case JCE_BIND_GAMEPAD_AXIS: {
        float v = jce_input_gamepad_axis(input, 0, (SDL_GamepadAxis)b->code);
        float dz = b->deadzone > 0 ? b->deadzone : 0.15f;
        if (fabsf(v) < dz) return 0;
        /* Remap from deadzone..1 to 0..1 */
        float sign = v > 0 ? 1.0f : -1.0f;
        float remapped = (fabsf(v) - dz) / (1.0f - dz);
        return sign * remapped * b->scale;
    }

    case JCE_BIND_COMPOSITE:
        /* Composites resolve into (x,y) via evaluate_composite(); they make
         * no scalar contribution to the flat sum. */
        return 0;
    }
    return 0;
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

    /* X axis: positive (right / pos) minus negative (left / neg). */
    if (b->comp_pos && jce_input_key_down(input, (SDL_Scancode)b->comp_pos))
        x += 1.0f;
    if (b->comp_neg && jce_input_key_down(input, (SDL_Scancode)b->comp_neg))
        x -= 1.0f;

    /* Y axis (2D only): up minus down.  For a 1D axis comp_up/comp_down are 0. */
    if (b->comp_up && jce_input_key_down(input, (SDL_Scancode)b->comp_up))
        y += 1.0f;
    if (b->comp_down && jce_input_key_down(input, (SDL_Scancode)b->comp_down))
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
        if (jce_input_key_down(input, (SDL_Scancode)sc)) {
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

    /* Gamepad: any button on pad 0, or any axis past a nominal deadzone. */
    for (int bn = 0; bn < JCE_GAMEPAD_BUTTON_COUNT; ++bn) {
        if (jce_input_gamepad_button(input, 0, bn)) {
            mask |= JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD);
            break;
        }
    }
    if (!(mask & JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD))) {
        for (int ax = 0; ax < JCE_GAMEPAD_AXIS_COUNT; ++ax) {
            if (fabsf(jce_input_gamepad_axis(input, 0, ax)) >= 0.5f) {
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
            } else {
                total += evaluate_binding(bd, input);
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

            if (has_vector_2d)
                total += sqrtf(vx * vx + vy * vy);/* 2D: magnitude     */
            else
                total += vx;                      /* 1D axis: keep sign */
        } else {
            e->value_x = total;
            e->value_y = 0.0f;
        }

        /* Clamp scalar to -1..1. */
        if (total > 1.0f)  total = 1.0f;
        if (total < -1.0f) total = -1.0f;

        e->value = total;
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

    /* Keyboard: WASD + Space jump + LShift sprint. */
    jce_action_bind(a, fwd,   &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_W, 1.0f, 0 });
    jce_action_bind(a, back,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_S, 1.0f, 0 });
    jce_action_bind(a, left,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_A, 1.0f, 0 });
    jce_action_bind(a, right, &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_D, 1.0f, 0 });
    jce_action_bind(a, jump,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_SPACE, 1.0f, 0 });
    jce_action_bind(a, sprint,&(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_LSHIFT, 1.0f, 0 });

    /* Gamepad: left stick = move, right stick = look,
     * South (A) = jump, L3 (left-stick click) = sprint. */
    jce_action_bind(a, fwd,   &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFTY,  -1.0f, 0.15f });
    jce_action_bind(a, right, &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFTX,   1.0f, 0.15f });
    jce_action_bind(a, jump,  &(JceBinding){ JCE_BIND_GAMEPAD_BTN,  SDL_GAMEPAD_BUTTON_SOUTH,      1.0f, 0 });
    jce_action_bind(a, sprint,&(JceBinding){ JCE_BIND_GAMEPAD_BTN,  SDL_GAMEPAD_BUTTON_LEFT_STICK, 1.0f, 0 });
    jce_action_bind(a, lx,    &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_RIGHTX,  1.0f, 0.15f });
    jce_action_bind(a, ly,    &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_RIGHTY, -1.0f, 0.15f });

    (void)up; (void)down;   /* registered, deliberately unbound */

    return first;
}

/* ── JSON action-map loading (editor-authored input_actions.json) ──── */

JceInputActions *jce_actions_load_file(const char *path)
{
    if (!path || !path[0]) return NULL;

    JceJson *root = jce_json_parse_file(path);
    if (!root) return NULL;

    JceJson *arr = jce_json_get(root, "actions");
    if (!jce_json_is_array(arr)) {
        jce_json_free(root);
        return NULL;
    }

    JceInputActions *a = jce_actions_create();
    if (!a) {
        jce_json_free(root);
        return NULL;
    }

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

            JceBinding bind = {
                .type     = (JceBindType)jce_json_get_int(bj, "type", JCE_BIND_KEY),
                .code     = jce_json_get_int(bj, "code", 0),
                .scale    = (float)jce_json_get_number(bj, "scale", 1.0),
                .deadzone = (float)jce_json_get_number(bj, "deadzone", 0.15),
                /* Composite sub-keys (absent / 0 for scalar binds — backward
                 * compatible: `code` carries the JceCompositeKind for these). */
                .comp_pos  = jce_json_get_int(bj, "comp_pos",  0),
                .comp_neg  = jce_json_get_int(bj, "comp_neg",  0),
                .comp_up   = jce_json_get_int(bj, "comp_up",   0),
                .comp_down = jce_json_get_int(bj, "comp_down", 0),
                /* Control-scheme device tag (absent / 0 == JCE_DEVICE_NONE:
                 * device-agnostic, identical to a pre-scheme action map). */
                .device_group = jce_json_get_int(bj, "device_group", 0)
            };
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
    }

    jce_json_free(root);

    if (registered == 0) {
        jce_actions_destroy(a);
        return NULL;
    }
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

    for (int i = 0; i < a->count; ++i) {
        const ActionEntry *e = &a->actions[i];

        JceJson *act = jce_json_object();
        if (!act) continue;
        jce_json_set_string(act, "name", e->name);

        JceJson *binds = jce_json_array();
        if (binds) {
            for (int b = 0; b < e->bind_count; ++b) {
                const JceBinding *bd = &e->binds[b];
                JceJson *bj = jce_json_object();
                if (!bj) continue;
                jce_json_set_int(bj, "type", (int)bd->type);
                jce_json_set_int(bj, "code", bd->code);
                jce_json_set_number(bj, "scale", (double)bd->scale);
                jce_json_set_number(bj, "deadzone", (double)bd->deadzone);
                /* Emit composite sub-keys only for composite binds so scalar
                 * output stays byte-for-byte compatible with the old schema. */
                if (bd->type == JCE_BIND_COMPOSITE) {
                    jce_json_set_int(bj, "comp_pos",  bd->comp_pos);
                    jce_json_set_int(bj, "comp_neg",  bd->comp_neg);
                    jce_json_set_int(bj, "comp_up",   bd->comp_up);
                    jce_json_set_int(bj, "comp_down", bd->comp_down);
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
    }

    /* take_ownership=true: root is freed regardless of write success. */
    return jce_json_write_file(path, root, /*pretty=*/true,
                               /*take_ownership=*/true);
}
