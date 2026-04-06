/*
 * jce_input.c  Cross-platform input management.
 */

#include <jce/platform/jce_input.h>
#include "core/jce_memory.h"
#include <string.h>

struct JceInput {
    /* Keyboard */
    bool keys_cur[SDL_SCANCODE_COUNT];
    bool keys_prev[SDL_SCANCODE_COUNT];

    /* Mouse */
    float    mouse_x, mouse_y;
    float    mouse_dx, mouse_dy;
    float    wheel;
    uint32_t mouse_cur;
    uint32_t mouse_prev;

    /* Touch */
    int touch_count;
    struct {
        SDL_FingerID id;
        float x, y, pressure;
    } touches[JCE_MAX_TOUCHES];

    /* Gamepads */
    int gamepad_count;
    struct {
        SDL_Gamepad *sdl_pad;
        SDL_JoystickID jid;
        uint32_t buttons_cur;
        uint32_t buttons_prev;
        float axes[SDL_GAMEPAD_AXIS_COUNT];
    } gamepads[JCE_MAX_GAMEPADS];
};

JceInput *jce_input_create(void)
{
    JceInput *input = (JceInput *)JCE_CALLOC(1, sizeof(*input));
    return input;
}

void jce_input_destroy(JceInput *input)
{
    if (!input) return;
    for (int i = 0; i < input->gamepad_count; i++) {
        if (input->gamepads[i].sdl_pad)
            SDL_CloseGamepad(input->gamepads[i].sdl_pad);
    }
    JCE_FREE(input);
}

void jce_input_update(JceInput *input)
{
    if (!input) return;

    memcpy(input->keys_prev, input->keys_cur, sizeof(input->keys_cur));
    input->mouse_prev = input->mouse_cur;
    input->mouse_dx   = 0.0f;
    input->mouse_dy   = 0.0f;
    input->wheel       = 0.0f;

    for (int i = 0; i < input->gamepad_count; i++)
        input->gamepads[i].buttons_prev = input->gamepads[i].buttons_cur;
}

/* -- internal: gamepad slot management ------------------------------ */

static int find_gamepad_by_jid(const JceInput *input, SDL_JoystickID jid)
{
    for (int i = 0; i < input->gamepad_count; i++)
        if (input->gamepads[i].jid == jid)
            return i;
    return -1;
}

static void add_gamepad(JceInput *input, SDL_JoystickID jid)
{
    if (input->gamepad_count >= JCE_MAX_GAMEPADS) return;
    if (find_gamepad_by_jid(input, jid) >= 0) return;

    SDL_Gamepad *pad = SDL_OpenGamepad(jid);
    if (!pad) return;

    int slot = input->gamepad_count++;
    input->gamepads[slot].sdl_pad     = pad;
    input->gamepads[slot].jid         = jid;
    input->gamepads[slot].buttons_cur = 0;
    input->gamepads[slot].buttons_prev = 0;
    memset(input->gamepads[slot].axes, 0, sizeof(input->gamepads[slot].axes));
}

static void remove_gamepad(JceInput *input, SDL_JoystickID jid)
{
    int idx = find_gamepad_by_jid(input, jid);
    if (idx < 0) return;

    if (input->gamepads[idx].sdl_pad)
        SDL_CloseGamepad(input->gamepads[idx].sdl_pad);

    /* Shift remaining entries down. */
    int last = input->gamepad_count - 1;
    if (idx < last)
        input->gamepads[idx] = input->gamepads[last];
    memset(&input->gamepads[last], 0, sizeof(input->gamepads[last]));
    input->gamepad_count--;
}

/* -- event dispatch ------------------------------------------------- */

void jce_input_handle_event(JceInput *input, const SDL_Event *event)
{
    if (!input || !event) return;

    switch (event->type) {

    /* Keyboard */
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode < SDL_SCANCODE_COUNT)
            input->keys_cur[event->key.scancode] = true;
        break;
    case SDL_EVENT_KEY_UP:
        if (event->key.scancode < SDL_SCANCODE_COUNT)
            input->keys_cur[event->key.scancode] = false;
        break;

    /* Mouse */
    case SDL_EVENT_MOUSE_MOTION:
        input->mouse_x  = event->motion.x;
        input->mouse_y  = event->motion.y;
        input->mouse_dx += event->motion.xrel;
        input->mouse_dy += event->motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        input->mouse_cur |= SDL_BUTTON_MASK(event->button.button);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        input->mouse_cur &= ~SDL_BUTTON_MASK(event->button.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        input->wheel += event->wheel.y;
        break;

    /* Touch */
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
        for (int i = 0; i < input->touch_count; i++) {
            if (input->touches[i].id == event->tfinger.fingerID) {
                input->touches[i].x = event->tfinger.x;
                input->touches[i].y = event->tfinger.y;
                input->touches[i].pressure = event->tfinger.pressure;
                return;
            }
        }
        if (event->type == SDL_EVENT_FINGER_DOWN &&
            input->touch_count < JCE_MAX_TOUCHES) {
            int s = input->touch_count++;
            input->touches[s].id       = event->tfinger.fingerID;
            input->touches[s].x        = event->tfinger.x;
            input->touches[s].y        = event->tfinger.y;
            input->touches[s].pressure = event->tfinger.pressure;
        }
        break;
    case SDL_EVENT_FINGER_UP:
        for (int i = 0; i < input->touch_count; i++) {
            if (input->touches[i].id == event->tfinger.fingerID) {
                int last = input->touch_count - 1;
                if (i < last)
                    input->touches[i] = input->touches[last];
                input->touch_count--;
                break;
            }
        }
        break;

    /* Gamepad */
    case SDL_EVENT_GAMEPAD_ADDED:
        add_gamepad(input, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(input, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        int idx = find_gamepad_by_jid(input,
            SDL_GetJoystickID(SDL_GetGamepadJoystick(
                SDL_GetGamepadFromID(event->gbutton.which))));
        if (idx >= 0)
            input->gamepads[idx].buttons_cur |= (1u << event->gbutton.button);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int idx = find_gamepad_by_jid(input,
            SDL_GetJoystickID(SDL_GetGamepadJoystick(
                SDL_GetGamepadFromID(event->gbutton.which))));
        if (idx >= 0)
            input->gamepads[idx].buttons_cur &= ~(1u << event->gbutton.button);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int idx = find_gamepad_by_jid(input,
            SDL_GetJoystickID(SDL_GetGamepadJoystick(
                SDL_GetGamepadFromID(event->gaxis.which))));
        if (idx >= 0 && event->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT)
            input->gamepads[idx].axes[event->gaxis.axis] =
                (float)event->gaxis.value / 32767.0f;
        break;
    }

    default:
        break;
    }
}

/* -- Keyboard queries ----------------------------------------------- */

bool jce_input_key_down(const JceInput *input, SDL_Scancode key)
{
    if (!input || key >= SDL_SCANCODE_COUNT) return false;
    return input->keys_cur[key];
}

bool jce_input_key_pressed(const JceInput *input, SDL_Scancode key)
{
    if (!input || key >= SDL_SCANCODE_COUNT) return false;
    return input->keys_cur[key] && !input->keys_prev[key];
}

bool jce_input_key_released(const JceInput *input, SDL_Scancode key)
{
    if (!input || key >= SDL_SCANCODE_COUNT) return false;
    return !input->keys_cur[key] && input->keys_prev[key];
}

/* -- Mouse queries -------------------------------------------------- */

void jce_input_mouse_pos(const JceInput *input, float *x, float *y)
{
    if (x) *x = input ? input->mouse_x : 0.0f;
    if (y) *y = input ? input->mouse_y : 0.0f;
}

void jce_input_mouse_delta(const JceInput *input, float *dx, float *dy)
{
    if (dx) *dx = input ? input->mouse_dx : 0.0f;
    if (dy) *dy = input ? input->mouse_dy : 0.0f;
}

bool jce_input_mouse_button(const JceInput *input, int button)
{
    if (!input) return false;
    return (input->mouse_cur & SDL_BUTTON_MASK(button)) != 0;
}

bool jce_input_mouse_button_pressed(const JceInput *input, int button)
{
    if (!input) return false;
    uint32_t mask = SDL_BUTTON_MASK(button);
    return (input->mouse_cur & mask) && !(input->mouse_prev & mask);
}

bool jce_input_mouse_button_released(const JceInput *input, int button)
{
    if (!input) return false;
    uint32_t mask = SDL_BUTTON_MASK(button);
    return !(input->mouse_cur & mask) && (input->mouse_prev & mask);
}

float jce_input_mouse_wheel(const JceInput *input)
{
    return input ? input->wheel : 0.0f;
}

/* -- Touch queries -------------------------------------------------- */

int jce_input_touch_count(const JceInput *input)
{
    return input ? input->touch_count : 0;
}

bool jce_input_touch_get(const JceInput *input, int index,
                         SDL_FingerID *id, float *x, float *y,
                         float *pressure)
{
    if (!input || index < 0 || index >= input->touch_count) return false;
    if (id)       *id       = input->touches[index].id;
    if (x)        *x        = input->touches[index].x;
    if (y)        *y        = input->touches[index].y;
    if (pressure) *pressure = input->touches[index].pressure;
    return true;
}

/* -- Gamepad queries ------------------------------------------------ */

int jce_input_gamepad_count(const JceInput *input)
{
    return input ? input->gamepad_count : 0;
}

bool jce_input_gamepad_button(const JceInput *input, int pad,
                              SDL_GamepadButton btn)
{
    if (!input || pad < 0 || pad >= input->gamepad_count) return false;
    return (input->gamepads[pad].buttons_cur & (1u << btn)) != 0;
}

bool jce_input_gamepad_button_pressed(const JceInput *input, int pad,
                                      SDL_GamepadButton btn)
{
    if (!input || pad < 0 || pad >= input->gamepad_count) return false;
    uint32_t mask = 1u << btn;
    return (input->gamepads[pad].buttons_cur & mask)
        && !(input->gamepads[pad].buttons_prev & mask);
}

float jce_input_gamepad_axis(const JceInput *input, int pad,
                             SDL_GamepadAxis axis)
{
    if (!input || pad < 0 || pad >= input->gamepad_count) return 0.0f;
    if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0.0f;
    return input->gamepads[pad].axes[axis];
}
