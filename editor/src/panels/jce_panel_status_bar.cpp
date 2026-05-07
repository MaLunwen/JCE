/*
 * jce_panel_status_bar.cpp  Bottom-of-window status bar.
 *
 * Shows: FPS | current scene name | * dirty marker | selection count |
 *        play state | (right-aligned) build status placeholder.
 *
 * Anchored to the bottom of the main viewport via SetNextWindowPos/Size,
 * styled like a slim toolbar (no title bar, no resize).
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

static const char *play_state_label(JcePlayState ps)
{
    switch (ps) {
        case JCE_PLAY_PLAYING: return jce_editor_i18n("statusBar.play.playing");
        case JCE_PLAY_PAUSED:  return jce_editor_i18n("statusBar.play.paused");
        case JCE_PLAY_STOPPED:
        default:               return jce_editor_i18n("statusBar.play.stopped");
    }
}

static const char *short_scene_name(const char *path)
{
    if (!path || !*path) return jce_editor_i18n("statusBar.untitled");
    const char *slash = std::strrchr(path, '/');
    const char *bslash = std::strrchr(path, '\\');
    const char *name = slash > bslash ? slash : bslash;
    return name ? name + 1 : path;
}

void jce_editor_panel_status_bar(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_STATUS_BAR);
    if (!*vis) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                   vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    if (ImGui::Begin("##jce_status_bar", nullptr, flags)) {
        const float fps = ImGui::GetIO().Framerate;
        ImGui::Text("%s %5.1f", jce_editor_i18n("statusBar.fps"), fps);

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        const char *path = jce_state_get_current_scene_path();
        bool dirty = jce_state_is_scene_modified();
        ImGui::Text("%s %s%s", jce_editor_i18n("statusBar.scene"),
                    short_scene_name(path), dirty ? " *" : "");

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        int sel_count = 0;
        jce_state_get_selection(&sel_count);
        ImGui::Text("%s %d", jce_editor_i18n("statusBar.selected"), sel_count);

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        ImGui::Text("%s", play_state_label(jce_state_get_play_state()));

        /* Right side: build/status placeholder. Hooks in once a build
         * subsystem reports progress globally. */
        const char *right = jce_editor_i18n("statusBar.ready");
        float right_w = ImGui::CalcTextSize(right).x;
        float avail   = ImGui::GetContentRegionAvail().x;
        if (avail > right_w + 8) {
            ImGui::SameLine(0, avail - right_w - 8);
            ImGui::TextUnformatted(right);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
