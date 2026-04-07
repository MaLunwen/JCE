/*
 * jce_panel_game_view.cpp  Game View panel (runtime viewport).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>

/* ── Game View state ──────────────────────────────────────────────── */

static int  s_aspect_idx  = 0;
static bool s_show_stats  = false;

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_game_view_content(void)
{
    /* Toolbar row */
    const char *aspects[] = { "Free", "16:9", "16:10", "4:3", "21:9", "1:1" };
    ImGui::PushItemWidth(80);
    ImGui::Combo("##aspect", &s_aspect_idx, aspects, 6);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    static int s_renderer_idx = 0;
    const char *renderers[] = { "OpenGL", "Vulkan", "Auto" };
    ImGui::PushItemWidth(80);
    ImGui::Combo("##renderer", &s_renderer_idx, renderers, 3);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###stats", jce_editor_i18n("game.stats"));
        ImGui::Checkbox(_lbl, &s_show_stats);
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    JcePlayState ps = jce_state_get_play_state();
    if (ps == JCE_PLAY_STOPPED) {
        if (ImGui::SmallButton(">"))  jce_state_play();
    } else {
        if (ImGui::SmallButton("||")) jce_state_pause();
        ImGui::SameLine();
        if (ImGui::SmallButton("[]")) jce_state_stop();
    }

    ImGui::Separator();

    /* Viewport area */
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY() + 8));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
        "%s  %.0f x %.0f", jce_editor_i18n("Game"), size.x, size.y);

    if (s_show_stats) {
        ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY()));
        const char *renderer_names[] = { "OpenGL 3.3", "Vulkan 1.2", "Auto" };
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 0.8f),
            "%s: %s", jce_editor_i18n("game.renderer"), renderer_names[s_renderer_idx]);
        ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY()));
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 0.8f),
            "FPS: %.1f | %s: -- | %s: --",
            ImGui::GetIO().Framerate,
            jce_editor_i18n("game.drawCalls"),
            jce_editor_i18n("game.triangles"));
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_game_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Game###GameView", vis))
        jce_editor_panel_game_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
