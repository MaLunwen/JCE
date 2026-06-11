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
    float      value;           /* this frame */
    float      prev_value;      /* last frame */
} ActionEntry;

struct JceInputActions {
    ActionEntry actions[JCE_ACTION_MAX];
    int         count;
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
/* Per-frame update                                                    */
/* ================================================================== */

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
    }
    return 0;
}

void jce_actions_update(JceInputActions *a, const JceInput *input)
{
    if (!a || !input) return;

    for (int i = 0; i < a->count; i++) {
        ActionEntry *e = &a->actions[i];
        e->prev_value = e->value;

        /* Accumulate all bindings (allows e.g. W + left-stick both). */
        float total = 0;
        for (int b = 0; b < e->bind_count; b++)
            total += evaluate_binding(&e->binds[b], input);

        /* Clamp to -1..1. */
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
                .deadzone = (float)jce_json_get_number(bj, "deadzone", 0.15)
            };
            jce_action_bind(a, id, &bind);
        }
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
                jce_json_array_push(binds, bj);
            }
            jce_json_set_child(act, "binds", binds);
        }
        jce_json_array_push(arr, act);
    }

    /* take_ownership=true: root is freed regardless of write success. */
    return jce_json_write_file(path, root, /*pretty=*/true,
                               /*take_ownership=*/true);
}
