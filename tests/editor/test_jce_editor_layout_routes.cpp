#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_defaults.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_viewport_trace.h"
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_perf_phase.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

/* Typed DockBuilder ports consume the complete production preset bodies.
 * Last assignment models the shared stable window-ID routing contract; these
 * CPU controls do not create an ImGui context or certify GUI geometry. */
using ImGuiID = uint32_t;
enum ImGuiDir { ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down };
enum { ImGuiDockNodeFlags_None = 0 };
struct ImVec2 {
    float x, y;
    ImVec2(float px, float py) : x(px), y(py) {}
};
struct ImGuiViewport {
    ImVec2 WorkPos{0.0f, 0.0f}, WorkSize{1600.0f, 900.0f};
    ImGuiID ID = 1;
};

namespace {

struct Rect { double x = 0, y = 0, width = 0, height = 0; };
struct DockPorts {
    ImGuiViewport viewport;
    std::map<ImGuiID, Rect> nodes;
    std::map<std::string, ImGuiID> windows;
    std::map<std::string, unsigned> calls;
    ImGuiID next = 1;
    unsigned remove = 0, add = 0, size = 0, split = 0, finish = 0;
} ports;

std::string stable_id(const char *name)
{
    const std::string label(name);
    const auto suffix = label.rfind("###");
    REQUIRE(suffix != std::string::npos);
    return label.substr(suffix);
}

} // namespace

namespace ImGui {

const ImGuiViewport *GetMainViewport() { return &ports.viewport; }
void DockBuilderRemoveNode(ImGuiID root)
{
    CHECK(root == 1);
    ports = DockPorts{};
    ++ports.remove;
}
ImGuiID DockBuilderAddNode(ImGuiID root, int flags)
{
    CHECK(root == 1);
    CHECK(flags == ImGuiDockNodeFlags_None);
    ports.nodes[root] = Rect{};
    ++ports.add;
    return root;
}
void DockBuilderSetNodeSize(ImGuiID root, ImVec2 size)
{
    REQUIRE(ports.nodes.count(root) == 1);
    ports.nodes[root] = Rect{0, 0, size.x, size.y};
    ++ports.size;
}
ImGuiID DockBuilderSplitNode(ImGuiID parent, ImGuiDir dir, float ratio,
                             ImGuiID *at_dir, ImGuiID *opposite)
{
    REQUIRE(ports.nodes.count(parent) == 1);
    REQUIRE(at_dir != nullptr);
    REQUIRE(opposite != nullptr);
    REQUIRE(ratio > 0.0f);
    REQUIRE(ratio < 1.0f);
    Rect a = ports.nodes.at(parent), b = a;
    if (dir == ImGuiDir_Down) {
        a.height *= ratio; a.y += b.height - a.height;
        b.height -= a.height;
    } else if (dir == ImGuiDir_Up) {
        a.height *= ratio; b.y += a.height; b.height -= a.height;
    } else if (dir == ImGuiDir_Left) {
        a.width *= ratio; b.x += a.width; b.width -= a.width;
    } else {
        a.width *= ratio; a.x += b.width - a.width;
        b.width -= a.width;
    }
    *at_dir = ++ports.next;
    *opposite = ++ports.next;
    ports.nodes[*at_dir] = a;
    ports.nodes[*opposite] = b;
    ++ports.split;
    return *opposite;
}
void DockBuilderDockWindow(const char *name, ImGuiID node)
{
    REQUIRE(ports.nodes.count(node) == 1);
    const auto id = stable_id(name);
    ports.windows[id] = node;
    ++ports.calls[id];
}
void DockBuilderFinish(ImGuiID root)
{
    CHECK(root == 1);
    ++ports.finish;
}

} // namespace ImGui

#ifndef JCE_EDITOR_LAYOUT_ROUTES_OWNER
#error JCE_EDITOR_LAYOUT_ROUTES_OWNER must name the actual production extract
#endif
#include JCE_EDITOR_LAYOUT_ROUTES_OWNER

namespace {

ImGuiID dock(const char *id)
{
    REQUIRE(ports.windows.count(id) == 1);
    return ports.windows.at(id);
}

void check_build()
{
    CHECK(ports.remove == 1);
    CHECK(ports.add == 1);
    CHECK(ports.size == 1);
    CHECK(ports.finish == 1);
}

void check_rect(const char *id, double x, double y, double width, double height)
{
    const Rect r = ports.nodes.at(dock(id));
    CHECK(r.x == doctest::Approx(x));
    CHECK(r.y == doctest::Approx(y));
    CHECK(r.width == doctest::Approx(width));
    CHECK(r.height == doctest::Approx(height));
}

} // namespace

TEST_CASE("Profiling retains its explicit upper cluster after extension routing")
{
    apply_layout_preset(1, 8);
    check_build();
    CHECK(ports.split == 2);
    check_rect("###profiler", 0, 0, 880, 540);
    CHECK(dock("###profiler") == dock("###hierarchy"));
    CHECK(dock("###network_stats") == dock("###hierarchy"));
    CHECK(dock("###physics_debugger") == dock("###hierarchy"));
    CHECK(ports.calls.at("###profiler") == 2);
    CHECK(ports.calls.at("###network_stats") == 2);
    CHECK(ports.calls.at("###physics_debugger") == 2);
}

TEST_CASE("Profiling preserves Game Scene and other extension destinations")
{
    apply_layout_preset(1, 8);
    check_build();
    check_rect("###game_view", 880, 0, 720, 540);
    check_rect("###scene_view", 0, 540, 1600, 360);
    CHECK(dock("###scene_view") == dock("###console"));
    CHECK(dock("###build_profiles") == dock("###console"));
    CHECK(dock("###jce_terrain") == dock("###game_view"));
    CHECK(dock("###jce_material_graph") == dock("###game_view"));
}

TEST_CASE("Default keeps shared viewports and bottom Profiler routing")
{
    apply_layout_preset(1, 0);
    check_build();
    CHECK(ports.split == 3);
    CHECK(dock("###profiler") == dock("###console"));
    CHECK(dock("###game_view") == dock("###scene_view"));
    CHECK(dock("###network_stats") == dock("###scene_view"));
    CHECK(dock("###physics_debugger") == dock("###inspector"));
    check_rect("###profiler", 0, 900 * (1.0 - JCE_LAYOUT_BOTTOM_RATIO),
               1600, 900 * JCE_LAYOUT_BOTTOM_RATIO);
}

TEST_CASE("Two by two keeps separate Scene Game and bottom right Profiler")
{
    apply_layout_preset(1, 3);
    check_build();
    CHECK(ports.split == 3);
    check_rect("###scene_view", 0, 0, 800, 450);
    check_rect("###game_view", 800, 0, 800, 450);
    check_rect("###profiler", 800, 450, 800, 450);
    CHECK(dock("###profiler") == dock("###console"));
    CHECK(dock("###network_stats") == dock("###scene_view"));
}

TEST_CASE("Remaining presets retain existing shared extension routing")
{
    for (int preset : {1, 2, 4, 5, 6, 7, 9}) {
        CAPTURE(preset);
        apply_layout_preset(1, preset);
        check_build();
        CHECK(ports.split == 3);
        CHECK(dock("###profiler") == dock("###console"));
        CHECK(dock("###build_profiles") == dock("###console"));
        CHECK(dock("###game_view") == dock("###scene_view"));
        CHECK(dock("###network_stats") == dock("###scene_view"));
        CHECK(dock("###physics_debugger") == dock("###jce_terrain"));
    }
}

TEST_CASE("Unknown preset still consumes the existing Default dispatcher")
{
    apply_layout_preset(1, -1);
    check_build();
    CHECK(ports.split == 3);
    CHECK(dock("###profiler") == dock("###console"));
    CHECK(dock("###game_view") == dock("###scene_view"));
}

/* Extend the same actual-layout owner: complete Scene/Game dock and overlay
 * blocks plus the actual Scene content prefix. UI/timer/heavy-content ports
 * prove CPU admission/order/cleanup, not pixels, docking persistence or GPU work. */
#ifndef JCE_EDITOR_VIEWPORT_MAX_STATE
#error The layout fixture requires actual viewport ownership extracts
#endif
enum {
    ImGuiStyleVar_WindowPadding, ImGuiStyleVar_WindowRounding,
    ImGuiStyleVar_WindowBorderSize
};
enum {
    ImGuiWindowFlags_NoFocusOnAppearing = 1, ImGuiWindowFlags_NoTitleBar = 2,
    ImGuiWindowFlags_NoCollapse = 4, ImGuiWindowFlags_NoResize = 8,
    ImGuiWindowFlags_NoMove = 16, ImGuiWindowFlags_NoDocking = 32,
    ImGuiWindowFlags_NoSavedSettings = 64, ImGuiWindowFlags_NoScrollbar = 128,
    ImGuiWindowFlags_NoScrollWithMouse = 256, ImGuiWindowFlags_NoNavFocus = 512
};
using ImGuiWindowFlags = int;
namespace {
struct ViewportPorts {
    bool visible[JCE_PANEL_COUNT] = {};
    bool scene_open = true, game_open = true, overlay_open = true;
    bool console_open = true, console_detached = false;
    bool toggle_game = false, restore_game = false;
    unsigned scene_body = 0, game_body = 0, scene_flush = 0, toolbar = 0;
    unsigned gizmo_cancel = 0, transaction_cancel = 0, terrain_end = 0, foliage_end = 0, pilot_stop = 0;
    unsigned bridge_clear = 0, begins = 0, ends = 0;
    unsigned scene_dock = 0, game_dock = 0, overlays = 0;
    unsigned console_dock = 0, console_calls = 0, console_admitted = 0, console_covered = 0;
    uint32_t game_policy = UINT32_MAX;
    std::string current_window;
} viewport;
ImGuiViewport detached_console_viewport{{0,0},{800,600},2};
bool s_dbg_focus_game = false, s_dbg_focus_scene = false;
bool s_focus_game_view = false, s_focus_scene_view = false;
bool s_gizmo_transaction_open = false, s_gizmo_raw_dragging = false, s_is_selecting = false;
bool s_sel_pending = false, s_sel_click_pending = false;
bool s_gpu_pick_selection_pending = false, s_gpu_marquee_pending = false;
bool gizmo_active = false;
int gizmo_axis = 0;
constexpr int JCE_GIZMO_AXIS_NONE = 0;
bool jce_gizmo_is_active() { return gizmo_active; }
int jce_gizmo_hovered_axis() { return gizmo_axis; }
void jce_gizmo_cancel_interaction() { gizmo_active = false; gizmo_axis = 0; ++viewport.gizmo_cancel; }
void jce_state_cancel_transaction() { ++viewport.transaction_cancel; }
void flush_async_drop_material_extracts() { ++viewport.scene_flush; }
void draw_scene_view_toolbar() { ++viewport.toolbar; }
void panel_phase_sv(const char *, uint64_t *) {}
void panel_phase(const char *, uint64_t *) {}
void jce_terrain_panel_end_brush_stroke() { ++viewport.terrain_end; }
void jce_foliage_brush_end_stroke() { ++viewport.foliage_end; }
void jce_editor_scene_camera_stop_pilot() { ++viewport.pilot_stop; }
JceEditorGameInputBridge bridge;
}
JceEditorGameInputBridge *jce_editor_game_input_bridge_shared() { return &bridge; }
void jce_editor_game_input_bridge_publish(JceEditorGameInputBridge *value,
                                         const JceEditorGameInputViewport *packet)
{ CHECK(value == &bridge); CHECK(packet == nullptr); ++viewport.bridge_clear; }
extern "C" void jce_editor_game_render_observation_policy(uint32_t reason)
{ viewport.game_policy = reason; }
extern "C" bool *jce_editor_panel_visible_ptr(JceEditorPanel kind)
{ REQUIRE(kind >= 0); REQUIRE(kind < JCE_PANEL_COUNT); return &viewport.visible[kind]; }
extern "C" void jce_editor_panel_default_pose(const char *) {}
extern "C" const char *jce_editor_i18n(const char *key) { return key; }
extern "C" uint64_t JCE_CALL jce_time_perf_counter(void) { return 1; }
extern "C" double JCE_CALL jce_time_perf_to_ms(uint64_t, uint64_t) { return 0; }
extern "C" void jce_perf_phase_add(const char *, double) {}
namespace ImGui {
void PushStyleVar(int, ImVec2) {}
void PushStyleVar(int, float) {}
void PopStyleVar(int = 1) {}
void SetNextWindowFocus() {}
void SetNextWindowPos(ImVec2) {}
void SetNextWindowSize(ImVec2) {}
void SetNextWindowViewport(ImGuiID) {}
bool Begin(const char *label, bool *, ImGuiWindowFlags)
{
    CHECK(viewport.current_window.empty());
    ++viewport.begins;
    const std::string name = label;
    if (name.find("###scene_view") != std::string::npos) {
        viewport.current_window = "scene"; ++viewport.scene_dock; return viewport.scene_open;
    }
    if (name.find("###game_view") != std::string::npos) {
        viewport.current_window = "game"; ++viewport.game_dock; return viewport.game_open;
    }
    if (name.find("###console") != std::string::npos) {
        viewport.current_window = "console"; ++viewport.console_dock; return viewport.console_open;
    }
    CHECK(name == "###viewport_maximized");
    viewport.current_window = "overlay"; ++viewport.overlays; return viewport.overlay_open;
}
ImGuiViewport *GetWindowViewport()
{
    CHECK(viewport.current_window == "console");
    return viewport.console_detached ? &detached_console_viewport : &ports.viewport;
}
void End() { CHECK_FALSE(viewport.current_window.empty()); viewport.current_window.clear(); ++viewport.ends; }
}
#include JCE_EDITOR_VIEWPORT_MAX_STATE
#include JCE_EDITOR_VIEWPORT_SCENE_CANCEL
#include JCE_EDITOR_VIEWPORT_GIZMO_CLEAR
/* Exact entry prefix, through the existing toolbar and before SceneViewCtx.
 * Reaching this counted continuation means the real early return admitted it. */
#include JCE_EDITOR_VIEWPORT_SCENE_PREFIX
    ++viewport.scene_body;
}
extern "C" void jce_editor_panel_game_view_content(void)
{
    ++viewport.game_body;
    if (viewport.toggle_game) { viewport.toggle_game = false; jce_editor_panel_set_maximized(JCE_PANEL_GAME_VIEW, true); }
    if (viewport.restore_game) { viewport.restore_game = false; jce_editor_panel_set_maximized(JCE_PANEL_GAME_VIEW, false); }
}
extern "C" void jce_editor_panel_console_content_admitted(bool admitted)
{
    CHECK(viewport.current_window == "console");
    ++viewport.console_calls;
    if (admitted) ++viewport.console_admitted;
    else ++viewport.console_covered;
}
static void draw_panel_windows(JceEditorPanel frame_maximized)
{
    char lbl[256];
    uint64_t ed_pt = 0;
#include JCE_EDITOR_VIEWPORT_MAX_DOCK
#include JCE_EDITOR_VIEWPORT_CONSOLE_DOCK
}
static void production_viewport_host()
{
#include JCE_EDITOR_VIEWPORT_MAX_HOST
}
namespace {
void viewport_reset(JceEditorPanel maximized = JCE_PANEL_COUNT)
{
    viewport = {};
    viewport.visible[JCE_PANEL_SCENE_VIEW] = viewport.visible[JCE_PANEL_GAME_VIEW] = true;
    viewport.visible[JCE_PANEL_CONSOLE] = true; detached_console_viewport.ID = 2;
    s_dbg_focus_game = s_dbg_focus_scene = false;
    s_focus_game_view = s_focus_scene_view = false;
    s_maximized_panel = maximized;
    gizmo_active = true; gizmo_axis = 1;
    s_gizmo_transaction_open = s_gizmo_raw_dragging = s_is_selecting = true;
    s_sel_pending = s_sel_click_pending = s_gpu_pick_selection_pending = s_gpu_marquee_pending = true;
}
void balanced_docks(unsigned overlays)
{
    CHECK(viewport.scene_dock == 1); CHECK(viewport.game_dock == 1);
    CHECK(viewport.overlays == overlays);
    CHECK(viewport.begins == viewport.ends); CHECK(viewport.current_window.empty());
}
}
TEST_CASE("actual viewport layout preserves dock windows and admits only the maximized owner")
{
    for (JceEditorPanel owner : {JCE_PANEL_COUNT, JCE_PANEL_SCENE_VIEW, JCE_PANEL_GAME_VIEW}) {
        CAPTURE(owner); viewport_reset(owner); production_viewport_host();
        balanced_docks(owner == JCE_PANEL_COUNT ? 0 : 1);
        CHECK(viewport.scene_body == (owner == JCE_PANEL_GAME_VIEW ? 0 : 1));
        CHECK(viewport.game_body == (owner == JCE_PANEL_SCENE_VIEW ? 0 : 1));
        CHECK(viewport.bridge_clear == (owner == JCE_PANEL_SCENE_VIEW ? 1 : 0));
    }
}
TEST_CASE("covered Scene consumes actual cleanup prefix without toolbar or heavy content")
{
    viewport_reset(JCE_PANEL_GAME_VIEW); production_viewport_host();
    CHECK(viewport.scene_flush == 1); CHECK(viewport.toolbar == 0); CHECK(viewport.scene_body == 0);
    CHECK(viewport.gizmo_cancel == 1); CHECK(viewport.transaction_cancel == 1);
    CHECK(viewport.terrain_end == 1); CHECK(viewport.foliage_end == 1); CHECK(viewport.pilot_stop == 1);
    CHECK_FALSE(s_gizmo_transaction_open); CHECK_FALSE(s_gizmo_raw_dragging); CHECK_FALSE(s_is_selecting);
    CHECK_FALSE(s_sel_pending); CHECK_FALSE(s_sel_click_pending);
    CHECK_FALSE(s_gpu_pick_selection_pending); CHECK_FALSE(s_gpu_marquee_pending);
}
TEST_CASE("own Scene overlay keeps its live interaction instead of cancelling through the dock")
{
    viewport_reset(JCE_PANEL_SCENE_VIEW); production_viewport_host();
    CHECK(viewport.scene_body == 1); CHECK(viewport.scene_flush == 1);
    CHECK(viewport.gizmo_cancel == 0); CHECK(viewport.pilot_stop == 0);
    CHECK(s_gizmo_transaction_open); CHECK(s_is_selecting);
}
TEST_CASE("Game maximize and restore clicks cannot draw dock and overlay in the same frame")
{
    viewport_reset(); viewport.toggle_game = true; production_viewport_host();
    CHECK(s_maximized_panel == JCE_PANEL_GAME_VIEW); CHECK(viewport.game_body == 1); balanced_docks(0);
    CHECK(viewport.console_admitted == 1); CHECK(viewport.console_covered == 0);
    viewport_reset(s_maximized_panel); viewport.restore_game = true; production_viewport_host();
    CHECK(s_maximized_panel == JCE_PANEL_COUNT); CHECK(viewport.game_body == 1); balanced_docks(1);
    CHECK(viewport.console_admitted == 0); CHECK(viewport.console_covered == 1);
    viewport_reset(s_maximized_panel); production_viewport_host();
    CHECK(viewport.scene_body == 1); CHECK(viewport.game_body == 1); balanced_docks(0);
}
TEST_CASE("switching owners and collapsed overlays preserve input release and restoration")
{
    viewport_reset(JCE_PANEL_GAME_VIEW); viewport.overlay_open = false; production_viewport_host();
    CHECK(viewport.game_body == 0); CHECK(viewport.bridge_clear == 1); balanced_docks(1);
    viewport_reset(JCE_PANEL_SCENE_VIEW); production_viewport_host();
    CHECK(viewport.game_body == 0); CHECK(viewport.scene_body == 1); balanced_docks(1);
    viewport_reset(JCE_PANEL_GAME_VIEW); viewport.visible[JCE_PANEL_GAME_VIEW] = false;
    production_viewport_host(); CHECK(s_maximized_panel == JCE_PANEL_COUNT);
    CHECK(viewport.game_body == 0); CHECK(viewport.bridge_clear == 1);
    viewport_reset(JCE_PANEL_SCENE_VIEW); viewport.visible[JCE_PANEL_SCENE_VIEW] = false;
    production_viewport_host(); CHECK(s_maximized_panel == JCE_PANEL_COUNT);
    CHECK(viewport.scene_body == 0); CHECK(viewport.pilot_stop == 1);
}

TEST_CASE("direct Scene denial still services CPU results and skips content")
{
    viewport_reset();
    jce_editor_panel_scene_view_content(false);
    CHECK(viewport.scene_flush == 1); CHECK(viewport.scene_body == 0);
    CHECK(viewport.toolbar == 0); CHECK(viewport.pilot_stop == 1);
    CHECK(viewport.begins == 0); CHECK(viewport.ends == 0);
}

TEST_CASE("actual layout reports hidden collapsed and covered Game policy")
{
    viewport_reset(); viewport.visible[JCE_PANEL_GAME_VIEW] = false;
    production_viewport_host();
    CHECK(viewport.game_policy == static_cast<uint32_t>(JceEditorViewportPolicyReason::Hidden));
    viewport_reset(); viewport.game_open = false; production_viewport_host();
    CHECK(viewport.game_policy == static_cast<uint32_t>(JceEditorViewportPolicyReason::Collapsed));
    viewport_reset(JCE_PANEL_SCENE_VIEW); production_viewport_host();
    CHECK(viewport.game_policy == static_cast<uint32_t>(JceEditorViewportPolicyReason::Clipped));
    viewport_reset(JCE_PANEL_GAME_VIEW); viewport.overlay_open = false; production_viewport_host();
    CHECK(viewport.game_policy == static_cast<uint32_t>(JceEditorViewportPolicyReason::Collapsed));
    viewport_reset(); production_viewport_host(); CHECK(viewport.game_policy == UINT32_MAX);
}

TEST_CASE("actual Console caller keeps balanced dock admission and excludes only same-host covered presentation")
{
    for (JceEditorPanel owner:{JCE_PANEL_COUNT,JCE_PANEL_GAME_VIEW,JCE_PANEL_SCENE_VIEW})
        for (bool detached:{false,true}) {
            CAPTURE(owner,detached);
            viewport_reset(owner); viewport.console_detached=detached;
            production_viewport_host();
            balanced_docks(owner==JCE_PANEL_COUNT?0:1);
            CHECK(viewport.console_dock==1); CHECK(viewport.console_calls==1);
            const bool admitted=owner==JCE_PANEL_COUNT || detached;
            CHECK(viewport.console_admitted==(admitted?1u:0u));
            CHECK(viewport.console_covered==(admitted?0u:1u));
        }
    viewport_reset(JCE_PANEL_GAME_VIEW);
    viewport.console_detached=true; detached_console_viewport.ID=ports.viewport.ID;
    production_viewport_host();
    CHECK(viewport.console_covered==1); CHECK(viewport.console_admitted==0);
    viewport_reset(JCE_PANEL_GAME_VIEW); viewport.console_open=false;
    production_viewport_host(); balanced_docks(1);
    CHECK(viewport.console_dock==1); CHECK(viewport.console_calls==0);
    viewport_reset(JCE_PANEL_SCENE_VIEW); viewport.visible[JCE_PANEL_CONSOLE]=false;
    production_viewport_host(); balanced_docks(1);
    CHECK(viewport.console_dock==0); CHECK(viewport.console_calls==0);
}
