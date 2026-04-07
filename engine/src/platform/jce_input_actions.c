/*
 * jce_input_actions.c  Action-based input implementation.
 */

#include "jce_input_actions.h"
#include <jce/platform/jce_input.h>
#include "core/jce_memory.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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
/* Default FPS bindings                                                */
/* ================================================================== */

int jce_actions_bind_fps_defaults(JceInputActions *a)
{
    if (!a) return -1;

    int first = a->count;

    int fwd   = jce_action_register(a, "move_forward");
    int back  = jce_action_register(a, "move_back");
    int left  = jce_action_register(a, "move_left");
    int right = jce_action_register(a, "move_right");
    int up    = jce_action_register(a, "move_up");
    int down  = jce_action_register(a, "move_down");
    int sprint= jce_action_register(a, "sprint");
    int lx    = jce_action_register(a, "look_x");
    int ly    = jce_action_register(a, "look_y");

    /* Keyboard WASD. */
    jce_action_bind(a, fwd,   &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_W, 1.0f, 0 });
    jce_action_bind(a, back,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_S, 1.0f, 0 });
    jce_action_bind(a, left,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_A, 1.0f, 0 });
    jce_action_bind(a, right, &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_D, 1.0f, 0 });
    jce_action_bind(a, up,    &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_SPACE, 1.0f, 0 });
    jce_action_bind(a, down,  &(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_LSHIFT, 1.0f, 0 });
    jce_action_bind(a, sprint,&(JceBinding){ JCE_BIND_KEY, SDL_SCANCODE_LCTRL, 1.0f, 0 });

    /* Gamepad: left stick = move, right stick = look. */
    jce_action_bind(a, fwd,   &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFTY,  -1.0f, 0.15f });
    jce_action_bind(a, right, &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_LEFTX,   1.0f, 0.15f });
    jce_action_bind(a, lx,    &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_RIGHTX,  1.0f, 0.15f });
    jce_action_bind(a, ly,    &(JceBinding){ JCE_BIND_GAMEPAD_AXIS, SDL_GAMEPAD_AXIS_RIGHTY, -1.0f, 0.15f });

    (void)back; (void)left; (void)down; (void)lx; (void)ly;

    return first;
}
