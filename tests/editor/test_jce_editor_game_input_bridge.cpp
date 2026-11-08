#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_game_input_bridge.h"

#include <cstring>
#include <limits>

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
    float position[4] = {};
    float pointer[3] = {};
    uint32_t buttons = 0;
};

void record(OrderProbe *probe, char call)
{
    REQUIRE(probe->count < (int)sizeof(probe->calls));
    probe->calls[probe->count++] = call;
}

void set_pointer(void *ud, float dx, float dy, float wheel_value, uint32_t buttons)
{
    auto *probe = static_cast<OrderProbe *>(ud);
    record(probe, 'P');
    probe->pointer[0] = dx; probe->pointer[1] = dy;
    probe->pointer[2] = wheel_value; probe->buttons = buttons;
}

void set_touches(void *ud, const JceRuntimeTouch *, int)
{
    record(static_cast<OrderProbe *>(ud), 'T');
}

void set_position(void *ud, float x, float y, float width, float height)
{
    auto *probe = static_cast<OrderProbe *>(ud);
    record(probe, 'V');
    probe->position[0] = x; probe->position[1] = y;
    probe->position[2] = width; probe->position[3] = height;
}

void set_actions(void *ud, const JceInputActions *)
{
    record(static_cast<OrderProbe *>(ud), 'A');
}

void step(void *ud, float)
{
    record(static_cast<OrderProbe *>(ud), 'S');
}

JceEditorGameInputViewport hover_viewport()
{
    JceEditorGameInputViewport viewport{};
    viewport.x = 100; viewport.y = 50;
    viewport.width = 640; viewport.height = 360;
    viewport.render_width = 1280; viewport.render_height = 720;
    viewport.visible = viewport.hovered = viewport.focused = true;
    viewport.pointer_valid = true;
    viewport.pointer_x = 420; viewport.pointer_y = 230;
    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    return viewport;
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
    viewport.focused = true;
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

    CHECK(sample.x == doctest::Approx(40.0f));
    CHECK(sample.y == doctest::Approx(40.0f));
    CHECK(sample.width == 1280.0f);
    CHECK(sample.height == 720.0f);

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
    viewport.focused = true;
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
    CHECK(sample.width == 0.0f);

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
    viewport.focused = true;
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

TEST_CASE("displayed Game image maps pointer and displacement to render pixels")
{
    JceEditorGameInputBridge bridge{};
    JceEditorGameInputViewport viewport{};
    viewport.x = 100; viewport.y = 50;
    viewport.width = 640; viewport.height = 360;
    viewport.render_width = 1280; viewport.render_height = 720;
    viewport.visible = viewport.hovered = true;
    viewport.focused = true;
    viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    JceEvent event = motion(420, 230, 30, -15);
    jce_editor_game_input_bridge_handle_event(&bridge, &event);
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.x == 640); CHECK(sample.y == 360);
    CHECK(sample.width == 1280); CHECK(sample.height == 720);
    CHECK(sample.dx == 60); CHECK(sample.dy == -30);
}

TEST_CASE("hover publication primes position without a second native event")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    /* The entry motion arrives before the panel publishes its new hover.
     * The real cursor snapshot below must prime position without replaying it. */
    JceEvent entry = motion(420, 230, 30, -15);
    jce_editor_game_input_bridge_handle_event(&bridge, &entry);
    CHECK(jce_editor_game_input_bridge_consume(&bridge).width == 0);
    viewport.hovered = true;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.x == 640); CHECK(sample.y == 360);
    CHECK(sample.width == 1280); CHECK(sample.height == 720);
    CHECK(sample.buttons == 0u);
    CHECK(sample.dx == 0); CHECK(sample.dy == 0); CHECK(sample.wheel == 0);
}

TEST_CASE("render size reaches the runtime before its first native click")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    JceEditorGameInputCommitOps ops{};
    ops.set_pointer = set_pointer; ops.set_position = set_position;
    ops.set_touches = set_touches; ops.set_actions = set_actions; ops.step = step;
    OrderProbe probe{};
    REQUIRE(jce_editor_game_input_bridge_commit(
        &bridge, nullptr, 0, nullptr, 1.0f / 60.0f, &ops, &probe));
    REQUIRE(probe.count == 5);
    CHECK(std::memcmp(probe.calls, "PVTAS", 5) == 0);
    REQUIRE(probe.position[2] == 1280);
    REQUIRE(probe.position[3] == 720);
    JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                           JCE_MOUSE_BUTTON_LEFT, 420, 230);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.buttons == 1u);
    CHECK(sample.width == probe.position[2]);
    CHECK(sample.height == probe.position[3]);
}

TEST_CASE("publication retains admitted native position and accumulated edges")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                           JCE_MOUSE_BUTTON_LEFT, 450, 240);
    JceEvent move = motion(450, 240, 1, 2);
    JceEvent scroll = wheel(0, 3);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    jce_editor_game_input_bridge_handle_event(&bridge, &move);
    jce_editor_game_input_bridge_handle_event(&bridge, &scroll);
    viewport.pointer_x = 500; viewport.pointer_y = 300;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.x == 700); CHECK(sample.y == 380);
    CHECK(sample.dx == 2); CHECK(sample.dy == 4);
    CHECK(sample.wheel == 3); CHECK(sample.buttons == 1u);
    JceEvent up = button(JCE_EVENT_MOUSE_BUTTON_UP,
                         JCE_MOUSE_BUTTON_LEFT, 2000, 2000);
    jce_editor_game_input_bridge_handle_event(&bridge, &up);
    CHECK(jce_editor_game_input_bridge_consume(&bridge).buttons == 0u);
}

TEST_CASE("native focus regain admits its first click before another publication")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                           JCE_MOUSE_BUTTON_LEFT, 450, 240);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    JceEvent lost{}; lost.type = JCE_EVENT_WINDOW_FOCUS_LOST;
    jce_editor_game_input_bridge_handle_event(&bridge, &lost);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.buttons == 0u); CHECK(sample.width == 0);
    JceEvent gained{}; gained.type = JCE_EVENT_WINDOW_FOCUS_GAINED;
    jce_editor_game_input_bridge_handle_event(&bridge, &gained);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.buttons == 1u); CHECK(sample.width == 1280);
    CHECK(sample.x == 700); CHECK(sample.y == 380);
}

TEST_CASE("hover focus visibility and owner loss clear held pointer state")
{
    for (int mode = 0; mode < 4; ++mode) {
        CAPTURE(mode);
        JceEditorGameInputBridge bridge{};
        auto viewport = hover_viewport();
        jce_editor_game_input_bridge_publish(&bridge, &viewport);
        JceEvent down = button(JCE_EVENT_MOUSE_BUTTON_DOWN,
                               JCE_MOUSE_BUTTON_LEFT, 450, 240);
        JceEvent move = motion(450, 240, 1, 2);
        JceEvent scroll = wheel(0, 3);
        jce_editor_game_input_bridge_handle_event(&bridge, &down);
        jce_editor_game_input_bridge_handle_event(&bridge, &move);
        jce_editor_game_input_bridge_handle_event(&bridge, &scroll);
        if (mode == 0) viewport.hovered = false;
        if (mode == 1) viewport.focused = false;
        if (mode == 2) viewport.visible = false;
        if (mode == 3) viewport.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
        jce_editor_game_input_bridge_publish(&bridge, &viewport);
        const auto sample = jce_editor_game_input_bridge_consume(&bridge);
        CHECK(sample.width == 0); CHECK(sample.height == 0);
        CHECK(sample.buttons == 0u);
        CHECK(sample.dx == 0); CHECK(sample.dy == 0); CHECK(sample.wheel == 0);
    }
}

TEST_CASE("invalid viewport or cursor snapshots cannot prime runtime input")
{
    for (int mode = 0; mode < 5; ++mode) {
        CAPTURE(mode);
        JceEditorGameInputBridge bridge{};
        auto viewport = hover_viewport();
        if (mode == 0) viewport.width = 0;
        if (mode == 1) viewport.height = std::numeric_limits<float>::quiet_NaN();
        if (mode == 2) viewport.pointer_valid = false;
        if (mode == 3) viewport.pointer_x = std::numeric_limits<float>::infinity();
        if (mode == 4) viewport.pointer_x = 2000;
        jce_editor_game_input_bridge_publish(&bridge, &viewport);
        const auto sample = jce_editor_game_input_bridge_consume(&bridge);
        CHECK(sample.width == 0); CHECK(sample.height == 0);
        CHECK(sample.buttons == 0u); CHECK(sample.dx == 0); CHECK(sample.wheel == 0);
    }
}

TEST_CASE("capture uses native motion and never primes a pinned UI cursor")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.captured = true;
    viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    CHECK(jce_editor_game_input_bridge_consume(&bridge).width == 0);
    JceEvent move = motion(-100, -100, 8, 9);
    jce_editor_game_input_bridge_handle_event(&bridge, &move);
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.width == 1280); CHECK(sample.height == 720);
    CHECK(sample.dx == 16); CHECK(sample.dy == 18);
    CHECK(sample.x == -400); CHECK(sample.y == -300);
}

TEST_CASE("explicit virtual pointer reaches an unfocused runtime without admitting OS events")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.focused = viewport.hovered = viewport.pointer_valid = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto move = motion(420, 230, 3, -2);
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    jce_editor_game_input_bridge_handle_event(&bridge, &move);
    jce_editor_game_input_bridge_handle_event(&bridge, &down);
    CHECK(jce_editor_game_input_bridge_consume(&bridge).width == 0);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &move));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.synthetic); CHECK(sample.buttons == 1u);
    CHECK(sample.x == 640); CHECK(sample.y == 360);
    CHECK(sample.dx == 6); CHECK(sample.dy == -4);
    CHECK(sample.width == 1280); CHECK(sample.height == 720);
    CHECK_FALSE(bridge.viewport.focused); CHECK_FALSE(bridge.viewport.hovered);
    CHECK_FALSE(bridge.viewport.captured); CHECK_FALSE(bridge.viewport.pointer_valid);
}

TEST_CASE("virtual cursor buttons persist while transient deltas drain and UP commits")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.focused = viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto scroll = wheel(0, 2);
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &scroll));
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    auto move = motion(430, 240, 4, 5);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &move));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &scroll));
    auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.synthetic); CHECK(sample.buttons == 1u);
    CHECK(sample.dx == 8); CHECK(sample.dy == 10); CHECK(sample.wheel == 2);
    viewport.focused = true;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.synthetic); CHECK(sample.buttons == 1u);
    CHECK(sample.dx == 0); CHECK(sample.dy == 0); CHECK(sample.wheel == 0);
    auto up = button(JCE_EVENT_MOUSE_BUTTON_UP, JCE_MOUSE_BUTTON_LEFT, 430, 240);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &up));
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(sample.synthetic); CHECK(sample.buttons == 0u); CHECK(sample.width == 1280);
    jce_editor_game_input_bridge_clear_virtual(&bridge);
    sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK_FALSE(sample.synthetic); CHECK(sample.width == 0); CHECK(sample.buttons == 0u);
}

TEST_CASE("virtual state is cancelled on hide Stop owner geometry or reset")
{
    for (int mode = 0; mode < 10; ++mode) {
        CAPTURE(mode);
        JceEditorGameInputBridge bridge{};
        auto viewport = hover_viewport();
        viewport.focused = viewport.hovered = false;
        jce_editor_game_input_bridge_publish(&bridge, &viewport);
        auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
        REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
        if (mode == 0) viewport.visible = false;
        if (mode == 1) viewport.owner = JCE_EDITOR_GAME_INPUT_NONE;
        if (mode == 2) viewport.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
        if (mode == 3) viewport.x += 1;
        if (mode == 4) viewport.y += 1;
        if (mode == 5) viewport.width += 1;
        if (mode == 6) viewport.height += 1;
        if (mode == 7) viewport.render_width += 1;
        if (mode == 8) viewport.render_height += 1;
        if (mode == 9) jce_editor_game_input_bridge_reset(&bridge);
        else jce_editor_game_input_bridge_publish(&bridge, &viewport);
        const auto sample = jce_editor_game_input_bridge_consume(&bridge);
        CHECK_FALSE(sample.synthetic); CHECK(sample.buttons == 0u);
        CHECK(sample.width == 0); CHECK(sample.wheel == 0);
    }
}

TEST_CASE("virtual admission rejects invalid geometry payload keys and outside presses")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.focused = viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto outside = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 99, 230);
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &outside));
    auto move = motion(420, 230, std::numeric_limits<float>::infinity(), 0);
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &move));
    auto scroll = wheel(0, 1);
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &scroll));
    JceEvent key{}; key.type = JCE_EVENT_KEY_DOWN;
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &key));
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, nullptr));
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(nullptr, &key));
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    for (int mode = 0; mode < 5; ++mode) {
        auto bad = viewport;
        if (mode == 0) bad.visible = false;
        if (mode == 1) bad.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
        if (mode == 2) bad.render_width = std::numeric_limits<float>::infinity();
        if (mode == 3) bad.render_height = -1;
        if (mode == 4) bad.height = 0;
        jce_editor_game_input_bridge_publish(&bridge, &bad);
        CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
        CHECK_FALSE(bridge.virtual_pointer.active);
    }
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    auto up = button(JCE_EVENT_MOUSE_BUTTON_UP, JCE_MOUSE_BUTTON_LEFT, 2000, 2000);
    CHECK_FALSE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &up));
    auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK_FALSE(sample.synthetic); CHECK(sample.buttons == 0u); CHECK(sample.width == 0);
}

TEST_CASE("virtual source uses the actual pre-step commit and does not merge native input")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto native = motion(500, 300, 10, 20);
    jce_editor_game_input_bridge_handle_event(&bridge, &native);
    auto move = motion(420, 230, 2, 3);
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &move));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    JceEditorGameInputCommitOps ops{};
    ops.set_pointer = set_pointer; ops.set_position = set_position;
    ops.set_touches = set_touches; ops.set_actions = set_actions; ops.step = step;
    OrderProbe probe{};
    REQUIRE(jce_editor_game_input_bridge_commit(&bridge, nullptr, 0, nullptr, 1.0f/60, &ops, &probe));
    REQUIRE(probe.count == 5); CHECK(std::memcmp(probe.calls, "PVTAS", 5) == 0);
    CHECK(probe.pointer[0] == 4); CHECK(probe.pointer[1] == 6); CHECK(probe.buttons == 1u);
    CHECK(probe.position[0] == 640); CHECK(probe.position[1] == 360);
    CHECK(probe.position[2] == 1280); CHECK(probe.position[3] == 720);
    jce_editor_game_input_bridge_clear_virtual(&bridge);
    const auto sample = jce_editor_game_input_bridge_consume(&bridge);
    CHECK_FALSE(sample.synthetic); CHECK(sample.dx == 0); CHECK(sample.dy == 0);
    CHECK(sample.buttons == 0u); CHECK(sample.x == 800); CHECK(sample.y == 500);
}

TEST_CASE("without an explicit virtual source native ownership and callback count are unchanged")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    CHECK_FALSE(bridge.virtual_pointer.active);
    CHECK_FALSE(jce_editor_game_input_bridge_consume(&bridge).synthetic);
    jce_editor_game_input_bridge_reset(&bridge);
    CHECK_FALSE(bridge.virtual_pointer.active);
    CHECK(bridge.virtual_pointer.buttons == 0u);
    jce_editor_game_input_bridge_clear_virtual(nullptr);
}

TEST_CASE("Canvas peek retains runtime transients buttons and mapped release without consumption")
{
    JceEditorGameInputBridge bridge{};
    auto viewport = hover_viewport();
    viewport.focused = viewport.hovered = false;
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto move = motion(420, 230, 3, 4);
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    auto scroll = wheel(0, 2);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &move));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &scroll));
    for (int i = 0; i < 2; ++i) {
        JceEditorGameInputSample sample{};
        REQUIRE(jce_editor_game_input_bridge_peek_virtual(&bridge, &viewport, &sample));
        CHECK(sample.synthetic);
        CHECK(sample.x == 640); CHECK(sample.y == 360);
        CHECK(sample.dx == 6); CHECK(sample.dy == 8);
        CHECK(sample.wheel == 2); CHECK(sample.buttons == 1u);
    }
    const auto consumed = jce_editor_game_input_bridge_consume(&bridge);
    CHECK(consumed.dx == 6); CHECK(consumed.dy == 8); CHECK(consumed.wheel == 2);
    JceEditorGameInputSample sample{};
    REQUIRE(jce_editor_game_input_bridge_peek_virtual(&bridge, &viewport, &sample));
    CHECK(sample.dx == 0); CHECK(sample.buttons == 1u);
    auto up = button(JCE_EVENT_MOUSE_BUTTON_UP, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &up));
    REQUIRE(jce_editor_game_input_bridge_peek_virtual(&bridge, &viewport, &sample));
    CHECK(sample.buttons == 0u); CHECK(sample.x == 640);
}

TEST_CASE("Canvas virtual peek rejects current hidden changed geometry and owner without draining")
{
    JceEditorGameInputBridge bridge{};
    const auto viewport = hover_viewport();
    jce_editor_game_input_bridge_publish(&bridge, &viewport);
    auto down = button(JCE_EVENT_MOUSE_BUTTON_DOWN, JCE_MOUSE_BUTTON_LEFT, 420, 230);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&bridge, &down));
    for (int mode = 0; mode < 7; ++mode) {
        auto current = viewport;
        if (mode == 0) current.visible = false;
        if (mode == 1) current.owner = JCE_EDITOR_GAME_INPUT_NONE;
        if (mode == 2) current.x += 1;
        if (mode == 3) current.width += 1;
        if (mode == 4) current.render_width += 1;
        if (mode == 5) current.render_height = std::numeric_limits<float>::infinity();
        JceEditorGameInputSample sample{}; sample.buttons = 99;
        CHECK_FALSE(jce_editor_game_input_bridge_peek_virtual(&bridge, mode == 6 ? nullptr : &current, &sample));
        CHECK(sample.buttons == 0u); CHECK_FALSE(sample.synthetic);
        CHECK(bridge.virtual_pointer.buttons == 1u);
    }
    JceEditorGameInputSample sample{};
    CHECK_FALSE(jce_editor_game_input_bridge_peek_virtual(nullptr, &viewport, &sample));
    CHECK_FALSE(jce_editor_game_input_bridge_peek_virtual(&bridge, &viewport, nullptr));
    jce_editor_game_input_bridge_clear_virtual(&bridge);
    CHECK_FALSE(jce_editor_game_input_bridge_peek_virtual(&bridge, &viewport, &sample));
}
