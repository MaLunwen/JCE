#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_state.h"

namespace {
struct Point { float x, y; };
struct ImGuiIO { Point MousePos; };
enum { ImGuiMouseButton_Left = 0 };
ImGuiIO g_io{};
bool g_native_down = false, g_captured = false;
JcePlayState g_play = JCE_PLAY_PLAYING;
int g_set_count = 0, g_text_count = 0;
float g_x = 0, g_y = 0;
bool g_down = false, g_valid = false;
namespace ImGui {
const ImGuiIO &GetIO() { return g_io; }
bool IsMouseDown(int button) { CHECK(button == ImGuiMouseButton_Left); return g_native_down; }
}
JcePlayState observed_play_state() { return g_play; }
bool jce_editor_game_render_is_mouse_captured() { return g_captured; }
void forward_text_input_to_canvas() { ++g_text_count; }
void jce_editor_game_render_set_ui_pointer(float x, float y, bool down, bool valid)
{
    ++g_set_count; g_x = x; g_y = y; g_down = down; g_valid = valid;
}

void draw_pointer(bool viewport_work = true, Point region_start = {100, 50},
                  Point view_size = {640, 360}, uint32_t canvas_w = 1280, uint32_t canvas_h = 720)
{
#define jce_state_get_play_state observed_play_state
#include JCE_TEST_GAME_CANVAS_POINTER_OWNER
#undef jce_state_get_play_state
}

JceEvent event(JceEventType type)
{
    JceEvent value{}; value.type = type;
    value.button.x = 420; value.button.y = 230;
    value.button.button = JCE_MOUSE_BUTTON_LEFT;
    return value;
}

void reset()
{
    g_io.MousePos = {-100, -100};
    g_native_down = g_captured = false; g_play = JCE_PLAY_PLAYING;
    g_set_count = g_text_count = 0;
    g_x = g_y = 0; g_down = g_valid = false;
    auto *bridge = jce_editor_game_input_bridge_shared();
    jce_editor_game_input_bridge_reset(bridge);
    JceEditorGameInputViewport viewport{};
    viewport.x = 100; viewport.y = 50; viewport.width = 640; viewport.height = 360;
    viewport.render_width = 1280; viewport.render_height = 720;
    viewport.visible = true; viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    jce_editor_game_input_bridge_publish(bridge, &viewport);
}
}

TEST_CASE("actual Canvas pointer block sends held virtual down and release with OS cursor outside")
{
    reset();
    auto *bridge = jce_editor_game_input_bridge_shared();
    auto down = event(JCE_EVENT_MOUSE_BUTTON_DOWN);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(bridge, &down));
    // The normal update consumes before the panel draws; held state remains.
    const auto runtime = jce_editor_game_input_bridge_consume(bridge);
    REQUIRE(runtime.synthetic); CHECK(runtime.buttons == 1u);
    draw_pointer();
    CHECK(g_set_count == 1); CHECK(g_text_count == 0);
    CHECK(g_valid); CHECK(g_down); CHECK(g_x == 640); CHECK(g_y == 360);
    CHECK_FALSE(bridge->viewport.focused); CHECK_FALSE(bridge->viewport.hovered);
    auto up = event(JCE_EVENT_MOUSE_BUTTON_UP);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(bridge, &up));
    draw_pointer();
    CHECK(g_set_count == 2); CHECK(g_valid); CHECK_FALSE(g_down);
    CHECK(jce_editor_game_input_bridge_consume(bridge).buttons == 0u);
}

TEST_CASE("actual Canvas native text capture and stopped gates retain their original values")
{
    reset();
    g_io.MousePos = {420, 230}; g_native_down = true;
    draw_pointer();
    CHECK(g_valid); CHECK(g_down); CHECK(g_x == 640); CHECK(g_y == 360); CHECK(g_text_count == 1);
    g_captured = true;
    draw_pointer();
    CHECK_FALSE(g_valid); CHECK_FALSE(g_down); CHECK(g_text_count == 1);
    g_captured = false; g_play = JCE_PLAY_STOPPED;
    draw_pointer();
    CHECK_FALSE(g_valid); CHECK_FALSE(g_down); CHECK(g_text_count == 1);
    g_play = JCE_PLAY_PAUSED;
    draw_pointer();
    CHECK_FALSE(g_valid); CHECK_FALSE(g_down); CHECK(g_text_count == 1);
}

TEST_CASE("actual Canvas virtual route refuses covered changed FBO rect Stop and cleared owner")
{
    reset();
    auto *bridge = jce_editor_game_input_bridge_shared();
    auto down = event(JCE_EVENT_MOUSE_BUTTON_DOWN);
    REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(bridge, &down));
    draw_pointer(false); CHECK_FALSE(g_valid);
    draw_pointer(true, {101, 50}); CHECK_FALSE(g_valid);
    draw_pointer(true, {100, 50}, {640, 360}, 640, 360); CHECK_FALSE(g_valid);
    g_play = JCE_PLAY_STOPPED; draw_pointer(); CHECK_FALSE(g_valid);
    g_play = JCE_PLAY_PLAYING;
    draw_pointer(); CHECK(g_valid); CHECK(g_down);
    jce_editor_game_input_bridge_reset(bridge);
    draw_pointer(); CHECK_FALSE(g_valid); CHECK_FALSE(g_down);
    CHECK(g_text_count == 0);
}
