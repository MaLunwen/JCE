#include "core/jce_editor_game_input_bridge.h"

#include <cmath>
#include <cstring>

namespace {

JceEditorGameInputBridge g_bridge;

void clear_sample(JceEditorGameInputBridge *bridge)
{
    bridge->dx = 0.0f;
    bridge->dy = 0.0f;
    bridge->wheel = 0.0f;
    bridge->buttons = 0u;
}

bool point_inside(const JceEditorGameInputViewport &viewport,
                  float x, float y)
{
    return std::isfinite(x) && std::isfinite(y) &&
           x >= viewport.x && y >= viewport.y &&
           x < viewport.x + viewport.width &&
           y < viewport.y + viewport.height;
}

uint32_t button_bit(uint8_t button)
{
    if (button < JCE_MOUSE_BUTTON_LEFT || button > JCE_MOUSE_BUTTON_X2)
        return 0u;
    return UINT32_C(1) << (button - 1u);
}

bool runtime_accepts_pointer(const JceEditorGameInputBridge *bridge)
{
    return bridge && bridge->viewport.visible &&
           bridge->viewport.owner == JCE_EDITOR_GAME_INPUT_RUNTIME &&
           (bridge->viewport.hovered || bridge->viewport.captured);
}

} // namespace

void jce_editor_game_input_bridge_reset(JceEditorGameInputBridge *bridge)
{
    if (bridge)
        std::memset(bridge, 0, sizeof(*bridge));
}

void jce_editor_game_input_bridge_publish(
    JceEditorGameInputBridge *bridge,
    const JceEditorGameInputViewport *viewport)
{
    if (!bridge)
        return;
    if (!viewport || !viewport->visible ||
        !std::isfinite(viewport->x) || !std::isfinite(viewport->y) ||
        !std::isfinite(viewport->width) || !std::isfinite(viewport->height) ||
        viewport->width <= 0.0f || viewport->height <= 0.0f) {
        jce_editor_game_input_bridge_reset(bridge);
        return;
    }

    const bool runtime_lost =
        viewport->owner != JCE_EDITOR_GAME_INPUT_RUNTIME ||
        (!viewport->hovered && !viewport->captured);
    bridge->viewport = *viewport;
    if (runtime_lost)
        clear_sample(bridge);
}

void jce_editor_game_input_bridge_handle_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event)
{
    if (!bridge || !event)
        return;
    if (event->type == JCE_EVENT_WINDOW_FOCUS_LOST) {
        clear_sample(bridge);
        return;
    }

    switch (event->type) {
    case JCE_EVENT_MOUSE_MOTION:
        if (runtime_accepts_pointer(bridge) &&
            (bridge->viewport.captured ||
             point_inside(bridge->viewport, event->motion.x, event->motion.y)) &&
            std::isfinite(event->motion.xrel) &&
            std::isfinite(event->motion.yrel)) {
            bridge->dx += event->motion.xrel;
            bridge->dy += event->motion.yrel;
        }
        break;
    case JCE_EVENT_MOUSE_WHEEL:
        if (runtime_accepts_pointer(bridge) &&
            std::isfinite(event->wheel.y)) {
            bridge->wheel += event->wheel.y;
        }
        break;
    case JCE_EVENT_MOUSE_BUTTON_DOWN: {
        const uint32_t bit = button_bit(event->button.button);
        if (bit && runtime_accepts_pointer(bridge) &&
            (bridge->viewport.captured ||
             point_inside(bridge->viewport, event->button.x, event->button.y))) {
            bridge->buttons |= bit;
        }
        break;
    }
    case JCE_EVENT_MOUSE_BUTTON_UP: {
        const uint32_t bit = button_bit(event->button.button);
        if (bit)
            bridge->buttons &= ~bit;
        break;
    }
    default:
        break;
    }
}

JceEditorGameInputSample jce_editor_game_input_bridge_consume(
    JceEditorGameInputBridge *bridge)
{
    JceEditorGameInputSample sample{};
    if (!bridge)
        return sample;
    sample.owner = bridge->viewport.owner;
    if (bridge->viewport.owner == JCE_EDITOR_GAME_INPUT_RUNTIME) {
        sample.dx = bridge->dx;
        sample.dy = bridge->dy;
        sample.wheel = bridge->wheel;
        sample.buttons = bridge->buttons;
    }
    bridge->dx = 0.0f;
    bridge->dy = 0.0f;
    bridge->wheel = 0.0f;
    return sample;
}

bool jce_editor_game_input_bridge_commit(
    JceEditorGameInputBridge *bridge,
    const JceRuntimeTouch *touches, int touch_count,
    const JceInputActions *actions, float dt,
    const JceEditorGameInputCommitOps *ops, void *ud)
{
    if (!bridge || !ops || !ops->set_pointer || !ops->set_touches ||
        !ops->set_actions || !ops->step) {
        return false;
    }
    if (!touches || touch_count < 0)
        touch_count = 0;
    if (touch_count > JCE_RUNTIME_MAX_TOUCHES)
        touch_count = JCE_RUNTIME_MAX_TOUCHES;

    const JceEditorGameInputSample sample =
        jce_editor_game_input_bridge_consume(bridge);
    ops->set_pointer(ud, sample.dx, sample.dy, sample.wheel, sample.buttons);
    ops->set_touches(ud, touches, touch_count);
    ops->set_actions(ud, actions);
    ops->step(ud, dt);
    return true;
}

JceEditorGameInputBridge *jce_editor_game_input_bridge_shared(void)
{
    return &g_bridge;
}

