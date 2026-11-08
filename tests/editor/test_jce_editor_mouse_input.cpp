/* Complete Editor event consumer with genuine ImGui CPU button behavior. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_key_event.h"
#include <cfloat>
#include <cmath>
#include <vector>

namespace {
struct EditorState { bool initialized = true, active = true; } s_editor;
struct ButtonEdge { int button; bool down; };
struct Ports {
    bool captured = false;
    unsigned bridge_events = 0, relative_events = 0;
    std::vector<ButtonEdge> captured_edges;
} ports;
JceEditorGameInputBridge bridge{};
bool jce_editor_pref_touchpad_h_invert = false;
bool jce_editor_hotkey_diag_enabled() { return false; }
void jce_editor_hotkey_diag_event(const JceEvent &, int, int, bool, bool) {}
ImGuiKey jce_key_to_imgui_key(JceKey) { return ImGuiKey_None; }
bool jce_editor_game_render_is_mouse_captured() { return ports.captured; }
void jce_editor_game_render_push_mouse_delta(float, float) { ++ports.relative_events; }
void jce_editor_game_render_push_mouse_button(int button, bool down)
{
    ports.captured_edges.push_back({button, down});
}
}
JceEditorGameInputBridge *jce_editor_game_input_bridge_shared() { return &bridge; }
void jce_editor_game_input_bridge_handle_event(JceEditorGameInputBridge *target,
                                              const JceEvent *)
{
    CHECK(target == &bridge);
    ++ports.bridge_events;
}
#ifndef JCE_EDITOR_MOUSE_INPUT_OWNER
#error The registered fixture must consume the complete actual Editor event owner
#endif
#include JCE_EDITOR_MOUSE_INPUT_OWNER

namespace {
struct Fixture {
    JceTestImGuiContext gui;
    ImVec2 origin, other{}, target{};
    unsigned target_clicks = 0, other_clicks = 0;
    explicit Fixture(ImVec2 window_origin = ImVec2(80, 60)) : origin(window_origin)
    {
        ports = {};
        s_editor = {};
        for (int i = 0; i < 3; ++i) frame();
    }
    void frame()
    {
        gui.begin();
        ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(620, 430), ImGuiCond_Always);
        ImGui::Begin("Mouse delivery", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 30, origin.y + 40));
        if (ImGui::SmallButton("Other")) ++other_clicks;
        ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        other = ImVec2(std::floor((lo.x + hi.x) * .5f), std::floor((lo.y + hi.y) * .5f));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 300, origin.y + 220));
        if (ImGui::SmallButton("Max")) ++target_clicks;
        lo = ImGui::GetItemRectMin(); hi = ImGui::GetItemRectMax();
        target = ImVec2(std::floor((lo.x + hi.x) * .5f), std::floor((lo.y + hi.y) * .5f));
        ImGui::End();
        gui.end();
        ImGui::Render();
    }
    void drain() { for (int i = 0; i < 8; ++i) frame(); }
    void motion(ImVec2 position)
    {
        JceEvent event{};
        event.type = JCE_EVENT_MOUSE_MOTION;
        event.motion.x = position.x; event.motion.y = position.y;
        event.motion.xrel = 2; event.motion.yrel = -3;
        jce_editor_process_event(&event);
    }
    void button(ImVec2 position, bool down, uint8_t code = JCE_MOUSE_BUTTON_LEFT)
    {
        JceEvent event{};
        event.type = down ? JCE_EVENT_MOUSE_BUTTON_DOWN : JCE_EVENT_MOUSE_BUTTON_UP;
        event.button.button = code; event.button.clicks = 1;
        event.button.x = position.x; event.button.y = position.y;
        jce_editor_process_event(&event);
    }
    void short_click(ImVec2 position)
    {
        button(position, true);
        button(position, false); // One host pump, without a NewFrame between edges.
    }
    void idle()
    {
        CHECK_FALSE(ImGui::GetIO().MouseDown[0]);
        CHECK(ImGui::GetIO().ConfigInputTrickleEventQueue);
    }
};
}

TEST_CASE("button coordinates admit a short click without any prior motion")
{
    Fixture f;
    CHECK_FALSE(ImGui::IsMousePosValid());
    f.short_click(f.target);
    f.drain();
    CHECK(f.target_clicks == 1);
    CHECK(f.other_clicks == 0);
    CHECK(ImGui::GetIO().MousePos.x == f.target.x);
    CHECK(ImGui::GetIO().MousePos.y == f.target.y);
    CHECK(ports.bridge_events == 2);
    CHECK(ports.captured_edges.empty());
    f.idle();
}

TEST_CASE("button position replaces stale other-widget coordinates before both edges")
{
    Fixture f;
    f.motion(f.other); f.drain();
    f.short_click(f.target); f.drain();
    CHECK(f.target_clicks == 1);
    CHECK(f.other_clicks == 0);
    CHECK(ports.relative_events == 1);
    f.idle();
}

TEST_CASE("ordinary motion and same-pump down up preserve exactly one genuine activation")
{
    Fixture f;
    f.motion(f.target); f.short_click(f.target); f.drain();
    CHECK(f.target_clicks == 1);
    CHECK(f.other_clicks == 0);
    f.drain();
    CHECK(f.target_clicks == 1);
    f.idle();
}

TEST_CASE("release coordinates outside the active button cancel rather than click stale position")
{
    Fixture f;
    f.motion(f.target); f.drain();
    f.button(f.target, true); f.frame();
    REQUIRE(ImGui::GetIO().MouseDown[0]);
    f.button(f.other, false); f.drain();
    CHECK(f.target_clicks == 0);
    CHECK(f.other_clicks == 0);
    f.idle();
}

TEST_CASE("capture keeps UI sentinel and owns both edges then releases without a motion")
{
    Fixture f;
    ports.captured = true;
    f.motion(f.target); f.short_click(f.target); f.drain();
    CHECK(f.target_clicks == 0);
    CHECK(f.other_clicks == 0);
    CHECK_FALSE(ImGui::IsMousePosValid());
    REQUIRE(ports.captured_edges.size() == 2);
    CHECK(ports.captured_edges[0].button == 0);
    CHECK(ports.captured_edges[0].down);
    CHECK(ports.captured_edges[1].button == 0);
    CHECK_FALSE(ports.captured_edges[1].down);
    CHECK(ports.relative_events == 1);
    f.idle();
    ports.captured = false;
    f.short_click(f.target); f.drain();
    CHECK(f.target_clicks == 1);
    CHECK(ports.bridge_events == 5);
    CHECK(ports.captured_edges.size() == 2);
    f.idle();
}

TEST_CASE("host-relative button coordinates match motion without adding the panel offset")
{
    Fixture f(ImVec2(135, 105));
    f.motion(f.other); f.drain();
    f.short_click(f.target); f.drain();
    CHECK(f.target_clicks == 1);
    CHECK(f.other_clicks == 0);
    CHECK(ImGui::GetIO().MousePos.x == f.target.x);
    CHECK(ImGui::GetIO().MousePos.y == f.target.y);
    f.idle();
}

TEST_CASE("unrecognized buttons do not move or click the UI")
{
    Fixture f;
    f.motion(f.other); f.drain();
    f.button(f.target, true, 255); f.button(f.target, false, 255); f.drain();
    CHECK(f.target_clicks == 0);
    CHECK(f.other_clicks == 0);
    CHECK(ImGui::GetIO().MousePos.x == f.other.x);
    CHECK(ImGui::GetIO().MousePos.y == f.other.y);
    CHECK(ports.captured_edges.empty());
    f.idle();
}

TEST_CASE("inactive and uninitialized Editor admission remains before bridge and UI")
{
    for (int blocked = 0; blocked < 2; ++blocked) {
        Fixture f;
        s_editor.initialized = blocked != 0;
        s_editor.active = blocked == 0;
        f.short_click(f.target); f.drain();
        CHECK(f.target_clicks == 0);
        CHECK(f.other_clicks == 0);
        CHECK(ports.bridge_events == 0);
        CHECK(ports.captured_edges.empty());
        CHECK_FALSE(ImGui::IsMousePosValid());
        f.idle();
    }
}
