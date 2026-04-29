/*
 * jce_dialog_project_settings.cpp  Centralized Project Settings dialog.
 *
 * Folder-tree on the left (Project / Build / Run / Render / Editor),
 * page content on the right.  Reads/writes JceEditorConfig through the
 * standard editor_config_load/save round-trip; Apply button persists.
 *
 * Distinct from the (per-user) Settings dialog (jce_editor_settings_dialog)
 * which only edits language/theme/fonts/renderer.  Project Settings is for
 * project-scoped configuration that travels with the project: build
 * presets, executable paths, etc.
 */

#include "jce_editor_dialogs.h"
#include "jce_editor_dialogs_internal.h"
#include "ui/jce_editor_style.h"
#include "core/jce_hotkeys.h"

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace {

enum Category {
    CAT_PROJECT = 0,
    CAT_BUILD,
    CAT_RUN,
    CAT_RENDER,
    CAT_HOTKEYS,
    CAT_COUNT
};

const char *category_name(int i)
{
    switch (i) {
        case CAT_PROJECT: return jce_editor_i18n("projectSettings.cat.project");
        case CAT_BUILD:   return jce_editor_i18n("projectSettings.cat.build");
        case CAT_RUN:     return jce_editor_i18n("projectSettings.cat.run");
        case CAT_RENDER:  return jce_editor_i18n("projectSettings.cat.render");
        case CAT_HOTKEYS: return jce_editor_i18n("projectSettings.cat.hotkeys");
        default:          return "?";
    }
}

struct PSState {
    bool             initialized   = false;
    bool             snapshot_done = false;
    int              category      = CAT_PROJECT;
    JceEditorConfig  cfg;
    JceEditorConfig  cfg_orig;
    bool             dirty         = false;
};

PSState s_ps;

void ensure_init()
{
    if (s_ps.initialized) return;
    s_ps = PSState{};
    s_ps.initialized = true;
}

void take_snapshot()
{
    if (!jce_editor_config_load(&s_ps.cfg))
        jce_editor_config_defaults(&s_ps.cfg);
    s_ps.cfg_orig = s_ps.cfg;
    s_ps.dirty    = false;
}

void apply_changes()
{
    jce_editor_config_save(&s_ps.cfg);
    jce_hotkeys_save();

    /* Live-apply font + DPI scale (no restart needed). */
    float fs = (s_ps.cfg.font_size >= 12 && s_ps.cfg.font_size <= 48)
                   ? (float)s_ps.cfg.font_size : 14.0f;
    jce_editor_request_font_reload(fs,
                                   s_ps.cfg.font_en_path, s_ps.cfg.font_zh_path);
    float scale = s_ps.cfg.ui_scale;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 3.0f) scale = 3.0f;
    ImGui::GetIO().FontGlobalScale = scale;

    s_ps.cfg_orig = s_ps.cfg;
    s_ps.dirty    = false;
    jce_editor_console_log("%s", jce_editor_i18n("projectSettings.savedLog"));
}

void revert_changes()
{
    s_ps.cfg   = s_ps.cfg_orig;
    s_ps.dirty = false;
}

bool input_text_field(const char *label, char *buf, size_t bufsz)
{
    ImGui::PushItemWidth(-1);
    bool changed = ImGui::InputText(label, buf, bufsz);
    ImGui::PopItemWidth();
    return changed;
}

void draw_project_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", jce_editor_i18n("projectSettings.cat.project"));
    ImGui::Separator();

    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.project.lastProject"));
    if (input_text_field("##last_project", s_ps.cfg.last_project,
                          sizeof(s_ps.cfg.last_project)))
        s_ps.dirty = true;

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        jce_editor_i18n("projectSettings.project.recentFmt"), s_ps.cfg.recent_count);
    if (ImGui::BeginChild("##recent_list", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < s_ps.cfg.recent_count; i++)
            ImGui::TextUnformatted(s_ps.cfg.recent_projects[i]);
    }
    ImGui::EndChild();

    if (ImGui::Button(jce_editor_i18n("projectSettings.project.clearRecent"))) {
        s_ps.cfg.recent_count = 0;
        s_ps.dirty = true;
    }
}

void draw_build_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", jce_editor_i18n("projectSettings.cat.build"));
    ImGui::Separator();

    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.cfgPreset"));
    if (input_text_field("##cfg_preset", s_ps.cfg.build_configure_preset,
                          sizeof(s_ps.cfg.build_configure_preset)))
        s_ps.dirty = true;

    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.buildPreset"));
    if (input_text_field("##build_preset", s_ps.cfg.build_preset,
                          sizeof(s_ps.cfg.build_preset)))
        s_ps.dirty = true;

    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.outputDir"));
    if (input_text_field("##out_dir", s_ps.cfg.build_output_path,
                          sizeof(s_ps.cfg.build_output_path)))
        s_ps.dirty = true;

    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.cmakeTarget"));
    if (input_text_field("##target", s_ps.cfg.game_target_name,
                          sizeof(s_ps.cfg.game_target_name)))
        s_ps.dirty = true;
}

void draw_run_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", jce_editor_i18n("projectSettings.cat.run"));
    ImGui::Separator();

    const char *mode_keys[] = {
        "projectSettings.run.mode.editorSim",
        "projectSettings.run.mode.subprocess",
    };
    const char *modes[2];
    modes[0] = jce_editor_i18n_or(mode_keys[0], "Editor Simulation");
    modes[1] = jce_editor_i18n_or(mode_keys[1], "External Game (subprocess)");
    int mode = s_ps.cfg.run_mode;
    if (mode < 0 || mode > 1) mode = 0;
    if (ImGui::Combo(jce_editor_i18n_id("projectSettings.run.mode", "run_mode"), &mode, modes, 2)) {
        s_ps.cfg.run_mode = mode;
        s_ps.dirty = true;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.run.gameExe"));
    if (input_text_field("##exe_path", s_ps.cfg.game_executable_path,
                          sizeof(s_ps.cfg.game_executable_path)))
        s_ps.dirty = true;

    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.run.workDir"));
    if (input_text_field("##work_dir", s_ps.cfg.game_working_directory,
                          sizeof(s_ps.cfg.game_working_directory)))
        s_ps.dirty = true;
}

void draw_render_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", jce_editor_i18n("projectSettings.cat.render"));
    ImGui::Separator();

    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.render.backend"));
    if (input_text_field("##renderer", s_ps.cfg.renderer,
                          sizeof(s_ps.cfg.renderer)))
        s_ps.dirty = true;
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                        jce_editor_i18n("projectSettings.render.backendHint"));

    ImGui::Spacing();
    const char *view_modes[] = {
        "Shaded", "Wireframe", "Wireframe Textured", "Textured",
        "Normals", "UV", "Overdraw", "LightingOnly"
    };
    int vm = s_ps.cfg.view_mode;
    if (vm < 0 || vm >= IM_ARRAYSIZE(view_modes)) vm = 0;
    if (ImGui::Combo(jce_editor_i18n("projectSettings.render.viewMode"), &vm, view_modes, IM_ARRAYSIZE(view_modes))) {
        s_ps.cfg.view_mode = vm;
        s_ps.dirty = true;
    }

    if (ImGui::Checkbox(jce_editor_i18n("projectSettings.render.showGrid"), &s_ps.cfg.show_grid))
        s_ps.dirty = true;
}

/* ── Hotkeys page ──────────────────────────────────────────────────── */
struct {
    int  recording_id = -1;     /* JceHotkeyId being recorded; -1 = none */
    char filter[64]   = {0};
} s_hk;

void draw_hotkeys_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", jce_editor_i18n("projectSettings.cat.hotkeys"));
    ImGui::Separator();

    ImGui::TextWrapped("%s", jce_editor_i18n("projectSettings.hotkeys.intro"));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##hkfilter", jce_editor_i18n("projectSettings.hotkeys.search"),
                             s_hk.filter, sizeof(s_hk.filter));

    ImGui::Spacing();
    if (ImGui::BeginTable("##hktab", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 360))) {
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.hotkeys.colAction"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.hotkeys.colChord"),  ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < JCE_HK_COUNT; ++i) {
            const char *name = jce_hotkey_name((JceHotkeyId)i);
            if (s_hk.filter[0]) {
                /* case-insensitive substring */
                char lname[128]; char lflt[64];
                int n = (int)strlen(name);
                for (int j = 0; j < n && j < 127; ++j)
                    lname[j] = (char)tolower((unsigned char)name[j]);
                lname[n < 127 ? n : 127] = '\0';
                int m = (int)strlen(s_hk.filter);
                for (int j = 0; j < m && j < 63; ++j)
                    lflt[j] = (char)tolower((unsigned char)s_hk.filter[j]);
                lflt[m < 63 ? m : 63] = '\0';
                if (!strstr(lname, lflt)) continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);

            ImGui::TableSetColumnIndex(1);
            char chord_buf[64];
            jce_hotkey_chord_label(jce_hotkey_get((JceHotkeyId)i),
                                    chord_buf, sizeof(chord_buf));
            bool is_recording = (s_hk.recording_id == i);
            if (is_recording) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                                    jce_editor_i18n("projectSettings.hotkeys.recording"));
                /* Capture next pressed non-modifier key. */
                ImGuiIO &io = ImGui::GetIO();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    s_hk.recording_id = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                    JceHotkeyChord c = {0, 0};
                    jce_hotkey_set((JceHotkeyId)i, c);
                    s_hk.recording_id = -1;
                    s_ps.dirty = true;
                } else {
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                        if (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper) continue;
                        if (k == ImGuiKey_ReservedForModCtrl ||
                            k == ImGuiKey_ReservedForModShift ||
                            k == ImGuiKey_ReservedForModAlt ||
                            k == ImGuiKey_ReservedForModSuper) continue;
                        if (ImGui::IsKeyPressed((ImGuiKey)k)) {
                            JceHotkeyChord c = {k, 0};
                            if (io.KeyCtrl)  c.mods |= JCE_HKM_CTRL;
                            if (io.KeyShift) c.mods |= JCE_HKM_SHIFT;
                            if (io.KeyAlt)   c.mods |= JCE_HKM_ALT;
                            if (io.KeySuper) c.mods |= JCE_HKM_SUPER;
                            jce_hotkey_set((JceHotkeyId)i, c);
                            s_hk.recording_id = -1;
                            s_ps.dirty = true;
                            break;
                        }
                    }
                }
            } else {
                if (ImGui::Selectable(chord_buf, false,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    s_hk.recording_id = i;
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(jce_editor_i18n("projectSettings.hotkeys.reset"))) {
                jce_hotkey_set((JceHotkeyId)i,
                                jce_hotkey_get_default((JceHotkeyId)i));
                s_ps.dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("projectSettings.hotkeys.resetAll"))) {
        jce_hotkeys_reset_all();
        s_ps.dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("projectSettings.hotkeys.persistsTo"));
}

} // namespace

extern "C" void jce_editor_dialog_project_settings(bool *p_open)
{
    if (!p_open || !*p_open) {
        s_ps.snapshot_done = false;
        return;
    }
    ensure_init();

    if (!s_ps.snapshot_done) {
        take_snapshot();
        s_ps.snapshot_done = true;
    }

    const char *popup_id = "###ProjectSettingsDialog";
    if (!ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char title[128];
    snprintf(title, sizeof(title), "%s%s", jce_editor_i18n("projectSettings.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(title, p_open,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        return;
    }

    /* Top dirty indicator */
    if (s_ps.dirty)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                            jce_editor_i18n("projectSettings.unsaved"));
    else
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), " ");

    /* Two-column layout: category tree | page content */
    ImGui::BeginChild("##cat", ImVec2(170, -ImGui::GetFrameHeightWithSpacing() - 8),
                       ImGuiChildFlags_Borders);
    for (int i = 0; i < CAT_COUNT; i++) {
        bool sel = (s_ps.category == i);
        if (ImGui::Selectable(category_name(i), sel))
            s_ps.category = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 8),
                       ImGuiChildFlags_Borders);
    switch (s_ps.category) {
        case CAT_PROJECT: draw_project_page(); break;
        case CAT_BUILD:   draw_build_page();   break;
        case CAT_RUN:     draw_run_page();     break;
        case CAT_RENDER:  draw_render_page();  break;
        case CAT_HOTKEYS: draw_hotkeys_page(); break;
    }
    ImGui::EndChild();

    /* Bottom buttons */
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("projectSettings.button.apply"), ImVec2(100, 0))) {
        if (s_ps.dirty) apply_changes();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("projectSettings.button.revert"), ImVec2(100, 0))) {
        revert_changes();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("projectSettings.button.ok"), ImVec2(100, 0))) {
        if (s_ps.dirty) apply_changes();
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("projectSettings.button.cancel"), ImVec2(100, 0))) {
        revert_changes();
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
