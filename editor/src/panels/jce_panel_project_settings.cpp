/*
 * jce_panel_project_settings.cpp  Unity-style Project Settings hub (P4-A.1).
 *
 * Single dockable window aggregating per-project configuration that
 * previously lived in 16+ separate panels (Input Manager, Physics
 * Layers, Lighting Settings, ...).  Left pane: vertical tab list;
 * right pane: tab content.
 *
 * Data is read from / written to JceProjectSettings via the existing
 * jce_project_settings_load/save API (editor/src/core/jce_project_settings).
 * Tabs whose backing surface is owned by a later P4 phase render a
 * small "(wired in P4-X.Y)" placeholder instead of blocking the user.
 *
 * Embeds (not duplicates) Input via jce_editor_panel_input_manager_content().
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_log.h>
#include <jce/middleware/ui/jce_localization.h>
#include "core/jce_editor_game_l10n.h"
#include "core/jce_editor_project.h"
#include "core/jce_hotkeys.h"
#include "core/jce_pak_key.h"
#include "core/jce_project_settings.h"
#include "dialogs/jce_path_input.h"
#include "ui/jce_editor_tip.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>
#include <cstdlib>

extern "C" {
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_quality_preset.h>
}

namespace {

enum Tab {
    TAB_PROJECT = 0,
    TAB_BUILD,
    TAB_RUN,
    TAB_HOTKEYS,
    TAB_INPUT,
    TAB_PHYSICS,
    TAB_PHYSICS2D,
    TAB_TAGS_LAYERS,
    TAB_QUALITY,
    TAB_GRAPHICS,
    TAB_TIME,
    TAB_PLAYER,
    TAB_PACKAGING,
    TAB_PRESET_MANAGER,
    TAB_AUDIO,
    TAB_EDITOR,
    TAB_LOCALIZATION,
    TAB_COUNT
};

struct State {
    bool                loaded = false;
    bool                dirty  = false;
    int                 tab    = TAB_INPUT;
    char                filter[64] = {0};
    JceProjectSettings  ps;
    JceEditorConfig     cfg;
    int                 hk_recording_id = -1;
    char                hk_filter[64] = {0};

    /* ── Game L10n grid (TAB_LOCALIZATION) ── */
    char                loc_filter[64]    = {0};
    char                loc_new_key[128]  = {0};
    char                loc_new_locale[16]= {0};

    /* ── Active jce_project.json editable buffers (TAB_PROJECT) ── */
    char                jp_root_cached[1024] = {0};
    bool                jp_loaded            = false;
    char                jp_name[128]         = {0};
    char                jp_version[64]       = {0};
    char                jp_sdk_path[512]     = {0};
    char                jp_source_assets[256]= {0};
    char                jp_cooked_assets[256]= {0};
    char                jp_startup_scene[512]= {0};
    char                jp_save_msg[256]     = {0};
    /* Bundles list: editable in-memory (relative-to-root strings), kept
     * in sync with the cached JceProject on load + on save. */
    std::vector<std::string> jp_bundles;
    bool                jp_bundle_picker_open = false;
    /* Persistent buffer for the bundle picker — jce_draw_path_input is
     * async, so the buffer must outlive the frame in which "Browse" is
     * clicked.  We push to jp_bundles + clear once a path appears. */
    char                jp_bundle_add_buf[1024] = {0};
};

State g_st;

const char *tab_i18n_key(int t)
{
    switch (t) {
        case TAB_PROJECT:        return "panel.project_settings.tab.project";
        case TAB_BUILD:          return "panel.project_settings.tab.build";
        case TAB_RUN:            return "panel.project_settings.tab.run";
        case TAB_HOTKEYS:        return "panel.project_settings.tab.hotkeys";
        case TAB_INPUT:          return "panel.project_settings.tab.input";
        case TAB_PHYSICS:        return "panel.project_settings.tab.physics";
        case TAB_PHYSICS2D:      return "panel.project_settings.tab.physics2d";
        case TAB_TAGS_LAYERS:    return "panel.project_settings.tab.tags_layers";
        case TAB_QUALITY:        return "panel.project_settings.tab.quality";
        case TAB_GRAPHICS:       return "panel.project_settings.tab.graphics";
        case TAB_TIME:           return "panel.project_settings.tab.time";
        case TAB_PLAYER:         return "panel.project_settings.tab.player";
        case TAB_PACKAGING:      return "panel.project_settings.tab.packaging";
        case TAB_PRESET_MANAGER: return "panel.project_settings.tab.preset_manager";
        case TAB_AUDIO:          return "panel.project_settings.tab.audio";
        case TAB_EDITOR:         return "panel.project_settings.tab.editor";
        case TAB_LOCALIZATION:   return "panel.project_settings.tab.localization";
        default:                 return "";
    }
}

const char *tab_fallback(int t)
{
    switch (t) {
        case TAB_PROJECT:        return "Project";
        case TAB_BUILD:          return "Build";
        case TAB_RUN:            return "Run";
        case TAB_HOTKEYS:        return "Hotkeys";
        case TAB_INPUT:          return "Input";
        case TAB_PHYSICS:        return "Physics";
        case TAB_PHYSICS2D:      return "Physics 2D";
        case TAB_TAGS_LAYERS:    return "Tags & Layers";
        case TAB_QUALITY:        return "Quality";
        case TAB_GRAPHICS:       return "Graphics";
        case TAB_TIME:           return "Time";
        case TAB_PLAYER:         return "Player";
        case TAB_PACKAGING:      return "Packaging";
        case TAB_PRESET_MANAGER: return "Preset Manager";
        case TAB_AUDIO:          return "Audio";
        case TAB_EDITOR:         return "Editor";
        case TAB_LOCALIZATION:   return "Localization";
        default:                 return "?";
    }
}

void ensure_loaded(void)
{
    if (g_st.loaded) return;
    jce_project_settings_defaults(&g_st.ps);
    jce_project_settings_load(&g_st.ps);
    if (!jce_editor_config_load(&g_st.cfg))
        jce_editor_config_defaults(&g_st.cfg);
    g_st.loaded = true;
    g_st.dirty  = false;
}

void mark_dirty(void) { g_st.dirty = true; }

void save_if_dirty(void)
{
    if (!g_st.dirty) return;
    bool ok_ps  = jce_project_settings_save(&g_st.ps);

    /* Reload-before-save (defense-in-depth against the whole-struct-from-
     * snapshot clobber): pull the current on-disk editor-config and overlay
     * ONLY the build/run + project fields this panel authors, so a save here
     * never reverts a sibling field (run_mode set in Game View, exe path
     * auto-updated by Build Profiles, prefs toggled in Preferences) changed
     * since this panel was opened.  Keep this list in sync with
     * draw_build / draw_run / draw_project. */
    JceEditorConfig disk;
    if (!jce_editor_config_load(&disk)) disk = g_st.cfg;
    snprintf(disk.build_configure_preset, sizeof(disk.build_configure_preset),
             "%s", g_st.cfg.build_configure_preset);
    snprintf(disk.build_preset, sizeof(disk.build_preset),
             "%s", g_st.cfg.build_preset);
    snprintf(disk.build_output_path, sizeof(disk.build_output_path),
             "%s", g_st.cfg.build_output_path);
    snprintf(disk.game_target_name, sizeof(disk.game_target_name),
             "%s", g_st.cfg.game_target_name);
    disk.run_mode = g_st.cfg.run_mode;
    snprintf(disk.game_executable_path, sizeof(disk.game_executable_path),
             "%s", g_st.cfg.game_executable_path);
    snprintf(disk.game_working_directory, sizeof(disk.game_working_directory),
             "%s", g_st.cfg.game_working_directory);
    snprintf(disk.last_project, sizeof(disk.last_project),
             "%s", g_st.cfg.last_project);
    disk.recent_count = g_st.cfg.recent_count;  /* draw_project only clears */
    bool ok_cfg = jce_editor_config_save(&disk);
    g_st.cfg = disk;   /* keep the snapshot consistent with what we wrote */

    jce_hotkeys_save();
    if (ok_ps)
        jce_project_settings_apply(&g_st.ps);
    if (ok_ps && ok_cfg)
        g_st.dirty = false;
}

/* ── Tab renderers ─────────────────────────────────────────────────── */

void draw_input(void)
{
    /* Reuse the existing Input Manager panel content verbatim. */
    jce_editor_panel_input_manager_content();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.cat.input"));
    ImGui::TextWrapped("%s", jce_editor_i18n("projectSettings.input.hint"));
    JceProjectInput &i = g_st.ps.input;
    if (ImGui::Checkbox(jce_editor_i18n("projectSettings.input.treatKeyboardAsDpad"),
                        &i.treat_keyboard_as_dpad))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n("projectSettings.input.enableGamepad"),
                        &i.enable_gamepad))
        mark_dirty();
    if (ImGui::SliderFloat(jce_editor_i18n("projectSettings.input.deadZone"),
                           &i.dead_zone, 0.0f, 0.9f))
        mark_dirty();
    if (ImGui::SliderFloat(jce_editor_i18n("projectSettings.common.gravity"),
                           &i.gravity, 0.1f, 10.0f))
        mark_dirty();
    if (ImGui::SliderFloat(jce_editor_i18n("projectSettings.input.sensitivity"),
                           &i.sensitivity, 0.1f, 10.0f))
        mark_dirty();
}

/* PS_KEY is the common namespace for project-settings field labels. */
#define PS_KEY "panel.project_settings.field."

void draw_layer_collision_matrix(uint32_t mat[JCE_PS_LAYER_COUNT])
{
    if (!ImGui::TreeNode(jce_editor_i18n("projectSettings.physics.layerCollisionMatrix")))
        return;
    if (ImGui::BeginTable("##lcm", JCE_PS_LAYER_COUNT + 1,
            ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollX, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("");
        for (int j = 0; j < JCE_PS_LAYER_COUNT; ++j) {
            char hdr[32]; std::snprintf(hdr, sizeof(hdr), "%d", j);
            ImGui::TableSetupColumn(hdr);
        }
        ImGui::TableHeadersRow();
        for (int i = 0; i < JCE_PS_LAYER_COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const char *nm = g_st.ps.tags_layers.layers[i];
            ImGui::Text("%2d %s", i, (nm && *nm) ? nm : "(unnamed)");
            for (int j = 0; j < JCE_PS_LAYER_COUNT; ++j) {
                ImGui::TableSetColumnIndex(j + 1);
                if (j > i) { ImGui::TextDisabled(""); continue; }
                char id[32]; std::snprintf(id, sizeof(id), "##c%d_%d", i, j);
                bool on = (mat[i] & (1u << j)) != 0;
                if (ImGui::Checkbox(id, &on)) {
                    if (on) { mat[i] |= (1u << j); mat[j] |= (1u << i); }
                    else    { mat[i] &= ~(1u << j); mat[j] &= ~(1u << i); }
                    mark_dirty();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

void draw_physics(void)
{
    JceProjectPhysics &p = g_st.ps.physics;
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "gravity", "Gravity"));
    if (ImGui::DragFloat3("##phys_gravity", p.gravity, 0.05f, -50.0f, 50.0f))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "solver_iters", "solver_iters"),
                       &p.default_solver_iterations, 1.0f, 1, 64))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "solver_vel_iters", "solver_vel_iters"),
                       &p.default_solver_velocity_iterations, 1.0f, 1, 64))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "contact_offset", "contact_offset"),
                         &p.default_contact_offset, 0.001f, 0.0001f, 1.0f))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "bounce_threshold", "bounce_threshold"),
                         &p.bounce_threshold, 0.05f, 0.0f, 100.0f))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "sleep_threshold", "sleep_threshold"),
                         &p.sleep_threshold, 0.05f, 0.0f, 100.0f))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "queries_hit_triggers", "queries_hit_triggers"),
                        &p.queries_hit_triggers))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n("projectSettings.physics.queriesHitBackfaces"),
                        &p.queries_hit_backfaces))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "auto_simulation", "auto_simulation"),
                        &p.auto_simulation))
        mark_dirty();
    ImGui::Separator();
    draw_layer_collision_matrix(p.layer_collision_matrix);
}

void draw_physics2d(void)
{
    JceProjectPhysics2D &p = g_st.ps.physics2d;
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "gravity2d", "Gravity (2D)"));
    if (ImGui::DragFloat2("##phys2d_gravity", p.gravity, 0.05f, -50.0f, 50.0f))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "velocity_iters", "velocity_iters"),
                       &p.velocity_iterations, 1.0f, 1, 64))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "position_iters", "position_iters"),
                       &p.position_iterations, 1.0f, 1, 64))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "queries_hit_triggers", "queries_hit_triggers_2d"),
                        &p.queries_hit_triggers))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "auto_sync_transforms", "auto_sync_transforms"),
                        &p.auto_sync_transforms))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "auto_simulation", "auto_simulation_2d"),
                        &p.auto_simulation))
        mark_dirty();
    ImGui::Separator();
    draw_layer_collision_matrix(p.layer_collision_matrix);
}

void draw_string_array_editor(const char *id, char (*arr)[JCE_PS_NAME_LEN],
                              int *count, int max_count, int builtin_count)
{
    ImGui::Text("(%d / %d)", *count, max_count);
    int remove_idx = -1;
    for (int i = 0; i < *count; ++i) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-80);
        char label[24]; std::snprintf(label, sizeof(label), "%d##%s", i, id);
        bool readonly = (i < builtin_count);
        if (readonly) ImGui::BeginDisabled();
        if (ImGui::InputText(label, arr[i], JCE_PS_NAME_LEN))
            mark_dirty();
        if (readonly) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!readonly && ImGui::SmallButton("X")) remove_idx = i;
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        for (int i = remove_idx; i < *count - 1; ++i)
            std::memcpy(arr[i], arr[i + 1], JCE_PS_NAME_LEN);
        arr[(*count) - 1][0] = '\0';
        (*count)--;
        mark_dirty();
    }
    if (*count < max_count &&
        ImGui::SmallButton(jce_editor_i18n("projectSettings.common.addPlus"))) {
        std::snprintf(arr[*count], JCE_PS_NAME_LEN, "New %s %d", id, *count);
        (*count)++;
        mark_dirty();
    }
}

void draw_tags_layers(void)
{
    JceProjectTagsAndLayers &t = g_st.ps.tags_layers;
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.tags"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_string_array_editor("tag", t.tags, &t.tag_count, JCE_PS_MAX_TAGS, 0);
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.sortingLayers"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_string_array_editor("sortlayer", t.sorting_layers,
                                 &t.sorting_layer_count,
                                 JCE_PS_MAX_SORTING_LAYERS, 0);
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("projectSettings.tagsLayers.layers"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled(jce_editor_i18n("projectSettings.tagsLayers.builtinHint"));
        for (int i = 0; i < JCE_PS_LAYER_COUNT; ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-1);
            char label[32]; std::snprintf(label, sizeof(label), "Layer %d", i);
            bool readonly = (i < 8);
            if (readonly) ImGui::BeginDisabled();
            if (ImGui::InputText(label, t.layers[i], JCE_PS_NAME_LEN))
                mark_dirty();
            if (readonly) ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
}

void draw_quality(void)
{
    const char *k_tier_names[] = {
        jce_editor_i18n_or(PS_KEY "tier.low",    "Low"),
        jce_editor_i18n_or(PS_KEY "tier.medium", "Medium"),
        jce_editor_i18n_or(PS_KEY "tier.high",   "High"),
        jce_editor_i18n_or(PS_KEY "tier.ultra",  "Ultra"),
    };
    static const JceQualityTier k_tiers[] = {
        JCE_QUALITY_LOW, JCE_QUALITY_MED, JCE_QUALITY_HIGH, JCE_QUALITY_ULTRA
    };
    static int s_tier_idx = 2;  /* default: High */

    ImGui::TextUnformatted(
        jce_editor_i18n_or(PS_KEY "quality_presets", "Quality Presets"));
    ImGui::Separator();

    /* Tier selector. */
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo(jce_editor_i18n_id(PS_KEY "quality.tier", "q_tier"),
                     &s_tier_idx, k_tier_names, 4)) {
        /* preview-only: apply on button press below */
    }

    ImGui::Spacing();

    /* Summary table — show every tier's key settings side by side. */
    if (ImGui::BeginTable("##q_table", 5,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg   |
                          ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(
            jce_editor_i18n_or(PS_KEY "quality.col.feature", "Feature"),
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(k_tier_names[0], ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn(k_tier_names[1], ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn(k_tier_names[2], ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn(k_tier_names[3], ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();

        JceQualityPreset presets[4];
        for (int i = 0; i < 4; ++i)
            jce_quality_preset_get(k_tiers[i], &presets[i]);

        /* Helper: emit one boolean row. */
        auto bool_row = [&](const char *label, bool v0, bool v1, bool v2, bool v3) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
            const bool vals[4] = { v0, v1, v2, v3 };
            for (int c = 1; c <= 4; ++c) {
                ImGui::TableSetColumnIndex(c);
                bool hi = (c - 1 == s_tier_idx);
                if (hi) ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                ImGui::TextUnformatted(vals[c - 1]
                    ? jce_editor_i18n_or(PS_KEY "on",  "On")
                    : jce_editor_i18n_or(PS_KEY "off", "Off"));
                if (hi) ImGui::PopStyleColor();
            }
        };

        bool_row(jce_editor_i18n_or(PS_KEY "feature.csm", "Shadows (CSM)"),
            presets[0].rp.enable_csm, presets[1].rp.enable_csm,
            presets[2].rp.enable_csm, presets[3].rp.enable_csm);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.ssao", "SSAO"),
            presets[0].rp.enable_ssao, presets[1].rp.enable_ssao,
            presets[2].rp.enable_ssao, presets[3].rp.enable_ssao);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.ssr", "SSR"),
            presets[0].rp.enable_ssr, presets[1].rp.enable_ssr,
            presets[2].rp.enable_ssr, presets[3].rp.enable_ssr);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.taa", "TAA"),
            presets[0].rp.enable_taa, presets[1].rp.enable_taa,
            presets[2].rp.enable_taa, presets[3].rp.enable_taa);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.bloom", "Bloom"),
            presets[0].rp.enable_bloom, presets[1].rp.enable_bloom,
            presets[2].rp.enable_bloom, presets[3].rp.enable_bloom);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.volumetricFog", "Volumetric Fog"),
            presets[0].rp.enable_volumetric_fog, presets[1].rp.enable_volumetric_fog,
            presets[2].rp.enable_volumetric_fog, presets[3].rp.enable_volumetric_fog);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.gpuParticles", "GPU Particles"),
            presets[0].rp.enable_gpu_particles, presets[1].rp.enable_gpu_particles,
            presets[2].rp.enable_gpu_particles, presets[3].rp.enable_gpu_particles);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.motionBlur", "Motion Blur"),
            presets[0].rp.enable_motion_blur, presets[1].rp.enable_motion_blur,
            presets[2].rp.enable_motion_blur, presets[3].rp.enable_motion_blur);
        bool_row(jce_editor_i18n_or(PS_KEY "feature.clothSim", "Cloth Sim"),
            presets[0].rp.enable_cloth, presets[1].rp.enable_cloth,
            presets[2].rp.enable_cloth, presets[3].rp.enable_cloth);

        /* Shadow resolution row. */
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "shadowRes", "Shadow Res"));
            for (int c = 1; c <= 4; ++c) {
                ImGui::TableSetColumnIndex(c);
                bool hi = (c - 1 == s_tier_idx);
                if (hi) ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                char buf[8];
                snprintf(buf, sizeof(buf), "%d",
                         (int)presets[c - 1].rp.shadow_resolution);
                ImGui::TextUnformatted(buf);
                if (hi) ImGui::PopStyleColor();
            }
        }

        /* Shadow filter row (per-pixel PCF tap tier, not map size). */
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "shadowFilter", "Shadow Filter"));
            static const char *k_filter_names[] = { "1-tap", "3x3", "5x5" };
            for (int c = 1; c <= 4; ++c) {
                ImGui::TableSetColumnIndex(c);
                bool hi = (c - 1 == s_tier_idx);
                if (hi) ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                int q = (int)presets[c - 1].rp.shadow_filter_quality;
                if (q < 0) q = 0; if (q > 2) q = 2;
                ImGui::TextUnformatted(k_filter_names[q]);
                if (hi) ImGui::PopStyleColor();
            }
        }

        /* Max lights row. */
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "maxLights", "Max Lights"));
            for (int c = 1; c <= 4; ++c) {
                ImGui::TableSetColumnIndex(c);
                bool hi = (c - 1 == s_tier_idx);
                if (hi) ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
                char buf[8];
                snprintf(buf, sizeof(buf), "%d",
                         presets[c - 1].max_active_lights);
                ImGui::TextUnformatted(buf);
                if (hi) ImGui::PopStyleColor();
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n_id(PS_KEY "quality.apply", "q_apply"))) {
        JceQualityPreset p;
        jce_quality_preset_get(k_tiers[s_tier_idx], &p);
        jce_quality_preset_apply(&p);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", k_tier_names[s_tier_idx]);
    ImGui::SameLine();
    ImGui::TextDisabled("—");
    ImGui::SameLine();
    ImGui::TextDisabled("%s",
        jce_editor_i18n_or(PS_KEY "quality.note",
            "applies to the running session; saved in .rp.json per-project"));
}

void draw_graphics(void)
{
    JceGpuTier  tier = jce_renderer_get_tier();
    const char *name = jce_gpu_tier_name(tier);
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "gpu_tier", "Current GPU Tier (read-only):"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s",
                       name ? name : "?");

    JceProjectGraphics &g = g_st.ps.graphics;
    ImGui::Separator();
    const char *cs_items[] = {
        jce_editor_i18n_or(PS_KEY "color_space.gamma",  "Gamma"),
        jce_editor_i18n_or(PS_KEY "color_space.linear", "Linear"),
    };
    int cs = g.color_space;
    if (cs < 0) cs = 0; if (cs > 1) cs = 1;
    if (ImGui::Combo(jce_editor_i18n_id(PS_KEY "color_space", "color_space"),
                     &cs, cs_items, IM_ARRAYSIZE(cs_items))) {
        g.color_space = cs; mark_dirty();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "hdr", "hdr"), &g.hdr)) mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "srgb_write", "srgb_write"),
                        &g.srgb_write)) mark_dirty();
    const char *msaa_items[] = {
        jce_editor_i18n_or(PS_KEY "msaa.off", "Off"), "2x", "4x", "8x"
    };
    int msaa_idx = 0;
    switch (g.default_msaa) { case 2: msaa_idx = 1; break;
                              case 4: msaa_idx = 2; break;
                              case 8: msaa_idx = 3; break;
                              default: msaa_idx = 0; }
    if (ImGui::Combo(jce_editor_i18n_id(PS_KEY "default_msaa", "default_msaa"),
                     &msaa_idx, msaa_items, IM_ARRAYSIZE(msaa_items))) {
        const int v[] = { 0, 2, 4, 8 };
        g.default_msaa = v[msaa_idx]; mark_dirty();
    }
    static const char *aniso[] = {
        jce_editor_i18n_or(PS_KEY "aniso.disabled", "Disabled"),
        jce_editor_i18n_or(PS_KEY "aniso.perTexture", "Per Texture"),
        jce_editor_i18n_or(PS_KEY "aniso.forced",   "Forced On"),
    };
    int an = g.anisotropic_textures;
    if (an < 0) an = 0; if (an > 2) an = 2;
    if (ImGui::Combo(jce_editor_i18n("projectSettings.graphics.anisotropicTextures"),
                     &an, aniso, IM_ARRAYSIZE(aniso))) {
        g.anisotropic_textures = an; mark_dirty();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.graphics.alwaysIncludedShaders"));
    if (ImGui::InputTextMultiline("##aishaders", g.always_included_shaders,
                                  sizeof(g.always_included_shaders),
                                  ImVec2(-1, 120)))
        mark_dirty();
}

void draw_time(void)
{
    JceProjectTime &t = g_st.ps.time;
    float hz = (t.fixed_timestep > 0.0f) ? (1.0f / t.fixed_timestep) : 50.0f;
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "fixed_update_hz", "fixed_update_hz"),
                         &hz, 1.0f, 1.0f, 240.0f, "%.1f")) {
        if (hz < 1.0f) hz = 1.0f;
        t.fixed_timestep = 1.0f / hz;
        mark_dirty();
    }
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "fixed_timestep", "fixed_timestep"),
                         &t.fixed_timestep, 0.001f, 0.001f, 1.0f, "%.4f"))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "max_allowed_timestep", "max_allowed_timestep"),
                         &t.max_allowed_timestep, 0.01f, 0.01f, 5.0f, "%.3f"))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "time_scale", "time_scale"),
                         &t.time_scale, 0.05f, 0.0f, 10.0f, "%.2f"))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n("projectSettings.time.maxParticleTimestep"),
                       &t.maximum_particle_timestep_ms, 1, 1, 1000))
        mark_dirty();
    ImGui::TextDisabled("%s", jce_editor_i18n("projectSettings.time.applyHint"));
}

void draw_player(void)
{
    JceProjectPlayer &p = g_st.ps.player;
    if (ImGui::InputText(jce_editor_i18n_id(PS_KEY "company_name", "company_name"),
                         p.company_name, sizeof(p.company_name)))
        mark_dirty();
    if (ImGui::InputText(jce_editor_i18n_id(PS_KEY "product_name", "product_name"),
                         p.product_name, sizeof(p.product_name)))
        mark_dirty();
    if (ImGui::InputText(jce_editor_i18n_id(PS_KEY "version", "version"),
                         p.version, sizeof(p.version)))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "screen_w", "screen_w"),
                       &p.default_screen_width, 8.0f, 320, 7680))
        mark_dirty();
    if (ImGui::DragInt(jce_editor_i18n_id(PS_KEY "screen_h", "screen_h"),
                       &p.default_screen_height, 8.0f, 240, 4320))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "fullscreen_default", "fullscreen_default"),
                        &p.fullscreen_default))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "run_in_background", "run_in_background"),
                        &p.run_in_background))
        mark_dirty();
    ImGui::Separator();
    if (jce_draw_path_input_asset(jce_editor_i18n("projectSettings.player.defaultIconPath"),
                                  p.default_icon_path, JCE_PS_PATH_LEN))
        mark_dirty();
    if (jce_draw_path_input_asset(jce_editor_i18n("projectSettings.player.defaultCursorPath"),
                                  p.default_cursor_path, JCE_PS_PATH_LEN))
        mark_dirty();
    if (ImGui::ColorEdit3(jce_editor_i18n("projectSettings.player.splashBgColor"),
                          p.splash_bg_color))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n("projectSettings.player.showSplash"),
                        &p.show_splash))
        mark_dirty();
}

void draw_audio(void)
{
    JceProjectAudio &a = g_st.ps.audio;
    if (ImGui::SliderFloat(jce_editor_i18n_id(PS_KEY "master_volume", "master_volume"),
                           &a.master_volume, 0.0f, 1.0f))
        mark_dirty();
    if (ImGui::DragFloat(jce_editor_i18n_id(PS_KEY "doppler_factor", "doppler_factor"),
                         &a.doppler_factor, 0.01f, 0.0f, 5.0f))
        mark_dirty();
    static const char *sr_items[] = { "22050", "44100", "48000", "96000" };
    int sr_idx = (a.sample_rate == 22050) ? 0 :
                 (a.sample_rate == 44100) ? 1 :
                 (a.sample_rate == 96000) ? 3 : 2;
    if (ImGui::Combo(jce_editor_i18n_id(PS_KEY "sample_rate", "sample_rate"),
                     &sr_idx, sr_items, IM_ARRAYSIZE(sr_items))) {
        a.sample_rate = std::atoi(sr_items[sr_idx]);
        mark_dirty();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "pause_on_focus_loss", "pause_on_focus_loss"),
                        &a.pause_on_focus_loss))
        mark_dirty();
    if (ImGui::Checkbox(jce_editor_i18n_id(PS_KEY "disable_audio", "disable_audio"),
                        &a.disable_audio))
        mark_dirty();
    ImGui::Separator();
    ImGui::TextDisabled("%s", jce_editor_i18n("projectSettings.audio.liveApplyHint"));
}

void draw_editor(void)
{
    JceProjectEditor &e = g_st.ps.editor;
    /* Auto-save is a per-user editor preference (it never travels with the
     * project), configured in Preferences > General and driven by
     * jce_editor_prefs_autosave_interval_sec().  Previously duplicated here
     * as an inert control; redirect users to the single source of truth. */
    ImGui::TextDisabled("%s", jce_editor_i18n_or(
        "projectSettings.editorPrefs.autoSaveMovedHint",
        "Auto-save interval is configured in Preferences > General."));
    ImGui::Separator();
    const char *behaviour[] = { "3D", "2D" };
    int bm = e.default_behavior_mode ? 1 : 0;
    if (ImGui::Combo(jce_editor_i18n_id(PS_KEY "behavior_mode", "behavior_mode"),
                     &bm, behaviour, IM_ARRAYSIZE(behaviour))) {
        e.default_behavior_mode = bm; mark_dirty();
    }
    const char *vc[] = {
        jce_editor_i18n_or("projectSettings.editorPrefs.vc.hidden",  "Hidden Meta Files"),
        jce_editor_i18n_or("projectSettings.editorPrefs.vc.visible", "Visible Meta Files"),
    };
    int vcm = e.version_control_mode ? 1 : 0;
    if (ImGui::Combo(jce_editor_i18n("projectSettings.editorPrefs.versionControlMode"),
                     &vcm, vc, IM_ARRAYSIZE(vc))) {
        e.version_control_mode = vcm; mark_dirty();
    }
    /* External script/image editors moved to per-user Preferences >
     * External Tools (machine paths must not travel with the project). */
    ImGui::Separator();
    ImGui::TextDisabled("%s", jce_editor_i18n_or(
        "projectSettings.editorPrefs.externalToolsMovedHint",
        "External script/image editors are configured in Preferences > External Tools."));
}

void draw_localization(void)
{
    /* Editor-chrome locale (jce_editor_i18n) — read-only, shown for
     * contrast with the GAME locale below.  Two separate systems. */
    JceLocale loc = jce_editor_i18n_get_locale();
    const char *loc_name = jce_editor_i18n_locale_code(loc);
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "current_locale", "Current Locale (read-only):"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", loc_name);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* ── Game string tables (<source_assets>/i18n/<locale>.json) ────── */
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "loc_game_header",
                                              "Game String Tables"));
    if (!jce_editor_gl10n_loaded()) {
        ImGui::TextDisabled("%s", jce_editor_i18n_or(PS_KEY "loc_no_project",
            "Open a project to edit its game string tables "
            "(<source assets>/i18n/<locale>.json)."));
        return;
    }
    ImGui::TextDisabled("%s", jce_editor_gl10n_dir());
    ImGui::Spacing();

    const int nloc = jce_editor_gl10n_locale_count();

    /* Game-locale preview combo — drives the live jce_loc table so the
     * game view / Play session re-resolve UIText locale keys instantly. */
    {
        const char *cur = jce_loc_get_locale();
        if (!cur || !cur[0]) cur = "en";
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(jce_editor_i18n_id(PS_KEY "loc_preview_locale",
                                                 "loc_prev"), cur)) {
            for (int i = 0; i < nloc; ++i) {
                const char *code = jce_editor_gl10n_locale_code_at(i);
                bool sel = (std::strcmp(code, cur) == 0);
                if (ImGui::Selectable(code, sel))
                    jce_loc_set_locale(code);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    /* Filter + add-key + add-locale row. */
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##loc_filter",
                             jce_editor_i18n_or(PS_KEY "loc_filter", "Filter"),
                             g_st.loc_filter, sizeof(g_st.loc_filter));
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##loc_new_key", "ui.new_key",
                             g_st.loc_new_key, sizeof(g_st.loc_new_key));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id(PS_KEY "loc_add_key", "loc_addk"))) {
        if (jce_editor_gl10n_add_key(g_st.loc_new_key))
            g_st.loc_new_key[0] = '\0';
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputTextWithHint("##loc_new_locale", "zh_cn",
                             g_st.loc_new_locale, sizeof(g_st.loc_new_locale));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id(PS_KEY "loc_add_locale", "loc_addl"))) {
        if (jce_editor_gl10n_add_locale(g_st.loc_new_locale))
            g_st.loc_new_locale[0] = '\0';
    }

    if (nloc == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n_or(PS_KEY "loc_no_locales",
            "No locale files yet — add a locale to create "
            "i18n/<code>.json."));
        return;
    }

    /* ── Key × locale grid ───────────────────────────────────────────── */
    char pending_delete[128] = {0};
    if (ImGui::BeginTable("##loc_grid", 1 + nloc,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                          ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, 340))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn(jce_editor_i18n_or(PS_KEY "loc_key_col", "Key"),
                                ImGuiTableColumnFlags_WidthFixed, 220.0f);
        for (int c = 0; c < nloc; ++c)
            ImGui::TableSetupColumn(jce_editor_gl10n_locale_code_at(c),
                                    ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        const int nkeys = jce_editor_gl10n_key_count();
        for (int r = 0; r < nkeys; ++r) {
            const char *key = jce_editor_gl10n_key_at(r);
            if (g_st.loc_filter[0] && !std::strstr(key, g_st.loc_filter))
                continue;
            const bool prot = jce_editor_gl10n_key_protected(key);

            ImGui::TableNextRow();
            ImGui::PushID(key);
            ImGui::TableSetColumnIndex(0);
            if (prot) {
                /* Locked: consumed by the engine's built-in pause/settings
                 * menu (fixed jce_i18n enum) — deletable never. */
                ImGui::TextDisabled("[L] %s", key);
                jce_editor::help_tip(jce_editor_i18n_or(
                    PS_KEY "loc_protected_tip",
                    "Built-in key consumed by the engine pause/settings "
                    "menu — cannot be deleted."));
            } else {
                ImGui::TextUnformatted(key);
                if (ImGui::BeginPopupContextItem("##loc_key_ctx")) {
                    if (ImGui::MenuItem(jce_editor_i18n_or(PS_KEY "loc_delete",
                                                           "Delete")))
                        std::snprintf(pending_delete, sizeof(pending_delete),
                                      "%s", key);
                    ImGui::EndPopup();
                }
            }

            for (int c = 0; c < nloc; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                const char *code = jce_editor_gl10n_locale_code_at(c);
                const char *v    = jce_editor_gl10n_get(code, key);
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", v ? v : "");
                ImGui::PushID(c);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##v", buf, sizeof(buf)))
                    jce_editor_gl10n_set(code, key, buf);
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (pending_delete[0])
        jce_editor_gl10n_remove_key(pending_delete);

    /* Save row. */
    if (ImGui::Button(jce_editor_i18n_id(PS_KEY "loc_save", "loc_save")))
        jce_editor_gl10n_save_all();
    if (jce_editor_gl10n_dirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n_or(PS_KEY "loc_dirty",
                                              "* unsaved string changes"));
    }
}

void draw_project(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.project.lastProject"));
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##last_project", g_st.cfg.last_project,
                         sizeof(g_st.cfg.last_project)))
        mark_dirty();
    ImGui::PopItemWidth();
    ImGui::Spacing();
    ImGui::TextDisabled(jce_editor_i18n("projectSettings.project.recentFmt"),
                        g_st.cfg.recent_count);
    if (ImGui::BeginChild("##recent_list", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < g_st.cfg.recent_count; ++i)
            ImGui::TextUnformatted(g_st.cfg.recent_projects[i]);
    }
    ImGui::EndChild();
    if (ImGui::Button(jce_editor_i18n("projectSettings.project.clearRecent"))) {
        g_st.cfg.recent_count = 0;
        mark_dirty();
    }

    /* ── Active project manifest editor ───────────────────────────── */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or(
        "projectSettings.project.manifestHeader",
        "Active Project (jce_project.json)"));

    const JceProject *jp = jce_editor_project_get();
    if (!jp) {
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "projectSettings.project.noManifest",
            "No project open — File > Open Project to load a jce_project.json."));
        g_st.jp_loaded = false;
        g_st.jp_root_cached[0] = '\0';
        return;
    }

    /* (Re)load editable buffers when the active project changes. */
    const char *jp_root = jp->project_root ? jp->project_root : "";
    if (!g_st.jp_loaded || std::strcmp(jp_root, g_st.jp_root_cached) != 0) {
        std::snprintf(g_st.jp_root_cached, sizeof(g_st.jp_root_cached),
                      "%s", jp_root);
        std::snprintf(g_st.jp_name, sizeof(g_st.jp_name),
                      "%s", jp->name ? jp->name : "");
        std::snprintf(g_st.jp_version, sizeof(g_st.jp_version),
                      "%s", jp->version ? jp->version : "");
        std::snprintf(g_st.jp_sdk_path, sizeof(g_st.jp_sdk_path),
                      "%s", jp->sdk_path ? jp->sdk_path : "");
        std::snprintf(g_st.jp_source_assets, sizeof(g_st.jp_source_assets),
                      "%s", jp->source_assets ? jp->source_assets : "");
        std::snprintf(g_st.jp_cooked_assets, sizeof(g_st.jp_cooked_assets),
                      "%s", jp->cooked_assets ? jp->cooked_assets : "");
        std::snprintf(g_st.jp_startup_scene, sizeof(g_st.jp_startup_scene),
                      "%s", jp->startup_scene ? jp->startup_scene : "");
        g_st.jp_bundles.clear();
        for (int i = 0; i < jp->bundles_count; ++i) {
            if (jp->bundles && jp->bundles[i])
                g_st.jp_bundles.emplace_back(jp->bundles[i]);
        }
        g_st.jp_save_msg[0] = '\0';
        g_st.jp_loaded = true;
    }

    ImGui::TextDisabled("%s", jp_root);
    ImGui::PushItemWidth(-1);
    ImGui::TextUnformatted(jce_editor_i18n_or("projectSettings.project.name", "Name"));
    ImGui::InputText("##jp_name", g_st.jp_name, sizeof(g_st.jp_name));
    ImGui::TextUnformatted(jce_editor_i18n_or("projectSettings.project.version", "Version"));
    ImGui::InputText("##jp_version", g_st.jp_version, sizeof(g_st.jp_version));
    ImGui::PopItemWidth();

    /* SDK paths intentionally stay absolute: projects may live outside
     * the installed SDK and need an unambiguous package root. */
    jce_draw_path_input_folder(
        jce_editor_i18n_or("projectSettings.project.sdkPath",
                           "JCE SDK root (absolute, optional)"),
        g_st.jp_sdk_path, sizeof(g_st.jp_sdk_path));

    /* Picker-driven path fields — no manual typing.  After the OS
     * dialog hands us an absolute path, we collapse it back to a path
     * relative to the project root (or to source_assets for the scene). */
    if (jce_draw_path_input_folder(
            jce_editor_i18n_or("projectSettings.project.sourceAssets",
                               "Source assets dir (relative to project root)"),
            g_st.jp_source_assets, sizeof(g_st.jp_source_assets))) {
        char rel[512];
        jce_editor_path_to_relative_to(rel, sizeof(rel),
                        g_st.jp_source_assets, jp_root);
        std::snprintf(g_st.jp_source_assets,
                      sizeof(g_st.jp_source_assets), "%s", rel);
    }
    if (jce_draw_path_input_folder(
            jce_editor_i18n_or("projectSettings.project.cookedAssets",
                               "Cooked assets dir (cook output, fed to PAK)"),
            g_st.jp_cooked_assets, sizeof(g_st.jp_cooked_assets))) {
        char rel[512];
        jce_editor_path_to_relative_to(rel, sizeof(rel),
                        g_st.jp_cooked_assets, jp_root);
        std::snprintf(g_st.jp_cooked_assets,
                      sizeof(g_st.jp_cooked_assets), "%s", rel);
    }
    {
        const char *src = (g_st.jp_source_assets[0]) ? g_st.jp_source_assets
                                                     : "assets";
        char src_abs[1024];
        if (jp_root[0])
            jce_path_join(src_abs, sizeof(src_abs), jp_root, src);
        else
            std::snprintf(src_abs, sizeof(src_abs), "%s", src);
        if (jce_draw_path_input_file(
                jce_editor_i18n_or("projectSettings.project.startupScene",
                                   "Startup scene (relative to source assets dir)"),
                g_st.jp_startup_scene, sizeof(g_st.jp_startup_scene),
                "Scenes (*.scene *.json);;All Files (*.*)")) {
            char rel[512];
            jce_editor_path_to_relative_to(rel, sizeof(rel),
                            g_st.jp_startup_scene, src_abs);
            std::snprintf(g_st.jp_startup_scene,
                          sizeof(g_st.jp_startup_scene), "%s", rel);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or(
        "projectSettings.project.bundles", "Bundles (.jbundle mounted at runtime)"));
    {
        int remove_idx = -1;
        for (size_t i = 0; i < g_st.jp_bundles.size(); ++i) {
            ImGui::PushID((int)i);
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", g_st.jp_bundles[i].c_str());
            ImGui::PushItemWidth(-90.0f);
            if (ImGui::InputText("##jp_bundle", buf, sizeof(buf)))
                g_st.jp_bundles[i] = buf;
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n_or(
                    "projectSettings.project.bundles.remove", "Remove")))
                remove_idx = (int)i;
            ImGui::PopID();
        }
        if (remove_idx >= 0)
            g_st.jp_bundles.erase(g_st.jp_bundles.begin() + remove_idx);

        /* Picker writes asynchronously to add_buf; when it lands we
         * auto-commit + clear so the user just picks a file and the
         * row appears in the list above. */
        char *add_buf = g_st.jp_bundle_add_buf;
        const size_t add_cap = sizeof(g_st.jp_bundle_add_buf);
        bool picked = jce_draw_path_input_file(
                jce_editor_i18n_or("projectSettings.project.bundles.add",
                                   "Add bundle (.jbundle)"),
                add_buf, add_cap,
                "JCE Bundle (*.jbundle);;All Files (*.*)");
        if (picked && add_buf[0]) {
            char rel[1024];
            jce_editor_path_to_relative_to(rel, sizeof(rel),
                                           add_buf, jp_root);
            g_st.jp_bundles.emplace_back(rel[0] ? rel : add_buf);
            add_buf[0] = '\0';
        }
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "projectSettings.project.bundles.hint",
            "Pick a .jbundle file above; it is added to the list. "
            "Click 'Save jce_project.json' to persist."));
    }

    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n_or(
            "projectSettings.project.saveManifest", "Save jce_project.json"))) {
        std::vector<const char *> ptrs;
        ptrs.reserve(g_st.jp_bundles.size());
        for (auto &s : g_st.jp_bundles) ptrs.push_back(s.c_str());
        /* Apply every field independently so an empty optional value
         * (e.g. version) does not short-circuit the bundle write. */
        bool ok_n = jce_editor_project_update_field("name",          g_st.jp_name);
        bool ok_v = jce_editor_project_update_field("version",       g_st.jp_version);
        bool ok_sdk = jce_editor_project_update_field("sdk_path",    g_st.jp_sdk_path);
        bool ok_s = jce_editor_project_update_field("source_assets", g_st.jp_source_assets);
        bool ok_c = jce_editor_project_update_field("cooked_assets", g_st.jp_cooked_assets);
        bool ok_e = jce_editor_project_update_field("startup_scene", g_st.jp_startup_scene);
        bool ok_b = jce_editor_project_set_bundles(
                        ptrs.empty() ? nullptr : ptrs.data(), (int)ptrs.size());
        bool ok = ok_n && ok_v && ok_sdk && ok_s && ok_c && ok_e && ok_b;
        jce_log_write(JCE_LOG_LEVEL_INFO, "project_settings", __FILE__, __LINE__,
                     "save: name=%d ver=%d sdk=%d src=%d cooked=%d scene=%d bundles=%d (%zu)",
                     (int)ok_n, (int)ok_v, (int)ok_sdk, (int)ok_s, (int)ok_c,
                     (int)ok_e, (int)ok_b, g_st.jp_bundles.size());
        std::snprintf(g_st.jp_save_msg, sizeof(g_st.jp_save_msg),
                      "%s", ok ? "Saved." : "Save failed (see console).");
        /* Force re-read of buffers from refreshed cache next frame. */
        g_st.jp_loaded = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or(
            "projectSettings.project.revertManifest", "Revert"))) {
        g_st.jp_loaded = false;
        g_st.jp_save_msg[0] = '\0';
    }
    if (g_st.jp_save_msg[0])
        ImGui::TextDisabled("%s", g_st.jp_save_msg);

    /* ── main.c maintenance (Reset / Eject) ───────────────────────── */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or(
        "projectSettings.project.mainC", "Application entry point (src/main.c)"));
    ImGui::TextDisabled("%s", jce_editor_i18n_or(
        "projectSettings.project.mainC.hint",
        "Reset: re-write src/main.c from the SDK template "
        "(thin shim that tracks engine upgrades). "
        "Eject: inline the default body so you can deeply customise "
        "boot flow (stops tracking engine upgrades for this file)."));

    static char s_main_msg[256] = {0};
    const JceProject *cur = jce_editor_project_get();
    const bool        have_project = (cur && cur->project_root && *cur->project_root);

    if (!have_project) ImGui::BeginDisabled();
    if (ImGui::Button(jce_editor_i18n_or(
            "projectSettings.project.resetMain", "Reset main.c"))) {
        char err[256] = {0};
        bool ok = jce_project_reset_main_c(
                      cur ? cur->project_root : "",
                      cur ? cur->name         : nullptr,
                      err, sizeof err);
        std::snprintf(s_main_msg, sizeof s_main_msg, "%s",
                      ok ? jce_editor_i18n_or("projectSettings.project.resetOk",
                                "Reset OK — rebuild to apply.") :
                           (err[0] ? err :
                            jce_editor_i18n_or("projectSettings.project.resetFailed",
                                 "Reset failed.")));
        jce_log_write(ok ? JCE_LOG_LEVEL_INFO : JCE_LOG_LEVEL_ERROR,
                      "project_settings", __FILE__, __LINE__,
                      "reset main.c: %s", s_main_msg);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or(
            "projectSettings.project.ejectMain", "Eject main.c"))) {
        char err[256] = {0};
        bool ok = jce_project_eject_main_c(
                      cur ? cur->project_root : "",
                      cur ? cur->name         : nullptr,
                      err, sizeof err);
        std::snprintf(s_main_msg, sizeof s_main_msg, "%s",
                      ok ? jce_editor_i18n_or("projectSettings.project.ejectOk",
                                "Ejected — main.c now owned by this project.") :
                           (err[0] ? err :
                            jce_editor_i18n_or("projectSettings.project.ejectFailed",
                                 "Eject failed.")));
        jce_log_write(ok ? JCE_LOG_LEVEL_INFO : JCE_LOG_LEVEL_ERROR,
                      "project_settings", __FILE__, __LINE__,
                      "eject main.c: %s", s_main_msg);
    }
    if (!have_project) ImGui::EndDisabled();

    if (s_main_msg[0]) ImGui::TextDisabled("%s", s_main_msg);
}

void draw_build(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.cfgPreset"));
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText("##cfg_preset", g_st.cfg.build_configure_preset,
                         sizeof(g_st.cfg.build_configure_preset)))
        mark_dirty();
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.buildPreset"));
    if (ImGui::InputText("##build_preset", g_st.cfg.build_preset,
                         sizeof(g_st.cfg.build_preset)))
        mark_dirty();
    ImGui::PopItemWidth();
    if (jce_draw_path_input_folder(
            jce_editor_i18n("projectSettings.build.outputDir"),
            g_st.cfg.build_output_path,
            sizeof(g_st.cfg.build_output_path)))
        mark_dirty();
    ImGui::PushItemWidth(-1);
    ImGui::TextUnformatted(jce_editor_i18n("projectSettings.build.cmakeTarget"));
    if (ImGui::InputText("##target", g_st.cfg.game_target_name,
                         sizeof(g_st.cfg.game_target_name)))
        mark_dirty();
    ImGui::PopItemWidth();
}

void draw_run(void)
{
    const char *modes[2] = {
        jce_editor_i18n_or("projectSettings.run.mode.editorSim",  "Editor Simulation"),
        jce_editor_i18n_or("projectSettings.run.mode.subprocess", "External Game (subprocess)"),
    };
    int mode = g_st.cfg.run_mode;
    if (mode < 0 || mode > 1) mode = 0;
    if (ImGui::Combo(jce_editor_i18n_id("projectSettings.run.mode", "run_mode"),
                     &mode, modes, 2)) {
        g_st.cfg.run_mode = mode;
        mark_dirty();
    }
    ImGui::Spacing();
    if (jce_draw_path_input_file(
            jce_editor_i18n("projectSettings.run.gameExe"),
            g_st.cfg.game_executable_path,
            sizeof(g_st.cfg.game_executable_path),
#if JCE_PLATFORM_WINDOWS
            jce_editor_i18n_or("fileDialog.filter.executables",
                 "Executables (*.exe);;All Files (*.*)")
#else
            jce_editor_i18n_or("fileDialog.filter.allFiles", "All Files (*.*)")
#endif
            ))
        mark_dirty();
    if (jce_draw_path_input_folder(
            jce_editor_i18n("projectSettings.run.workDir"),
            g_st.cfg.game_working_directory,
            sizeof(g_st.cfg.game_working_directory)))
        mark_dirty();
}

void draw_hotkeys(void)
{
    ImGui::TextWrapped("%s", jce_editor_i18n("projectSettings.hotkeys.intro"));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##hkfilter",
                             jce_editor_i18n("projectSettings.hotkeys.search"),
                             g_st.hk_filter, sizeof(g_st.hk_filter));
    ImGui::Spacing();
    if (ImGui::BeginTable("##hktab", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.hotkeys.colAction"),
                                ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.hotkeys.colChord"),
                                ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < JCE_HK_COUNT; ++i) {
            const char *name = jce_hotkey_name((JceHotkeyId)i);
            if (g_st.hk_filter[0]) {
                char lname[128]; char lflt[64];
                int n = (int)std::strlen(name);
                for (int j = 0; j < n && j < 127; ++j)
                    lname[j] = (char)std::tolower((unsigned char)name[j]);
                lname[n < 127 ? n : 127] = '\0';
                int m = (int)std::strlen(g_st.hk_filter);
                for (int j = 0; j < m && j < 63; ++j)
                    lflt[j] = (char)std::tolower((unsigned char)g_st.hk_filter[j]);
                lflt[m < 63 ? m : 63] = '\0';
                if (!std::strstr(lname, lflt)) continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            char chord_buf[64];
            jce_hotkey_chord_label(jce_hotkey_get((JceHotkeyId)i),
                                   chord_buf, sizeof(chord_buf));
            bool is_recording = (g_st.hk_recording_id == i);
            if (is_recording) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                                   jce_editor_i18n("projectSettings.hotkeys.recording"));
                ImGuiIO &io = ImGui::GetIO();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    g_st.hk_recording_id = -1;
                } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                    JceHotkeyChord c = {0, 0};
                    jce_hotkey_set((JceHotkeyId)i, c);
                    g_st.hk_recording_id = -1;
                    mark_dirty();
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
                            g_st.hk_recording_id = -1;
                            mark_dirty();
                            break;
                        }
                    }
                }
            } else {
                if (ImGui::Selectable(chord_buf, false,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    g_st.hk_recording_id = i;
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(jce_editor_i18n("projectSettings.hotkeys.reset"))) {
                jce_hotkey_set((JceHotkeyId)i,
                               jce_hotkey_get_default((JceHotkeyId)i));
                mark_dirty();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("projectSettings.hotkeys.resetAll"))) {
        jce_hotkeys_reset_all();
        mark_dirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("projectSettings.hotkeys.persistsTo"));
}

void draw_preset_manager(void)
{
    JceProjectPresetManager &pm = g_st.ps.presets;
    ImGui::Text("%s %d / %d",
                jce_editor_i18n("projectSettings.presetManager.bindings"),
                pm.count, JCE_PS_MAX_PRESET_BINDINGS);
    if (ImGui::Button(jce_editor_i18n("projectSettings.presetManager.addBinding")) &&
        pm.count < JCE_PS_MAX_PRESET_BINDINGS) {
        std::memset(&pm.bindings[pm.count], 0, sizeof(pm.bindings[0]));
        pm.count++;
        mark_dirty();
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("##pm", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.componentType"),
                                ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.presetPath"),
                                ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(jce_editor_i18n("projectSettings.presetManager.filter"),
                                ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        int remove_idx = -1;
        for (int i = 0; i < pm.count; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##ct", pm.bindings[i].component_type, JCE_PS_NAME_LEN))
                mark_dirty();
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (jce_draw_path_input_asset("##pp", pm.bindings[i].preset_path, JCE_PS_PATH_LEN))
                mark_dirty();
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##ft", pm.bindings[i].filter, JCE_PS_NAME_LEN))
                mark_dirty();
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton(jce_editor_i18n("projectSettings.common.remove")))
                remove_idx = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove_idx >= 0) {
            for (int i = remove_idx; i < pm.count - 1; ++i)
                pm.bindings[i] = pm.bindings[i + 1];
            pm.count--;
            mark_dirty();
        }
    }
}

void draw_quality_levels(void)
{
    JceProjectQuality &q = g_st.ps.quality;
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or(PS_KEY "quality.perLevel", "Per-Level Editor"));
    if (ImGui::Button(jce_editor_i18n("projectSettings.quality.addLevel")) &&
        q.count < JCE_PS_MAX_QUALITY_LEVELS) {
        std::snprintf(q.levels[q.count].name, JCE_PS_NAME_LEN, "Level %d", q.count);
        q.levels[q.count].vsync_count      = 1;
        q.levels[q.count].target_framerate = -1;
        q.levels[q.count].lod_bias         = 1.0f;
        q.levels[q.count].shadow_distance  = 50.0f;
        q.count++;
        mark_dirty();
    }
    ImGui::SameLine();
    ImGui::Text("(%d / %d)", q.count, JCE_PS_MAX_QUALITY_LEVELS);

    if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.currentLevel"),
                     &q.current_level,
                     [](void *u, int i, const char **o) -> bool {
                         auto *qq = (JceProjectQuality *)u;
                         if (i < 0 || i >= qq->count) return false;
                         *o = qq->levels[i].name; return true;
                     }, &q, q.count))
        mark_dirty();

    if (ImGui::BeginTabBar("##qlvls")) {
        for (int i = 0; i < q.count; ++i) {
            ImGui::PushID(i);
            char tab[80]; std::snprintf(tab, sizeof(tab), "%s##t", q.levels[i].name);
            if (ImGui::BeginTabItem(tab)) {
                JceProjectQualityLevel *lv = &q.levels[i];
                if (ImGui::InputText(jce_editor_i18n("projectSettings.common.name"),
                                     lv->name, JCE_PS_NAME_LEN)) mark_dirty();
                if (ImGui::SliderInt(jce_editor_i18n("projectSettings.quality.pixelLightCount"),
                                     &lv->pixel_light_count, 0, 16)) mark_dirty();
                const char *tx[] = {
                    jce_editor_i18n_or(PS_KEY "texq.full",    "Full"),
                    jce_editor_i18n_or(PS_KEY "texq.half",    "Half"),
                    jce_editor_i18n_or(PS_KEY "texq.quarter", "Quarter"),
                    jce_editor_i18n_or(PS_KEY "texq.eighth",  "Eighth"),
                };
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.textureQuality"),
                                 &lv->texture_quality, tx, 4)) mark_dirty();
                const char *aniso[] = {
                    jce_editor_i18n_or(PS_KEY "aniso.disabled",   "Disabled"),
                    jce_editor_i18n_or(PS_KEY "aniso.perTexture", "Per Texture"),
                    jce_editor_i18n_or(PS_KEY "aniso.forced",     "Forced On"),
                };
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.anisotropic"),
                                 &lv->anisotropic, aniso, 3)) mark_dirty();
                const char *aa[] = { jce_editor_i18n_or(PS_KEY "msaa.off", "Off"), "2x", "4x", "8x" };
                int aa_i = (lv->anti_aliasing <= 0) ? 0 :
                           (lv->anti_aliasing == 2) ? 1 :
                           (lv->anti_aliasing == 4) ? 2 : 3;
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.antiAliasing"),
                                 &aa_i, aa, 4)) {
                    const int v[] = { 0, 2, 4, 8 };
                    lv->anti_aliasing = v[aa_i]; mark_dirty();
                }
                if (ImGui::Checkbox(jce_editor_i18n("projectSettings.quality.softParticles"),
                                    &lv->soft_particles)) mark_dirty();
                if (ImGui::Checkbox(jce_editor_i18n("projectSettings.quality.realtimeReflectionProbes"),
                                    &lv->realtime_reflection_probes)) mark_dirty();
                const char *sq[] = {
                    jce_editor_i18n_or(PS_KEY "shadowq.disable",  "Disable"),
                    jce_editor_i18n_or(PS_KEY "shadowq.hardOnly", "Hard Only"),
                    jce_editor_i18n_or(PS_KEY "shadowq.hardSoft", "Hard + Soft"),
                };
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowQuality"),
                                 &lv->shadow_quality, sq, 3)) mark_dirty();
                const char *sr[] = {
                    jce_editor_i18n_or(PS_KEY "tier.low",    "Low"),
                    jce_editor_i18n_or(PS_KEY "tier.medium", "Medium"),
                    jce_editor_i18n_or(PS_KEY "tier.high",   "High"),
                    jce_editor_i18n_or(PS_KEY "shadowres.veryHigh", "Very High"),
                };
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowResolution"),
                                 &lv->shadow_resolution, sr, 4)) mark_dirty();
                if (ImGui::DragFloat(jce_editor_i18n("projectSettings.quality.shadowDistance"),
                                     &lv->shadow_distance, 1.0f, 0.0f, 5000.0f)) mark_dirty();
                const char *sc[] = { "1", "2", "4" };
                int sc_i = (lv->shadow_cascades <= 1) ? 0 :
                           (lv->shadow_cascades == 2) ? 1 : 2;
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.shadowCascades"),
                                 &sc_i, sc, 3)) {
                    const int v[] = { 1, 2, 4 };
                    lv->shadow_cascades = v[sc_i]; mark_dirty();
                }
                const char *vs[] = {
                    jce_editor_i18n_or(PS_KEY "msaa.off", "Off"),
                    jce_editor_i18n_or(PS_KEY "vsync.everyVblank", "Every VBlank"),
                    jce_editor_i18n_or(PS_KEY "vsync.every2nd",    "Every 2nd VBlank"),
                };
                if (ImGui::Combo(jce_editor_i18n("projectSettings.quality.vsync"),
                                 &lv->vsync_count, vs, 3)) mark_dirty();
                if (ImGui::DragInt(jce_editor_i18n("projectSettings.quality.targetFramerate"),
                                   &lv->target_framerate, 1, -1, 480)) mark_dirty();
                if (ImGui::DragFloat(jce_editor_i18n("projectSettings.quality.lodBias"),
                                     &lv->lod_bias, 0.05f, 0.1f, 10.0f)) mark_dirty();
                if (ImGui::SmallButton(jce_editor_i18n("projectSettings.quality.deleteLevel")) &&
                    q.count > 1) {
                    for (int j = i; j < q.count - 1; ++j) q.levels[j] = q.levels[j + 1];
                    q.count--;
                    if (q.current_level >= q.count) q.current_level = q.count - 1;
                    mark_dirty();
                }
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
}

/* ── Packaging (asset encryption) ──────────────────────────────────── */

void draw_packaging(void)
{
    JceProjectPackaging &pk = g_st.ps.packaging;
    const char *proj = jce_editor_assets_get_project();
    const std::string root = (proj && proj[0]) ? proj : "";

    ImGui::TextWrapped("%s", jce_editor_i18n_or(
        "projectSettings.packaging.hint",
        "Encrypt the embedded asset PAK and bundle payloads (ChaCha20). "
        "This deters casual extraction; the key ships inside the game "
        "binary, so it is obfuscation, not secrecy."));
    ImGui::Spacing();

    if (ImGui::Checkbox(jce_editor_i18n_or(
            "projectSettings.packaging.encryptAssets", "Encrypt assets"),
            &pk.encrypt_assets)) {
        mark_dirty();
        /* First enable: generate the key on the spot so the user sees the
         * fingerprint immediately. */
        if (pk.encrypt_assets && !root.empty() && !jce_pak_key_exists(root)) {
            std::string err;
            if (!jce_pak_key_generate(root, false, &err))
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "[pak_key] generate failed: %s", err.c_str());
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n_or(
            "projectSettings.packaging.encryptAssets.tip",
            "Deters casual extraction; the key ships inside the game binary."));

    ImGui::BeginDisabled(!pk.encrypt_assets);
    if (ImGui::Checkbox(jce_editor_i18n_or(
            "projectSettings.packaging.encryptDebug", "Also encrypt debug builds"),
            &pk.encrypt_debug_builds))
        mark_dirty();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or(
        "projectSettings.packaging.keyStatus", "Encryption key"));

    if (root.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "projectSettings.packaging.noProject", "(no project open)"));
        return;
    }

    uint64_t fp = 0;
    const bool have_key = jce_pak_key_fingerprint_of(root, &fp);
    if (have_key) {
        char fphex[24];
        std::snprintf(fphex, sizeof(fphex), "%08x",
                      (unsigned)(fp >> 32));
        ImGui::Text("%s: %s", jce_editor_i18n_or(
            "projectSettings.packaging.fingerprint", "Key fingerprint"), fphex);
        ImGui::SameLine();
        ImGui::TextDisabled("(.jce/pak_key.hex)");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
            jce_editor_i18n_or("projectSettings.packaging.noKey",
                               "No key generated yet"));
    }

    static char s_import_buf[1024] = {0};
    static bool s_confirm_regen = false;

    if (!have_key) {
        if (ImGui::Button(jce_editor_i18n_or(
                "projectSettings.packaging.generate", "Generate Key"))) {
            std::string err;
            if (!jce_pak_key_generate(root, false, &err))
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "[pak_key] generate failed: %s", err.c_str());
        }
    } else {
        if (ImGui::Button(jce_editor_i18n_or(
                "projectSettings.packaging.regenerate", "Regenerate Key")))
            s_confirm_regen = true;
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n_or(
                "projectSettings.packaging.showInFolder", "Show in Folder")))
            jce_host_reveal_path(jce_pak_key_path(root).c_str());
    }

    /* Regenerate confirmation: old encrypted archives become unreadable. */
    if (s_confirm_regen)
        ImGui::OpenPopup("###pak_key_regen");
    char regen_title[160];
    std::snprintf(regen_title, sizeof(regen_title), "%s###pak_key_regen",
                  jce_editor_i18n_or("projectSettings.packaging.regenTitle",
                                     "Regenerate encryption key?"));
    if (ImGui::BeginPopupModal(regen_title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", jce_editor_i18n_or(
            "projectSettings.packaging.regenWarn",
            "Bundles and builds encrypted with the OLD key will become "
            "unreadable. Rebuild all bundles after regenerating."));
        ImGui::Spacing();
        if (ImGui::Button(jce_editor_i18n_or(
                "projectSettings.packaging.regenConfirm", "Regenerate"),
                ImVec2(120, 0))) {
            std::string err;
            if (jce_pak_key_generate(root, true, &err))
                jce_pak_key_install_process(root);
            else
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "[pak_key] regenerate failed: %s", err.c_str());
            s_confirm_regen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n_or("dialog.cancel", "Cancel"),
                          ImVec2(120, 0))) {
            s_confirm_regen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    jce_draw_path_input_file(jce_editor_i18n_or(
            "projectSettings.packaging.importPath", "Import key file"),
        s_import_buf, sizeof(s_import_buf),
        jce_editor_i18n_or("fileDialog.filter.keyFile", "Key (*.hex *.txt);;All Files (*.*)"));
    ImGui::SameLine();
    ImGui::BeginDisabled(s_import_buf[0] == '\0');
    if (ImGui::Button(jce_editor_i18n_or(
            "projectSettings.packaging.import", "Import"))) {
        std::string err;
        if (jce_pak_key_import(root, s_import_buf, &err)) {
            jce_pak_key_install_process(root);
            s_import_buf[0] = '\0';
        } else {
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "[pak_key] import failed: %s", err.c_str());
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("%s", jce_editor_i18n_or(
        "projectSettings.packaging.honesty",
        "Note: no integrity protection (MAC). Keep .jce/pak_key.hex out of "
        "version control; losing it makes encrypted archives unreadable."));
}

void draw_tab_content(int t)
{
    switch (t) {
        case TAB_PROJECT:        draw_project();        break;
        case TAB_BUILD:          draw_build();          break;
        case TAB_RUN:            draw_run();            break;
        case TAB_HOTKEYS:        draw_hotkeys();        break;
        case TAB_INPUT:          draw_input();          break;
        case TAB_PHYSICS:        draw_physics();        break;
        case TAB_PHYSICS2D:      draw_physics2d();      break;
        case TAB_TAGS_LAYERS:    draw_tags_layers();    break;
        case TAB_QUALITY:        draw_quality();
                                 draw_quality_levels(); break;
        case TAB_GRAPHICS:       draw_graphics();       break;
        case TAB_TIME:           draw_time();           break;
        case TAB_PLAYER:         draw_player();         break;
        case TAB_PACKAGING:      draw_packaging();      break;
        case TAB_PRESET_MANAGER: draw_preset_manager(); break;
        case TAB_AUDIO:          draw_audio();          break;
        case TAB_EDITOR:         draw_editor();         break;
        case TAB_LOCALIZATION:   draw_localization();   break;
        default:                 break;
    }
}

bool tab_matches_filter(int t)
{
    if (g_st.filter[0] == '\0') return true;
    const char *label = jce_editor_i18n_or(tab_i18n_key(t), tab_fallback(t));
    /* Case-insensitive contains. */
    char a[64]; char b[64];
    size_t la = std::strlen(g_st.filter);
    size_t lb = std::strlen(label);
    if (la >= sizeof(a) || lb >= sizeof(b)) return true;
    for (size_t i = 0; i <= la; ++i)
        a[i] = (char)std::tolower((unsigned char)g_st.filter[i]);
    for (size_t i = 0; i <= lb; ++i)
        b[i] = (char)std::tolower((unsigned char)label[i]);
    return std::strstr(b, a) != nullptr;
}

} /* namespace */

extern "C" void jce_editor_panel_project_settings_content(void)
{
    ensure_loaded();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float left_w = 180.0f;

    /* ── Left pane: tab list ───────────────────────────────────────── */
    ImGui::BeginChild("##ps_left", ImVec2(left_w, avail.y), true);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ps_filter",
                             jce_editor_i18n_or("panel.project_settings.filter_hint", "Filter..."),
                             g_st.filter, sizeof(g_st.filter));
    ImGui::Separator();
    for (int t = 0; t < TAB_COUNT; ++t) {
        if (!tab_matches_filter(t)) continue;
        const char *label = jce_editor_i18n_or(tab_i18n_key(t),
                                               tab_fallback(t));
        char row[96];
        std::snprintf(row, sizeof(row), "  %s", label);
        if (ImGui::Selectable(row, g_st.tab == t))
            g_st.tab = t;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    /* ── Right pane: tab content ───────────────────────────────────── */
    ImGui::BeginChild("##ps_right", ImVec2(0, avail.y), false);
    const char *title = jce_editor_i18n_or(tab_i18n_key(g_st.tab),
                                           tab_fallback(g_st.tab));
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    ImGui::BeginChild("##ps_body", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                      false);
    draw_tab_content(g_st.tab);
    ImGui::EndChild();

    /* Footer: dirty indicator + Save / Revert. */
    if (g_st.dirty)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n_or("panel.project_settings.unsaved", "* unsaved"));
    else
        ImGui::TextDisabled("%s",
                            jce_editor_i18n_or("panel.project_settings.saved", "saved"));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or("panel.project_settings.save", "Save"), ImVec2(80, 0)))
        save_if_dirty();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or("panel.project_settings.revert", "Revert"),
                      ImVec2(80, 0))) {
        g_st.loaded = false;
        ensure_loaded();
    }
    ImGui::EndChild();
}

extern "C" void jce_editor_panel_project_settings(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROJECT_SETTINGS);
    if (!vis) return;

    /* P8-C: Project Settings is a modal popup (non-dockable). Visibility
     * toggle (menu / hotkey / search) requests OpenPopup once. */
    static bool s_modal_open = false;
    if (*vis) {
        s_modal_open = true;
        *vis = false;
        /* Re-sync the cached project-settings + editor-config snapshots from
         * disk on EVERY open (mirrors the Preferences panel), so g_st.cfg is
         * never a session-stale snapshot whose whole-struct save would revert
         * run_mode / exe path / prefs changed by other surfaces since the
         * first open.  This is the primary fix for the build/run clobber. */
        g_st.loaded = false;
        ensure_loaded();
        ImGui::OpenPopup("###project_settings");
    }
    if (!s_modal_open) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 size(820.0f, 560.0f);
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
               vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    char title[96];
    std::snprintf(title, sizeof(title), "%s###project_settings",
                  jce_editor_i18n_or("panel.project_settings.title",
                                     "Project Settings"));
    if (ImGui::BeginPopupModal(title, &s_modal_open,
                               ImGuiWindowFlags_NoDocking |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoCollapse)) {
        jce_editor_panel_project_settings_content();
        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n_or("dialog.close", "Close"),
                          ImVec2(120, 0))) {
            s_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

extern "C" void jce_editor_project_settings_focus_tab_physics(void)
{
    g_st.tab = TAB_PHYSICS;
}
