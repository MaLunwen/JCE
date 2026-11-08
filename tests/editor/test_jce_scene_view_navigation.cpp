/* Actual production item registration/draw owner + real ImGui CPU context.
 * No native window, scene renderer, pixel/GPU completion or full UI acceptance. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include "panels/jce_scene_view_internal.h"
#include <string>

namespace {
const float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
struct NavigationFixture {
    JceTestImGuiContext gui;
    ImVec2 origin{16,24}, size{600,420};
    ImVec2 window_size{800,600};
    SceneViewCtx last{};
    bool show_axis = true;
    bool image_available = true, welcome_modal = false;
    int cube_action = -1, axis_action = -1;
    unsigned scene_press_count = 0, continuous_scene_count = 0;
    unsigned cube_action_count = 0, axis_action_count = 0;
    unsigned cancel_deferred_count = 0;
    unsigned imgui_errors = 0;
    std::string last_imgui_error;
    NavigationFixture()
    {
        gui.context->ErrorCallbackUserData = this;
        gui.context->ErrorCallback = [](ImGuiContext *ctx, void *user, const char *message) {
            auto *self = static_cast<NavigationFixture *>(user);
            ++self->imgui_errors; self->last_imgui_error = message;
            /* Only this failing test context converts an observed native
             * assertion into the strict zero-error assertion below. This lets
             * old-owner negatives fail ordinarily instead of aborting. */
            ctx->IO.ConfigErrorRecoveryEnableAssert = false;
        };
    }
    ~NavigationFixture()
    {
        gui.context->ErrorCallback = nullptr;
        gui.context->ErrorCallbackUserData = nullptr;
    }
    void frame(ImVec2 mouse, bool left, bool right = false)
    {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
        io.ConfigErrorRecoveryEnableAssert = true;
        gui.begin();
        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
        ImGui::Begin("Navigation input fixture", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        last = {}; last.avail = size; last.screen_pos = origin;
        last.dl = ImGui::GetWindowDrawList();
        /* Match actual setup_scene_viewport: Image (or unavailable-resource
         * label) precedes navigation registration. The texture token is only
         * retained in CPU draw data; this fixture starts no GPU/backend. */
        ImGui::SetCursorScreenPos(origin);
        if (image_available)
            ImGui::Image((ImTextureID)1, size);
        else
            ImGui::TextUnformatted("Scene resource unavailable");
        const ImVec2 after_image = ImGui::GetCursorScreenPos();
        register_scene_viewport_input(&last, show_axis);
        if (image_available) {
            CHECK(ImGui::GetCursorScreenPos().x == after_image.x);
            CHECK(ImGui::GetCursorScreenPos().y == after_image.y);
        }
        /* The production registry must leave the scene item last for DnD. */
        CHECK(ImGui::GetItemID() == ImGui::GetID("##SceneViewInput"));
        if (jce_scene_view_should_start_selection(last.viewport_left_clicked,
                io.KeyAlt, false, false)) ++scene_press_count;
        if (jce_scene_view_left_input_belongs_to_viewport(last.viewport_left_clicked,
                last.viewport_active, ImGui::IsMouseDown(ImGuiMouseButton_Left)))
            ++continuous_scene_count;
        if (jce_scene_view_should_cancel_deferred_pick(
                ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                last.viewport_left_clicked, last.viewport_active)) ++cancel_deferred_count;
        cube_action = draw_view_cube(last.dl, origin, size, view, last.cube_left_clicked);
        axis_action = show_axis
            ? draw_axis_indicator(last.dl, origin, size, view, last.axis_left_clicked) : -1;
        if (cube_action != -1) ++cube_action_count;
        if (axis_action >= 0) ++axis_action_count;
        ImGui::End(); ImGui::PopStyleVar();
        if (welcome_modal) {
            ImGui::SetNextWindowSize(ImVec2(380,180), ImGuiCond_Always);
            ImGui::OpenPopup("Welcome layout fixture");
            if (ImGui::BeginPopupModal("Welcome layout fixture", nullptr,
                    ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextUnformatted("Modal covers an otherwise complete Scene View frame");
                ImGui::EndPopup();
            }
        }
        gui.end(); ImGui::Render();
        INFO(last_imgui_error);
        CHECK(imgui_errors == 0);
        CHECK(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->Valid);
    }
    ImVec2 center() const { return ImVec2(origin.x+size.x/2, origin.y+size.y/2); }
    JceSceneViewNavigationLayout cube() const
    { return jce_scene_view_cube_layout(origin.x, origin.y, size.x, size.y); }
    JceSceneViewNavigationLayout axis() const
    { return jce_scene_view_axis_layout(origin.x, origin.y, size.x, size.y); }
    void warm(ImVec2 p) { frame(p,false); frame(p,false); }
};
}

TEST_CASE("cube face owns press hold outside and release without scene selection")
{
    NavigationFixture f; const auto cube=f.cube();
    const ImVec2 p(cube.center_x,cube.center_y); f.warm(p); f.frame(p,true);
    CHECK(f.last.cube_left_clicked); CHECK(f.cube_action==0);
    CHECK(f.last.navigation_active); CHECK_FALSE(f.last.viewport_left_clicked);
    CHECK_FALSE(f.last.viewport_active); CHECK(f.scene_press_count==0);
    CHECK(f.cancel_deferred_count==1); CHECK(f.cube_action_count==1);
    f.frame(f.center(),true); CHECK(f.last.navigation_active);
    CHECK_FALSE(f.last.viewport_active); CHECK(f.continuous_scene_count==0);
    f.frame(f.center(),false); CHECK_FALSE(f.last.navigation_active);
    CHECK(f.scene_press_count==0); CHECK(f.cube_action_count==1);
}
TEST_CASE("cube home and blank chrome consume without a scene press")
{
    NavigationFixture f; const auto c=f.cube();
    const ImVec2 home(c.bounds.x0+10,c.bounds.y0+10); f.warm(home); f.frame(home,true);
    CHECK(f.cube_action==-2); CHECK(f.last.cube_left_clicked); CHECK(f.scene_press_count==0);
    f.frame(home,false);
    const ImVec2 blank(c.bounds.x0+2,c.bounds.y0+2); f.warm(blank); f.frame(blank,true);
    CHECK(f.last.cube_left_clicked); CHECK(f.cube_action==-1); CHECK(f.scene_press_count==0);
}
TEST_CASE("visible axis owns its tip while hidden axis leaves the scene reachable")
{
    NavigationFixture f; const auto a=f.axis(); const ImVec2 tip(a.center_x+a.radius,a.center_y);
    f.warm(tip); f.frame(tip,true); CHECK(f.last.axis_left_clicked); CHECK(f.axis_action==0);
    CHECK(f.scene_press_count==0); f.frame(tip,false);
    f.show_axis=false; f.warm(tip); f.frame(tip,true);
    CHECK_FALSE(f.last.axis_left_clicked); CHECK(f.last.viewport_left_clicked);
    CHECK(f.scene_press_count==1);
}
TEST_CASE("ordinary scene press and existing scene drag retain ownership across cube chrome")
{
    NavigationFixture f; const auto c=f.cube(); const ImVec2 chrome(c.center_x,c.center_y);
    f.warm(f.center()); f.frame(f.center(),true);
    CHECK(f.last.viewport_left_clicked); CHECK(f.last.viewport_active); CHECK(f.scene_press_count==1);
    f.frame(chrome,true); CHECK(f.last.viewport_active); CHECK_FALSE(f.last.navigation_active);
    CHECK_FALSE(f.last.cube_left_clicked); CHECK(f.cube_action_count==0);
    f.frame(chrome,false); CHECK(f.scene_press_count==1); CHECK(f.cube_action_count==0);
}
TEST_CASE("chrome right click cannot open the scene context menu")
{
    NavigationFixture f; const auto c=f.cube(); const ImVec2 p(c.center_x,c.center_y);
    f.warm(p); f.frame(p,false,true);
    CHECK(f.last.navigation_active); CHECK_FALSE(f.last.viewport_right_clicked);
    CHECK_FALSE(jce_scene_view_should_open_context_menu(f.last.viewport_right_clicked,false));
    CHECK(f.scene_press_count==0); CHECK(f.cube_action_count==0);
    f.frame(p,false,false);
    f.warm(f.center()); f.frame(f.center(),false,true);
    CHECK(f.last.viewport_right_clicked);
    CHECK(jce_scene_view_should_open_context_menu(f.last.viewport_right_clicked,false));
}

TEST_CASE("scene image spacing fallback and welcome modal finish End and Render without layout errors")
{
    NavigationFixture f;
    ImGui::GetIO().DisplaySize = ImVec2(1707,1024);
    f.window_size = ImVec2(1707,1024); f.origin = ImVec2(0,0); f.size = f.window_size;
    CHECK(ImGui::GetStyle().ItemSpacing.y > 0);
    f.warm(f.center());
    CHECK(f.imgui_errors == 0);
    f.image_available = false;
    f.warm(f.center());
    CHECK(f.imgui_errors == 0);
    f.image_available = true; f.welcome_modal = true;
    f.warm(f.center());
    CHECK(f.imgui_errors == 0);
}
