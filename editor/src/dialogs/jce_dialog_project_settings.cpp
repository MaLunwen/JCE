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
#include "core/jce_project_settings.h"

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
    /* ── Unity-aligned project pages ─────────────────────────────── */
    CAT_AUDIO,
    CAT_EDITOR_PREFS,
    CAT_GRAPHICS,
    CAT_INPUT,
    CAT_PHYSICS,
    CAT_PHYSICS2D,
    CAT_PLAYER,
    CAT_PRESET_MANAGER,
    CAT_QUALITY,
    CAT_TAGS_LAYERS,
    CAT_TIME,
    CAT_COUNT
};

const char *category_name(int i)
{
    switch (i) {
        case CAT_PROJECT:        return jce_editor_i18n("projectSettings.cat.project");
        case CAT_BUILD:          return jce_editor_i18n("projectSettings.cat.build");
        case CAT_RUN:            return jce_editor_i18n("projectSettings.cat.run");
        case CAT_RENDER:         return jce_editor_i18n("projectSettings.cat.render");
        case CAT_HOTKEYS:        return jce_editor_i18n("projectSettings.cat.hotkeys");
        case CAT_AUDIO:          return jce_editor_i18n_or("projectSettings.cat.audio",         "Audio");
        case CAT_EDITOR_PREFS:   return jce_editor_i18n_or("projectSettings.cat.editor",        "Editor");
        case CAT_GRAPHICS:       return jce_editor_i18n_or("projectSettings.cat.graphics",      "Graphics");
        case CAT_INPUT:          return jce_editor_i18n_or("projectSettings.cat.input",         "Input");
        case CAT_PHYSICS:        return jce_editor_i18n_or("projectSettings.cat.physics",       "Physics");
        case CAT_PHYSICS2D:      return jce_editor_i18n_or("projectSettings.cat.physics2d",     "Physics 2D");
        case CAT_PLAYER:         return jce_editor_i18n_or("projectSettings.cat.player",        "Player");
        case CAT_PRESET_MANAGER: return jce_editor_i18n_or("projectSettings.cat.presetManager", "Preset Manager");
        case CAT_QUALITY:        return jce_editor_i18n_or("projectSettings.cat.quality",       "Quality");
        case CAT_TAGS_LAYERS:    return jce_editor_i18n_or("projectSettings.cat.tagsLayers",    "Tags & Layers");
        case CAT_TIME:           return jce_editor_i18n_or("projectSettings.cat.time",          "Time");
        default:                 return "?";
    }
}

struct PSState {
    bool                initialized   = false;
    bool                snapshot_done = false;
    int                 category      = CAT_PROJECT;
    JceEditorConfig     cfg;
    JceEditorConfig     cfg_orig;
    JceProjectSettings  ps;
    JceProjectSettings  ps_orig;
    bool                dirty         = false;
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
    jce_project_settings_load(&s_ps.ps);
    s_ps.ps_orig  = s_ps.ps;
    s_ps.dirty    = false;
}

void apply_changes()
{
    jce_editor_config_save(&s_ps.cfg);
    jce_hotkeys_save();
    jce_project_settings_save(&s_ps.ps);
    jce_project_settings_apply(&s_ps.ps);

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
    s_ps.ps_orig  = s_ps.ps;
    s_ps.dirty    = false;
    jce_editor_console_log("%s", jce_editor_i18n("projectSettings.savedLog"));
}

void revert_changes()
{
    s_ps.cfg   = s_ps.cfg_orig;
    s_ps.ps    = s_ps.ps_orig;
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

/* ── New Unity-aligned project pages ──────────────────────────────── */
namespace {

void page_header(const char *title)
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", title);
    ImGui::Separator();
}

#define DIRTY_IF(expr) do { if (expr) s_ps.dirty = true; } while (0)

void draw_audio_page()
{
    page_header(category_name(CAT_AUDIO));
    JceProjectAudio &a = s_ps.ps.audio;
    DIRTY_IF(ImGui::SliderFloat(jce_editor_i18n("projectSettings.audio.masterVolume"),  &a.master_volume,  0.0f, 1.0f));
    DIRTY_IF(ImGui::SliderFloat(jce_editor_i18n("projectSettings.audio.dopplerFactor"), &a.doppler_factor, 0.0f, 5.0f));
    static const char *rates[] = { "22050", "44100", "48000", "96000" };
    int rate_idx = (a.sample_rate == 22050) ? 0 :
                   (a.sample_rate == 44100) ? 1 :
                   (a.sample_rate == 96000) ? 3 : 2;
    if (ImGui::Combo(jce_editor_i18n("projectSettings.audio.sampleRate"), &rate_idx, rates, IM_ARRAYSIZE(rates))) {
        int v = atoi(rates[rate_idx]); a.sample_rate = v; s_ps.dirty = true;
    }
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.audio.pauseOnFocusLoss"), &a.pause_on_focus_loss));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.audio.disableAudio"),       &a.disable_audio));
    ImGui::TextDisabled(jce_editor_i18n("projectSettings.audio.liveApplyHint"));
}

void draw_editor_prefs_page()
{
    page_header(category_name(CAT_EDITOR_PREFS));
    JceProjectEditor &e = s_ps.ps.editor;
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.editorPrefs.autoSaveEnabled"), &e.auto_save_enabled));
    DIRTY_IF(ImGui::SliderInt(jce_editor_i18n("projectSettings.editorPrefs.autoSaveInterval"), &e.auto_save_interval_sec, 30, 1800));
    static const char *modes[] = { "3D", "2D" };
    DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.editorPrefs.defaultBehaviorMode"), &e.default_behavior_mode, modes, 2));
    static const char *vc[] = { "Hidden Meta Files", "Visible Meta Files" };
    DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.editorPrefs.versionControlMode"), &e.version_control_mode, vc, 2));
    ImGui::Spacing();
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.editorPrefs.externalScriptEditor"),
                              e.external_script_editor, JCE_PS_PATH_LEN));
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.editorPrefs.externalImageEditor"),
                              e.external_image_editor,  JCE_PS_PATH_LEN));
}

void draw_graphics_page()
{
    page_header(category_name(CAT_GRAPHICS));
    JceProjectGraphics &g = s_ps.ps.graphics;
    static const char *cs[] = { "Gamma", "Linear" };
    DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.graphics.colorSpace"), &g.color_space, cs, 2));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.graphics.hdr"),        &g.hdr));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.graphics.srgbWrite"), &g.srgb_write));
    static const char *msaa[] = { "Off (1x)", "2x", "4x", "8x" };
    int msaa_idx = (g.default_msaa <= 1) ? 0 :
                   (g.default_msaa == 2) ? 1 :
                   (g.default_msaa == 4) ? 2 : 3;
    if (ImGui::Combo(jce_editor_i18n("projectSettings.graphics.defaultMsaa"), &msaa_idx, msaa, 4)) {
        static const int vals[] = { 0, 2, 4, 8 };
        g.default_msaa = vals[msaa_idx];
        s_ps.dirty = true;
    }
    static const char *aniso[] = { "Disabled", "Per Texture", "Forced On" };
    DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.graphics.anisotropicTextures"), &g.anisotropic_textures, aniso, 3));
    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.graphics.alwaysIncludedShaders"));
    DIRTY_IF(ImGui::InputTextMultiline("##aishaders", g.always_included_shaders,
                                       sizeof(g.always_included_shaders),
                                       ImVec2(-1, 120)));
}

void draw_input_page()
{
    page_header(category_name(CAT_INPUT));
    ImGui::TextWrapped("%s", jce_editor_i18n("projectSettings.input.hint"));
    ImGui::Spacing();
    JceProjectInput &i = s_ps.ps.input;
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.input.treatKeyboardAsDpad"), &i.treat_keyboard_as_dpad));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.input.enableGamepad"),          &i.enable_gamepad));
    DIRTY_IF(ImGui::SliderFloat(jce_editor_i18n("projectSettings.input.deadZone"),   &i.dead_zone,   0.0f, 0.9f));
    DIRTY_IF(ImGui::SliderFloat(jce_editor_i18n("projectSettings.common.gravity"),     &i.gravity,     0.1f, 10.0f));
    DIRTY_IF(ImGui::SliderFloat(jce_editor_i18n("projectSettings.input.sensitivity"), &i.sensitivity, 0.1f, 10.0f));
}

void draw_layer_collision_matrix(uint32_t mat[JCE_PS_LAYER_COUNT],
                                 const char *(*get_layer_name)(int))
{
    if (ImGui::TreeNode(jce_editor_i18n("projectSettings.physics.layerCollisionMatrix"))) {
        if (ImGui::BeginTable("##lcm", JCE_PS_LAYER_COUNT + 1,
                ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_ScrollX, ImVec2(0, 360))) {
            ImGui::TableSetupColumn("");
            for (int j = 0; j < JCE_PS_LAYER_COUNT; j++) {
                char hdr[32]; snprintf(hdr, sizeof(hdr), "%d", j);
                ImGui::TableSetupColumn(hdr);
            }
            ImGui::TableHeadersRow();
            for (int i = 0; i < JCE_PS_LAYER_COUNT; i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const char *nm = get_layer_name(i);
                ImGui::Text("%2d %s", i, (nm && *nm) ? nm : "(unnamed)");
                /* Lower triangle only — symmetric. */
                for (int j = 0; j < JCE_PS_LAYER_COUNT; j++) {
                    ImGui::TableSetColumnIndex(j + 1);
                    if (j > i) { ImGui::TextDisabled(""); continue; }
                    char id[32]; snprintf(id, sizeof(id), "##c%d_%d", i, j);
                    bool on = (mat[i] & (1u << j)) != 0;
                    if (ImGui::Checkbox(id, &on)) {
                        if (on) { mat[i] |= (1u << j); mat[j] |= (1u << i); }
                        else    { mat[i] &= ~(1u << j); mat[j] &= ~(1u << i); }
                        s_ps.dirty = true;
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

const char *layer_name_of(int i)
{
    return s_ps.ps.tags_layers.layers[i];
}

void draw_physics_page()
{
    page_header(category_name(CAT_PHYSICS));
    JceProjectPhysics &p = s_ps.ps.physics;
    DIRTY_IF(ImGui::DragFloat3(jce_editor_i18n("projectSettings.common.gravity"), p.gravity, 0.05f, -100.0f, 100.0f, "%.3f"));
    DIRTY_IF(ImGui::DragFloat (jce_editor_i18n("projectSettings.physics.defaultContactOffset"), &p.default_contact_offset,
                               0.001f, 0.0f, 1.0f, "%.4f"));
    DIRTY_IF(ImGui::SliderInt (jce_editor_i18n("projectSettings.physics.solverIterations"),          &p.default_solver_iterations,          1, 32));
    DIRTY_IF(ImGui::SliderInt (jce_editor_i18n("projectSettings.physics.solverVelocityIterations"), &p.default_solver_velocity_iterations, 1, 32));
    DIRTY_IF(ImGui::DragFloat (jce_editor_i18n("projectSettings.physics.bounceThreshold"), &p.bounce_threshold, 0.01f, 0.0f, 100.0f));
    DIRTY_IF(ImGui::DragFloat (jce_editor_i18n("projectSettings.physics.sleepThreshold"),  &p.sleep_threshold,  0.001f, 0.0f, 1.0f, "%.4f"));
    DIRTY_IF(ImGui::Checkbox  (jce_editor_i18n("projectSettings.physics.queriesHitTriggers"),   &p.queries_hit_triggers));
    DIRTY_IF(ImGui::Checkbox  (jce_editor_i18n("projectSettings.physics.queriesHitBackfaces"),  &p.queries_hit_backfaces));
    DIRTY_IF(ImGui::Checkbox  (jce_editor_i18n("projectSettings.physics.autoSimulation"),        &p.auto_simulation));
    ImGui::Spacing();
    draw_layer_collision_matrix(p.layer_collision_matrix, layer_name_of);
}

void draw_physics2d_page()
{
    page_header(category_name(CAT_PHYSICS2D));
    JceProjectPhysics2D &p = s_ps.ps.physics2d;
    DIRTY_IF(ImGui::DragFloat2(jce_editor_i18n("projectSettings.common.gravity"), p.gravity, 0.05f, -100.0f, 100.0f, "%.3f"));
    DIRTY_IF(ImGui::SliderInt(jce_editor_i18n("projectSettings.physics2d.velocityIterations"), &p.velocity_iterations, 1, 32));
    DIRTY_IF(ImGui::SliderInt(jce_editor_i18n("projectSettings.physics2d.positionIterations"), &p.position_iterations, 1, 32));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.physics.queriesHitTriggers"),   &p.queries_hit_triggers));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.physics2d.autoSyncTransforms"),   &p.auto_sync_transforms));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.physics.autoSimulation"),        &p.auto_simulation));
    ImGui::Spacing();
    draw_layer_collision_matrix(p.layer_collision_matrix, layer_name_of);
}

void draw_player_page()
{
    page_header(category_name(CAT_PLAYER));
    JceProjectPlayer &p = s_ps.ps.player;
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.player.companyName"), p.company_name, JCE_PS_NAME_LEN));
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.player.productName"), p.product_name, JCE_PS_NAME_LEN));
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.player.version"),      p.version,      sizeof(p.version)));
    ImGui::Spacing();
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.player.defaultIconPath"),   p.default_icon_path,   JCE_PS_PATH_LEN));
    DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.player.defaultCursorPath"), p.default_cursor_path, JCE_PS_PATH_LEN));
    DIRTY_IF(ImGui::ColorEdit3(jce_editor_i18n("projectSettings.player.splashBgColor"),    p.splash_bg_color));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.player.showSplash"),         &p.show_splash));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.player.runInBackground"),   &p.run_in_background));
    DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.player.fullscreenDefault"),  &p.fullscreen_default));
    DIRTY_IF(ImGui::DragInt(jce_editor_i18n("projectSettings.player.defaultScreenWidth"),  &p.default_screen_width,  1, 320, 7680));
    DIRTY_IF(ImGui::DragInt(jce_editor_i18n("projectSettings.player.defaultScreenHeight"), &p.default_screen_height, 1, 240, 4320));
}

void draw_preset_manager_page()
{
    page_header(category_name(CAT_PRESET_MANAGER));
    JceProjectPresetManager &pm = s_ps.ps.presets;
    ImGui::Text("%s %d / %d", jce_editor_i18n("projectSettings.presetManager.bindings"), pm.count, JCE_PS_MAX_PRESET_BINDINGS);
    if (ImGui::Button(jce_editor_i18n("projectSettings.presetManager.addBinding")) && pm.count < JCE_PS_MAX_PRESET_BINDINGS) {
        memset(&pm.bindings[pm.count], 0, sizeof(pm.bindings[0]));
        pm.count++;
        s_ps.dirty = true;
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("##pm", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders, ImVec2(0, 0))) {
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.componentType"), ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.presetPath"),    ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.filter"),         ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("",               ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        int remove_idx = -1;
        for (int i = 0; i < pm.count; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            DIRTY_IF(ImGui::InputText("##ct", pm.bindings[i].component_type, JCE_PS_NAME_LEN));
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            DIRTY_IF(ImGui::InputText("##pp", pm.bindings[i].preset_path, JCE_PS_PATH_LEN));
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            DIRTY_IF(ImGui::InputText("##ft", pm.bindings[i].filter, JCE_PS_NAME_LEN));
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton(jce_editor_i18n("projectSettings.common.remove"))) remove_idx = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove_idx >= 0) {
            for (int i = remove_idx; i < pm.count - 1; i++)
                pm.bindings[i] = pm.bindings[i + 1];
            pm.count--;
            s_ps.dirty = true;
        }
    }
}

void draw_quality_page()
{
    page_header(category_name(CAT_QUALITY));
    JceProjectQuality &q = s_ps.ps.quality;
    if (ImGui::Button(jce_editor_i18n("projectSettings.quality.addLevel")) && q.count < JCE_PS_MAX_QUALITY_LEVELS) {
        snprintf(q.levels[q.count].name, JCE_PS_NAME_LEN, "Level %d", q.count);
        q.levels[q.count].vsync_count   = 1;
        q.levels[q.count].target_framerate = -1;
        q.levels[q.count].lod_bias      = 1.0f;
        q.levels[q.count].shadow_distance = 50.0f;
        q.count++;
        s_ps.dirty = true;
    }
    ImGui::SameLine();
    ImGui::Text("(%d / %d)", q.count, JCE_PS_MAX_QUALITY_LEVELS);

    ImGui::Spacing();
    DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.currentLevel"), &q.current_level,
                          [](void *u, int i, const char **o) -> bool {
                              auto *qq = (JceProjectQuality *)u;
                              if (i < 0 || i >= qq->count) return false;
                              *o = qq->levels[i].name; return true;
                          }, &q, q.count));

    if (ImGui::BeginTabBar("##qlvls")) {
        for (int i = 0; i < q.count; i++) {
            ImGui::PushID(i);
            char tab[80]; snprintf(tab, sizeof(tab), "%s##t", q.levels[i].name);
            if (ImGui::BeginTabItem(tab)) {
                JceProjectQualityLevel *lv = &q.levels[i];
                DIRTY_IF(ImGui::InputText(jce_editor_i18n("projectSettings.common.name"), lv->name, JCE_PS_NAME_LEN));
                DIRTY_IF(ImGui::SliderInt(jce_editor_i18n("projectSettings.quality.pixelLightCount"), &lv->pixel_light_count, 0, 16));
                static const char *tx[] = { "Full", "Half", "Quarter", "Eighth" };
                DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.textureQuality"), &lv->texture_quality, tx, 4));
                static const char *aniso[] = { "Disabled", "Per Texture", "Forced On" };
                DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.anisotropic"), &lv->anisotropic, aniso, 3));
                static const char *aa[] = { "Off", "2x", "4x", "8x" };
                int aa_i = (lv->anti_aliasing <= 0) ? 0 :
                           (lv->anti_aliasing == 2) ? 1 :
                           (lv->anti_aliasing == 4) ? 2 : 3;
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.antiAliasing"), &aa_i, aa, 4)) {
                    static const int v[] = { 0, 2, 4, 8 };
                    lv->anti_aliasing = v[aa_i]; s_ps.dirty = true;
                }
                DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.quality.softParticles"),            &lv->soft_particles));
                DIRTY_IF(ImGui::Checkbox(jce_editor_i18n("projectSettings.quality.realtimeReflectionProbes"),&lv->realtime_reflection_probes));
                static const char *sq[] = { "Disable", "Hard Only", "Hard + Soft" };
                DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowQuality"), &lv->shadow_quality, sq, 3));
                static const char *sr[] = { "Low", "Medium", "High", "Very High" };
                DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowResolution"), &lv->shadow_resolution, sr, 4));
                DIRTY_IF(ImGui::DragFloat(jce_editor_i18n("projectSettings.quality.shadowDistance"), &lv->shadow_distance, 1.0f, 0.0f, 5000.0f));
                static const char *sc[] = { "1", "2", "4" };
                int sc_i = (lv->shadow_cascades <= 1) ? 0 :
                           (lv->shadow_cascades == 2) ? 1 : 2;
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowCascades"), &sc_i, sc, 3)) {
                    static const int v[] = { 1, 2, 4 };
                    lv->shadow_cascades = v[sc_i]; s_ps.dirty = true;
                }
                static const char *vs[] = { "Off", "Every VBlank", "Every 2nd VBlank" };
                DIRTY_IF(ImGui::Combo(jce_editor_i18n("projectSettings.quality.vsync"), &lv->vsync_count, vs, 3));
                DIRTY_IF(ImGui::DragInt(jce_editor_i18n("projectSettings.quality.targetFramerate"),
                                        &lv->target_framerate, 1, -1, 480));
                DIRTY_IF(ImGui::DragFloat(jce_editor_i18n("projectSettings.quality.lodBias"), &lv->lod_bias, 0.05f, 0.1f, 10.0f));
                if (ImGui::SmallButton(jce_editor_i18n("projectSettings.quality.deleteLevel")) && q.count > 1) {
                    for (int j = i; j < q.count - 1; j++) q.levels[j] = q.levels[j + 1];
                    q.count--;
                    if (q.current_level >= q.count) q.current_level = q.count - 1;
                    s_ps.dirty = true;
                }
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
}

void draw_string_array_editor(const char *id, char (*arr)[JCE_PS_NAME_LEN],
                              int *count, int max_count, int builtin_count)
{
    ImGui::Text("(%d / %d)", *count, max_count);
    int remove_idx = -1;
    for (int i = 0; i < *count; i++) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-80);
        char label[24]; snprintf(label, sizeof(label), "%d##%s", i, id);
        bool readonly = (i < builtin_count);
        if (readonly) ImGui::BeginDisabled();
        DIRTY_IF(ImGui::InputText(label, arr[i], JCE_PS_NAME_LEN));
        if (readonly) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!readonly && ImGui::SmallButton("X")) remove_idx = i;
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        for (int i = remove_idx; i < *count - 1; i++)
            memcpy(arr[i], arr[i + 1], JCE_PS_NAME_LEN);
        arr[(*count) - 1][0] = '\0';
        (*count)--;
        s_ps.dirty = true;
    }
    if (*count < max_count && ImGui::SmallButton(jce_editor_i18n("projectSettings.common.addPlus"))) {
        snprintf(arr[*count], JCE_PS_NAME_LEN, "New %s %d", id, *count);
        (*count)++;
        s_ps.dirty = true;
    }
}

void draw_tags_layers_page()
{
    page_header(category_name(CAT_TAGS_LAYERS));
    JceProjectTagsAndLayers &t = s_ps.ps.tags_layers;
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.tags"), ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_string_array_editor("tag", t.tags, &t.tag_count, JCE_PS_MAX_TAGS, 0);
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.sortingLayers"), ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_string_array_editor("sortlayer", t.sorting_layers,
                                 &t.sorting_layer_count, JCE_PS_MAX_SORTING_LAYERS, 0);
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.layers"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled(jce_editor_i18n("projectSettings.tagsLayers.builtinHint"));
        for (int i = 0; i < JCE_PS_LAYER_COUNT; i++) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-1);
            char label[32]; snprintf(label, sizeof(label), "Layer %d", i);
            bool readonly = (i < 8);
            if (readonly) ImGui::BeginDisabled();
            DIRTY_IF(ImGui::InputText(label, t.layers[i], JCE_PS_NAME_LEN));
            if (readonly) ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
}

void draw_time_page()
{
    page_header(category_name(CAT_TIME));
    JceProjectTime &t = s_ps.ps.time;
    DIRTY_IF(ImGui::DragFloat(jce_editor_i18n("projectSettings.time.fixedTimestep"),       &t.fixed_timestep,       0.001f, 0.0001f, 1.0f, "%.4f"));
    DIRTY_IF(ImGui::DragFloat(jce_editor_i18n("projectSettings.time.maxAllowedTimestep"), &t.max_allowed_timestep, 0.01f, 0.01f, 5.0f, "%.4f"));
    DIRTY_IF(ImGui::DragFloat(jce_editor_i18n("projectSettings.time.timeScale"),           &t.time_scale,           0.05f, 0.0f, 100.0f, "%.3f"));
    DIRTY_IF(ImGui::DragInt  (jce_editor_i18n("projectSettings.time.maxParticleTimestep"),
                              &t.maximum_particle_timestep_ms, 1, 1, 1000));
    ImGui::TextDisabled(jce_editor_i18n("projectSettings.time.applyHint"));
}

#undef DIRTY_IF

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
    ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_Appearing);
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
        case CAT_PROJECT:        draw_project_page();        break;
        case CAT_BUILD:          draw_build_page();          break;
        case CAT_RUN:            draw_run_page();            break;
        case CAT_RENDER:         draw_render_page();         break;
        case CAT_HOTKEYS:        draw_hotkeys_page();        break;
        case CAT_AUDIO:          draw_audio_page();          break;
        case CAT_EDITOR_PREFS:   draw_editor_prefs_page();   break;
        case CAT_GRAPHICS:       draw_graphics_page();       break;
        case CAT_INPUT:          draw_input_page();          break;
        case CAT_PHYSICS:        draw_physics_page();        break;
        case CAT_PHYSICS2D:      draw_physics2d_page();      break;
        case CAT_PLAYER:         draw_player_page();         break;
        case CAT_PRESET_MANAGER: draw_preset_manager_page(); break;
        case CAT_QUALITY:        draw_quality_page();        break;
        case CAT_TAGS_LAYERS:    draw_tags_layers_page();    break;
        case CAT_TIME:           draw_time_page();           break;
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
