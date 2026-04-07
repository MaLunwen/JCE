/*
 * jce_panel_scene_view.cpp  Scene View panel (3D viewport + gizmo toolbar).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_scene_view_content(void)
{
    /* Toolbar row (gizmo mode / space / view mode / grid / camera) */
    JceGizmoMode gm = jce_state_get_gizmo_mode();
    if (ImGui::RadioButton("T", gm == JCE_GIZMO_TRANSLATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("R", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("S", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    JceGizmoSpace gs = jce_state_get_gizmo_space();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###local", jce_editor_i18n("toolbar.local"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_LOCAL))
            jce_state_set_gizmo_space(JCE_GIZMO_LOCAL);
    }
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###world", jce_editor_i18n("toolbar.global"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_WORLD))
            jce_state_set_gizmo_space(JCE_GIZMO_WORLD);
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    JceSceneViewMode vm = jce_state_get_view_mode();
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Shaded",    NULL, vm == JCE_VIEW_SHADED))
            jce_state_set_view_mode(JCE_VIEW_SHADED);
        if (ImGui::MenuItem("Wireframe", NULL, vm == JCE_VIEW_WIREFRAME))
            jce_state_set_view_mode(JCE_VIEW_WIREFRAME);
        if (ImGui::MenuItem("Textured",  NULL, vm == JCE_VIEW_TEXTURED))
            jce_state_set_view_mode(JCE_VIEW_TEXTURED);
        ImGui::EndMenu();
    }

    ImGui::SameLine();

    bool grid = jce_state_get_show_grid();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###grid", jce_editor_i18n("scene.grid"));
        if (ImGui::Checkbox(_lbl, &grid))
            jce_state_set_show_grid(grid);
    }

    ImGui::SameLine();

    if (ImGui::BeginMenu("Camera")) {
        ImGui::MenuItem("Reset");
        ImGui::MenuItem("Top");
        ImGui::MenuItem("Front");
        ImGui::MenuItem("Side");
        ImGui::EndMenu();
    }

    ImGui::Separator();

    /* Viewport area */
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY() + 8));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
        "Scene View  %.0f x %.0f", size.x, size.y);

    const char *mode_names[] = {
        jce_editor_i18n("toolbar.translate"),
        jce_editor_i18n("toolbar.rotate"),
        jce_editor_i18n("toolbar.scale")
    };
    const char *space_names[] = {
        jce_editor_i18n("toolbar.local"),
        jce_editor_i18n("toolbar.global")
    };
    ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY()));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.4f), "Gizmo: %s | Space: %s",
                        mode_names[jce_state_get_gizmo_mode()],
                        space_names[jce_state_get_gizmo_space()]);

    /* Keyboard shortcuts for gizmo modes */
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
        if (ImGui::IsKeyPressed(ImGuiKey_E)) jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
        if (ImGui::IsKeyPressed(ImGuiKey_R)) jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_scene_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Scene###SceneView", vis))
        jce_editor_panel_scene_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
