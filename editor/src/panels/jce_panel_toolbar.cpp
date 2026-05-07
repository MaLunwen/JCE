/*
 * jce_panel_toolbar.cpp  Top-level editor Toolbar (Unity-style).
 *
 * Hosts:
 *   - Transform tools (Q/W/E/R)              -> jce_state_set_gizmo_mode
 *   - Pivot / Center toggle (Z)              -> jce_state_set_gizmo_pivot
 *   - Local / Global toggle (X)              -> jce_state_set_gizmo_space
 *   - Play / Pause / Step                    -> jce_state_play/pause/stop/step
 *
 * Hotkeys are processed globally here (no focus requirement) so they work
 * from any panel.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_hotkeys.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>

/* ── Hotkey processing (global, focus-agnostic) ────────────────────── */

static void process_toolbar_hotkeys(void)
{
    /* Skip when an ImGui text widget owns the keyboard, otherwise typing
     * "w" into a text field would switch the gizmo. */
    if (ImGui::GetIO().WantTextInput) return;

    if (jce_hotkey_pressed(JCE_HK_GIZMO_NONE))      jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_TRANSLATE)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_ROTATE))    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_SCALE))     jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

    if (jce_hotkey_pressed(JCE_HK_GIZMO_TOGGLE_SPACE)) {
        JceGizmoSpace gs = jce_state_get_gizmo_space();
        jce_state_set_gizmo_space(gs == JCE_GIZMO_LOCAL ? JCE_GIZMO_WORLD : JCE_GIZMO_LOCAL);
    }
    if (jce_hotkey_pressed(JCE_HK_GIZMO_TOGGLE_PIVOT)) {
        JceGizmoPivot gp = jce_state_get_gizmo_pivot();
        jce_state_set_gizmo_pivot(gp == JCE_GIZMO_PIVOT ? JCE_GIZMO_CENTER : JCE_GIZMO_PIVOT);
    }

    if (jce_hotkey_pressed(JCE_HK_PLAY_TOGGLE)) {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_STOPPED) jce_state_play();
        else                        jce_state_stop();
    }
    if (jce_hotkey_pressed(JCE_HK_PLAY_PAUSE)) {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_PLAYING) jce_state_pause();
        else if (ps == JCE_PLAY_PAUSED) jce_state_play();
    }
    if (jce_hotkey_pressed(JCE_HK_PLAY_STEP)) {
        jce_state_step(1.0f / 60.0f);
    }
}

/* ── Content drawing (no Begin/End) ───────────────────────────────── */

static void draw_transform_tools(void)
{
    JceGizmoMode gm = jce_state_get_gizmo_mode();

    /* No-tool indicator (Q): we re-use Translate as the default "Move".
     * A dedicated NONE state is reserved for a future hand/select tool. */
    if (ImGui::RadioButton("Q##tool_hand", false))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.hand"));
    ImGui::SameLine();
    if (ImGui::RadioButton("W##tool_move", gm == JCE_GIZMO_TRANSLATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.move"));
    ImGui::SameLine();
    if (ImGui::RadioButton("E##tool_rot", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.rotate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("R##tool_scl", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.scale"));
}

static void draw_pivot_space(void)
{
    JceGizmoPivot gp = jce_state_get_gizmo_pivot();
    if (ImGui::SmallButton(jce_editor_i18n(gp == JCE_GIZMO_PIVOT ? "toolbar.pivot" : "toolbar.center"))) {
        jce_state_set_gizmo_pivot(gp == JCE_GIZMO_PIVOT ? JCE_GIZMO_CENTER : JCE_GIZMO_PIVOT);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.pivotToggle"));

    ImGui::SameLine();
    JceGizmoSpace gs = jce_state_get_gizmo_space();
    if (ImGui::SmallButton(jce_editor_i18n(gs == JCE_GIZMO_LOCAL ? "toolbar.local" : "toolbar.global"))) {
        jce_state_set_gizmo_space(gs == JCE_GIZMO_LOCAL ? JCE_GIZMO_WORLD : JCE_GIZMO_LOCAL);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.spaceToggle"));
}

static void draw_play_controls(void)
{
    JcePlayState ps = jce_state_get_play_state();

    ImVec4 stopped_btn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button,
        ps == JCE_PLAY_PLAYING ? ImVec4(0.20f, 0.60f, 0.20f, 1.0f) : stopped_btn);
    if (ImGui::Button(ps == JCE_PLAY_STOPPED ? " > " : " || ")) {
        if (ps == JCE_PLAY_STOPPED)      jce_state_play();
        else if (ps == JCE_PLAY_PLAYING) jce_state_pause();
        else                             jce_state_play(); /* resume from pause */
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.playPause"));

    ImGui::SameLine();
    bool can_stop = (ps != JCE_PLAY_STOPPED);
    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button(" [] ")) jce_state_stop();
    if (!can_stop) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.stop"));

    ImGui::SameLine();
    bool can_step = (ps == JCE_PLAY_PAUSED);
    if (!can_step) ImGui::BeginDisabled();
    if (ImGui::Button(" >| ")) jce_state_step(1.0f / 60.0f);
    if (!can_step) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("toolbar.tip.step"));
}

static void draw_toolbar_content(void)
{
    process_toolbar_hotkeys();

    draw_transform_tools();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    draw_pivot_space();

    /* Center play controls. */
    float play_w = ImGui::CalcTextSize(" > ").x * 3 +
                   ImGui::GetStyle().ItemSpacing.x * 2 +
                   ImGui::GetStyle().FramePadding.x * 6;
    float win_w  = ImGui::GetContentRegionAvail().x;
    if (win_w > play_w) {
        ImGui::SameLine(0, (win_w - play_w) * 0.5f);
    } else {
        ImGui::SameLine();
    }

    draw_play_controls();
}

/* ── Standalone window wrapper ────────────────────────────────────── */

void jce_editor_panel_toolbar(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_TOOLBAR);
    if (!*vis) return;

    /* Pin the toolbar to the very top of the main viewport (Unity-style)
       so layout presets never spawn it as a floating window. It stays
       above the menu bar's WorkPos area; the host DockSpace below shrinks
       automatically because we draw before the host window. */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float bar_h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##jce_top_toolbar", nullptr, flags)) {
        draw_toolbar_content();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
