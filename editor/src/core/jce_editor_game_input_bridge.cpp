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
    bridge->position_valid = false;
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
    return bridge && bridge->viewport.visible && bridge->viewport.focused &&
           bridge->viewport.owner == JCE_EDITOR_GAME_INPUT_RUNTIME &&
           (bridge->viewport.hovered || bridge->viewport.captured);
}

bool virtual_viewport_admitted(const JceEditorGameInputViewport &v)
{
    return v.visible && v.owner == JCE_EDITOR_GAME_INPUT_RUNTIME &&
        std::isfinite(v.x) && std::isfinite(v.y) &&
        std::isfinite(v.width) && std::isfinite(v.height) &&
        v.width > 0 && v.height > 0 &&
        std::isfinite(v.x + v.width) && std::isfinite(v.y + v.height) &&
        std::isfinite(v.render_width) && std::isfinite(v.render_height) &&
        v.render_width >= 0 && v.render_height >= 0 &&
        std::isfinite(v.render_width / v.width) &&
        std::isfinite(v.render_height / v.height);
}

bool same_geometry(const JceEditorGameInputViewport &a,
                   const JceEditorGameInputViewport &b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width &&
        a.height == b.height && a.render_width == b.render_width &&
        a.render_height == b.render_height;
}

void prime_hover_position(JceEditorGameInputBridge *bridge)
{
    const auto &viewport = bridge->viewport;
    if (bridge->position_valid || !runtime_accepts_pointer(bridge) ||
        !viewport.hovered || viewport.captured || !viewport.pointer_valid ||
        !point_inside(viewport, viewport.pointer_x, viewport.pointer_y)) {
        return;
    }
    /* The first entry motion can precede hover publication. Copy only the
     * actual cursor snapshot; native button/delta/wheel state stays untouched. */
    bridge->x = viewport.pointer_x;
    bridge->y = viewport.pointer_y;
    bridge->position_valid = true;
}

JceEditorGameInputSample sample_input(const JceEditorGameInputBridge *bridge)
{
    JceEditorGameInputSample sample{};
    if (!bridge) return sample;
    sample.owner = bridge->viewport.owner;
    const bool virtual_source = bridge->virtual_pointer.active && virtual_viewport_admitted(bridge->viewport);
    if (bridge->viewport.owner == JCE_EDITOR_GAME_INPUT_RUNTIME) {
        const float rw = bridge->viewport.render_width > 0.0f
                       ? bridge->viewport.render_width : bridge->viewport.width;
        const float rh = bridge->viewport.render_height > 0.0f
                       ? bridge->viewport.render_height : bridge->viewport.height;
        const float sx = bridge->viewport.width > 0 ? rw / bridge->viewport.width : 1.0f;
        const float sy = bridge->viewport.height > 0 ? rh / bridge->viewport.height : 1.0f;
        sample.synthetic = virtual_source;
        sample.replayed = virtual_source && bridge->virtual_pointer.replayed;
        sample.dx = (virtual_source ? bridge->virtual_pointer.dx : bridge->dx) * sx;
        sample.dy = (virtual_source ? bridge->virtual_pointer.dy : bridge->dy) * sy;
        sample.wheel = virtual_source ? bridge->virtual_pointer.wheel : bridge->wheel;
        sample.buttons = virtual_source ? bridge->virtual_pointer.buttons : bridge->buttons;
        if (virtual_source || (bridge->position_valid && runtime_accepts_pointer(bridge))) {
            const float x = virtual_source ? bridge->virtual_pointer.x : bridge->x;
            const float y = virtual_source ? bridge->virtual_pointer.y : bridge->y;
            sample.x = (x - bridge->viewport.x) * sx;
            sample.y = (y - bridge->viewport.y) * sy;
            sample.width = rw;
            sample.height = rh;
        }
    }
    return sample;
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
        viewport->owner != JCE_EDITOR_GAME_INPUT_RUNTIME || !viewport->focused ||
        (!viewport->hovered && !viewport->captured);
    if (bridge->virtual_pointer.active &&
        (!virtual_viewport_admitted(*viewport) ||
         !same_geometry(bridge->viewport, *viewport)))
        jce_editor_game_input_bridge_clear_virtual(bridge);
    bridge->viewport = *viewport;
    if (runtime_lost)
        clear_sample(bridge);
    prime_hover_position(bridge);
}

void jce_editor_game_input_bridge_clear_virtual(JceEditorGameInputBridge *bridge)
{
    if (bridge) bridge->virtual_pointer = {};
}

void jce_editor_game_input_bridge_sync_replay(
    JceEditorGameInputBridge *bridge, const JceInput *input)
{
    if (!bridge) return;
    if (!jce_input_snapshot_was_replayed(input)) {
        if (bridge->virtual_pointer.replayed)
            jce_editor_game_input_bridge_clear_virtual(bridge);
        return;
    }

    /* Real replay replaces live input at the Engine seam. Do not merge native
     * events accumulated before this same app-update into the replay sample. */
    clear_sample(bridge);
    JceInputFrame frame{};
    jce_input_capture(input, &frame);
    if (!virtual_viewport_admitted(bridge->viewport) ||
        !point_inside(bridge->viewport, frame.mouse_x, frame.mouse_y) ||
        !std::isfinite(frame.mouse_dx) || !std::isfinite(frame.mouse_dy) ||
        !std::isfinite(frame.mouse_wheel)) {
        jce_editor_game_input_bridge_clear_virtual(bridge);
        return;
    }
    const auto &v = bridge->viewport;
    const float sx = v.render_width > 0 ? v.render_width / v.width : 1;
    const float sy = v.render_height > 0 ? v.render_height / v.height : 1;
    if (!std::isfinite(frame.mouse_dx * sx) ||
        !std::isfinite(frame.mouse_dy * sy)) {
        jce_editor_game_input_bridge_clear_virtual(bridge);
        return;
    }
    auto &pointer = bridge->virtual_pointer;
    pointer.dx = frame.mouse_dx; pointer.dy = frame.mouse_dy;
    pointer.wheel = frame.mouse_wheel;
    pointer.x = frame.mouse_x; pointer.y = frame.mouse_y;
    pointer.buttons = frame.mouse_buttons;
    pointer.active = true;
    pointer.replayed = true;
}

bool jce_editor_game_input_bridge_handle_virtual_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event)
{
    if (!bridge || !event || !virtual_viewport_admitted(bridge->viewport))
        return false;
    auto &pointer = bridge->virtual_pointer;
    if (event->type == JCE_EVENT_MOUSE_WHEEL) {
        if (!pointer.active || !point_inside(bridge->viewport, pointer.x, pointer.y) ||
            !std::isfinite(event->wheel.x) || !std::isfinite(event->wheel.y) ||
            !std::isfinite(pointer.wheel + event->wheel.y)) return false;
        pointer.wheel += event->wheel.y;
        pointer.replayed = false;
        return true;
    }
    if (event->type == JCE_EVENT_MOUSE_MOTION) {
        const auto &move = event->motion;
        const auto &v = bridge->viewport;
        const float sx = v.render_width > 0 ? v.render_width / v.width : 1;
        const float sy = v.render_height > 0 ? v.render_height / v.height : 1;
        if (!point_inside(bridge->viewport, move.x, move.y) ||
            !std::isfinite(move.xrel) || !std::isfinite(move.yrel) ||
            !std::isfinite(pointer.dx + move.xrel) ||
            !std::isfinite(pointer.dy + move.yrel) ||
            !std::isfinite((pointer.dx + move.xrel) * sx) ||
            !std::isfinite((pointer.dy + move.yrel) * sy)) return false;
        pointer.x = move.x; pointer.y = move.y;
        pointer.dx += move.xrel; pointer.dy += move.yrel;
        pointer.active = true;
        pointer.replayed = false;
        return true;
    }
    if (event->type != JCE_EVENT_MOUSE_BUTTON_DOWN &&
        event->type != JCE_EVENT_MOUSE_BUTTON_UP) return false;
    const uint32_t bit = button_bit(event->button.button);
    if (!bit) return false;
    if (!point_inside(bridge->viewport, event->button.x, event->button.y)) {
        if (event->type == JCE_EVENT_MOUSE_BUTTON_UP)
            jce_editor_game_input_bridge_clear_virtual(bridge);
        return false;
    }
    if (event->type == JCE_EVENT_MOUSE_BUTTON_UP) {
        if (!pointer.active) return false;
        pointer.buttons &= ~bit;
    } else {
        pointer.buttons |= bit;
    }
    pointer.x = event->button.x; pointer.y = event->button.y;
    pointer.active = true;
    pointer.replayed = false;
    return true;
}

void jce_editor_game_input_bridge_handle_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event)
{
    if (!bridge || !event)
        return;
    if (event->type == JCE_EVENT_WINDOW_FOCUS_GAINED) {
        /* Platform events arrive before the next ImGui/panel publication.
         * Admit the following native click without waiting another frame. */
        bridge->viewport.focused = true;
        return;
    }
    if (event->type == JCE_EVENT_WINDOW_FOCUS_LOST) {
        bridge->viewport.focused = false;
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
            bridge->x = event->motion.x;
            bridge->y = event->motion.y;
            bridge->position_valid = true;
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
            bridge->x = event->button.x;
            bridge->y = event->button.y;
            bridge->position_valid = true;
        }
        break;
    }
    case JCE_EVENT_MOUSE_BUTTON_UP: {
        const uint32_t bit = button_bit(event->button.button);
        if (bit)
            bridge->buttons &= ~bit;
        if (runtime_accepts_pointer(bridge) &&
            point_inside(bridge->viewport, event->button.x, event->button.y)) {
            bridge->x = event->button.x;
            bridge->y = event->button.y;
            bridge->position_valid = true;
        }
        break;
    }
    default:
        if (!bridge->viewport.captured && !runtime_accepts_pointer(bridge))
            bridge->position_valid = false;
        break;
    }
}

JceEditorGameInputSample jce_editor_game_input_bridge_consume(
    JceEditorGameInputBridge *bridge)
{
    const JceEditorGameInputSample sample = sample_input(bridge);
    if (!bridge)
        return sample;
    bridge->dx = 0.0f;
    bridge->dy = 0.0f;
    bridge->wheel = 0.0f;
    if (bridge->virtual_pointer.active) {
        bridge->virtual_pointer.dx = bridge->virtual_pointer.dy = 0;
        bridge->virtual_pointer.wheel = 0;
    }
    return sample;
}

bool jce_editor_game_input_bridge_peek_virtual(
    const JceEditorGameInputBridge *bridge,
    const JceEditorGameInputViewport *current_viewport,
    JceEditorGameInputSample *out)
{
    if (!out) return false;
    *out = {};
    if (!bridge || !current_viewport || !bridge->virtual_pointer.active ||
        !virtual_viewport_admitted(bridge->viewport) ||
        !virtual_viewport_admitted(*current_viewport) ||
        !same_geometry(bridge->viewport, *current_viewport) ||
        !point_inside(*current_viewport, bridge->virtual_pointer.x,
                      bridge->virtual_pointer.y)) return false;
    *out = sample_input(bridge);
    return out->synthetic;
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
    if (ops->set_position)
        ops->set_position(ud, sample.x, sample.y, sample.width, sample.height);
    ops->set_touches(ud, touches, touch_count);
    ops->set_actions(ud, actions);
    ops->step(ud, dt);
    return true;
}

JceEditorGameInputBridge *jce_editor_game_input_bridge_shared(void)
{
    return &g_bridge;
}
