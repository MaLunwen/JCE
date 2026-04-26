/*
 * jce_panel_game_view.cpp  Game View panel (runtime viewport).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_i18n.h"
#include "jce_editor_config.h"
#include "jce_run_manager.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

/* Renderer backend list comes from jce_editor_panels.cpp via the
   editor's public accessor (populated lazily from
   jce_renderer_caps_list_backends()). */

/* ── Game View state ──────────────────────────────────────────────── */

static int  s_aspect_idx  = 0;
static bool s_show_stats  = false;
static int  s_run_mode_idx = 0;
static bool s_run_mode_loaded = false;

enum {
    JCE_GAME_VIEW_RUN_EDITOR_SIMULATION = 0,
    JCE_GAME_VIEW_RUN_EXTERNAL_GAME = 1,
};

static void ensure_run_mode_loaded(void)
{
    if (s_run_mode_loaded) return;

    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    s_run_mode_idx = cfg.run_mode == JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         ? JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         : JCE_GAME_VIEW_RUN_EDITOR_SIMULATION;
    s_run_mode_loaded = true;
}

static void persist_run_mode(void)
{
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    cfg.run_mode = s_run_mode_idx;
    jce_editor_config_save(&cfg);
}

static void start_external_game(void)
{
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);

    JceRunConfig run_cfg;
    memset(&run_cfg, 0, sizeof(run_cfg));
    snprintf(run_cfg.executable_path, sizeof(run_cfg.executable_path), "%s",
             cfg.game_executable_path);
    snprintf(run_cfg.working_directory, sizeof(run_cfg.working_directory), "%s",
             cfg.game_working_directory);
    run_cfg.capture_stdout = true;
    run_cfg.capture_stderr = true;

    jce_run_manager_start(&run_cfg);
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_game_view_content(void)
{
    ensure_run_mode_loaded();
    jce_run_manager_poll();

    /* Toolbar row */
    const char *aspects[] = { "Free", "16:9", "16:10", "4:3", "21:9", "1:1" };
    ImGui::PushItemWidth(80);
    ImGui::Combo("##aspect", &s_aspect_idx, aspects, 6);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    static int s_renderer_idx = 0;
    const char *const *renderer_names = nullptr;
    int renderer_count = jce_editor_renderer_backends(&renderer_names);
    ImGui::PushItemWidth(80);
    ImGui::Combo("##renderer", &s_renderer_idx, renderer_names, renderer_count);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###stats", jce_editor_i18n("game.stats"));
        ImGui::Checkbox(_lbl, &s_show_stats);
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    const char *run_modes[] = { "Editor Simulation", "External Game" };
    bool external_running = jce_run_manager_is_running();
    ImGui::PushItemWidth(150);
    if (external_running) ImGui::BeginDisabled();
    if (ImGui::Combo("##runMode", &s_run_mode_idx, run_modes, 2))
        persist_run_mode();
    if (external_running) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::SameLine();

    if (s_run_mode_idx == JCE_GAME_VIEW_RUN_EXTERNAL_GAME) {
        JceRunStatus rs;
        jce_run_manager_get_status(&rs);
        if (!rs.running) {
            if (ImGui::SmallButton(">")) start_external_game();
        } else {
            if (ImGui::SmallButton("[]")) jce_run_manager_request_stop();
            ImGui::SameLine();
            if (rs.state == JCE_RUN_STOPPING)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Stopping external game");
            else
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "External game running");
        }
    } else {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_STOPPED) {
            if (ImGui::SmallButton(">"))  jce_state_play();
        } else {
            if (ImGui::SmallButton("||")) jce_state_pause();
            ImGui::SameLine();
            if (ImGui::SmallButton("[]")) jce_state_stop();
            ImGui::SameLine();
            if (ps == JCE_PLAY_PLAYING)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), ">> Playing");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "|| Paused");
        }
    }

    ImGui::Separator();

    /* Viewport area */
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY() + 8));
    ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
        "%s  %.0f x %.0f", jce_editor_i18n("Game"), size.x, size.y);

    if (s_show_stats) {
        const char *const *names = nullptr;
        int n = jce_editor_renderer_backends(&names);
        const char *current = (names && s_renderer_idx >= 0 && s_renderer_idx < n)
                                  ? names[s_renderer_idx] : "?";
        ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY()));
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 0.8f),
            "%s: %s", jce_editor_i18n("game.renderer"), current);
        ImGui::SetCursorPos(ImVec2(8, ImGui::GetCursorPosY()));
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 0.8f),
            "%s: %.1f | %s: -- | %s: --",
            jce_editor_i18n("preferences.display.targetFps"),
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
    char title[256];
    snprintf(title, sizeof(title), "%s###GameView", jce_editor_i18n("Game"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_game_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
