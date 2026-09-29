#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_game_input_bridge.h"

#include <cstring>

namespace {

JceEvent motion(float x, float y, float dx, float dy)
{
    JceEvent event{};
    event.type = JCE_EVENT_MOUSE_MOTION;
    event.motion = {x, y, dx, dy};
    return event;
}

JceEvent button(JceEventType type, uint8_t code, float x, float y)
{
    JceEvent event{};
    event.type = type;
    event.button.button = code;
    event.button.x = x;
    event.button.y = y;
    return event;
}

JceEvent wheel(float x, float y)
{
    JceEvent event{};
    event.type = JCE_EVENT_MOUSE_WHEEL;
    event.wheel = {x, y};
    return event;
}

struct OrderProbe {
    char calls[8] = {};
    int count = 0;
};

void record(OrderProbe *probe, char call)
{
    REQUIRE(probe->count < (int)sizeof(probe->calls));
    probe->calls[probe->count++] = call;
}

void set_pointer(void *ud, float, float, float, uint32_t)
{
    record(static_cast<OrderProbe *>(ud), 'P');
}

void set_touches(void *ud, const JceRuntimeTouch *, int)
{
    record(static_cast<OrderProbe *>(ud), 'T');
}

void set_actions(void *ud, const JceInputActions *)
{
    record(static_cast<OrderProbe *>(ud), 'A');
}

void step(void *ud, float)
{
    record(static_cast<OrderProbe *>(ud), 'S');
}

} // namespace

TEST_CASE("runtime-owned viewport receives bounded pointer events")
{
    JceEditorGameInputBridge bridge{};
    jce_editor_game_input_bridge_reset(&bridge);

    JceEvent outside = motion(20, 20, 4, 5);
    jce_editor_game_input_bridge_handle_event(&bridge, &outside);
    JceEditorGameInputSample sample =
        jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == 0.0f);
    CHECK(sample.dy == 0.0f);

    JceEditorGameInputViewport viewport{};
    viewport.x = 100;
    viewport.y = 50;
    viewport.width = 1280;
    viewport.height = 720;
    viewport.visible = true;
    viewport.hovered = true;
    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);

    JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                           JCE_MOUSE_BUTTON_LEFT, 120, 80);
    JceEvent move_a = motion(120, 80, 2.5f, -1.0f);
    JceEvent move_b = motion(140, 90, 1.5f, 3.0f);
    JceEvent scroll = wheel(0.0f, 2.0f);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    jce_editor_game_input_bridge_handle_event(&bridge, &move_a);
    jce_editor_game_input_bridge_handle_event(&bridge, &move_b);
    jce_editor_game_input_bridge_handle_event(&bridge, &scroll);

    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == doctest::Approx(4.0f));
    CHECK(sample.dy == doctest::Approx(2.0f));
    CHECK(sample.wheel == doctest::Approx(2.0f));
    CHECK(sample.buttons == 1u);

    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == 0.0f);
    CHECK(sample.dy == 0.0f);
    CHECK(sample.wheel == 0.0f);
    CHECK(sample.buttons == 1u);

    JceEvent up = button(JCE_EVENT_MOUSE_BUTTON_UP,
                         JCE_MOUSE_BUTTON_LEFT, 2000, 2000);
    jce_editor_game_input_bridge_handle_event(&bridge, &up);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.buttons == 0u);
}

TEST_CASE("focus loss and ownership changes cannot strand runtime input")
{
    JceEditorGameInputBridge bridge{};
    JceEditorGameInputViewport viewport{};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = 1280;
    viewport.height = 720;
    viewport.visible = true;
    viewport.hovered = true;
    viewport.captured = true;
    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);

    JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                           JCE_MOUSE_BUTTON_LEFT, -100, -100);
    JceEvent move = motion(-100, -100, 8, 9);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    jce_editor_game_input_bridge_handle_event(&bridge, &move);

    JceEvent lost{};
    lost.type = JCE_EVENT_WINDOW_FOCUS_LOST;
    jce_editor_game_input_bridge_handle_event(&bridge, &lost);
    JceEditorGameInputSample sample =
        jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == 0.0f);
    CHECK(sample.buttons == 0u);

    viewport.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
    viewport.captured = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    jce_editor_game_input_bridge_handle_event(&bridge, &move);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == 0.0f);
    CHECK(sample.owner == JCE_EDITOR_GAME_INPUT_FREE_FLY);

    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    JceEvent outside = motion(2000, 2000, 5, 6);
    jce_editor_game_input_bridge_handle_event(&bridge, &outside);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.dx == 0.0f);
}

TEST_CASE("commit order is pointer touch actions then runtime step")
{
    JceEditorGameInputBridge bridge{};
    JceEditorGameInputViewport viewport{};
    viewport.width = 1280;
    viewport.height = 720;
    viewport.visible = true;
    viewport.hovered = true;
    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);

    JceRuntimeTouch touches[1] = {{7u, 0.25f, 0.5f, 1.0f}};
    JceEditorGameInputCommitOps ops{};
    ops.set_pointer = set_pointer;
    ops.set_touches = set_touches;
    ops.set_actions = set_actions;
    ops.step = step;
    OrderProbe probe{};

    REQUIRE(jce_editor_game_input_bridge_commit(
        &bridge, touches, 1, nullptr, 1.0f / 60.0f, &ops, &probe));
    REQUIRE(probe.count == 4);
    CHECK(std::memcmp(probe.calls, "PTAS", 4) == 0);
}
