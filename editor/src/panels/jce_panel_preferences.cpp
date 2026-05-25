/*
 * jce_panel_preferences.cpp  User-level Preferences panel (P4-A.2).
 *
 * Unity-style two-pane layout (vertical tab list on the left, page
 * content on the right) for user-scoped editor settings:
 *
 *   General      autosave interval, startup behaviour, recent count
 *   Appearance   theme, font size, UI scale
 *   Hotkeys      placeholder — wired in P4-A.3
 *
 * Persistence
 * -----------
 * Settings are mirrored to "<cwd>/.jce/prefs.json" using cJSON via
 * <jce/os/core/jce_json.h> and the editor's existing host-path file
 * helpers.  The engine does not currently expose an "OS user pref dir"
 * accessor (no SDL_GetPrefPath wrapper in api_core.h), and the rest of
 * the editor — hotkeys.json, editor-config.json — already follows the
 * ".jce/<file>.json" convention next to the project.  We stay
 * consistent with that until a dedicated user-dir helper lands.
 *
 * Theme / font / UI scale are applied to the live ImGui context the
 * frame the user toggles them, then flushed to disk.
 */

#include "panels/jce_panel_preferences.h"

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_style.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_config.h"
#include "core/jce_hotkeys.h"
#include "dialogs/jce_path_input.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/api_core.h>
#include <jce/api_graphics.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
}

#define LOG_TAG "panel_prefs"

namespace {

/* ── On-disk schema ────────────────────────────────────────────────── */

enum AutosaveInterval {
    AUTOSAVE_OFF = 0,
    AUTOSAVE_1MIN,
    AUTOSAVE_5MIN,
    AUTOSAVE_15MIN,
    AUTOSAVE_COUNT
};

enum StartupBehavior {
    STARTUP_LAST = 0,
    STARTUP_EMPTY,
    STARTUP_PICKER,
    STARTUP_COUNT
};

/* Theme indices map 1:1 to the canonical jce_editor_apply_theme() values
 * (JCE_THEME_DARK=0, JCE_THEME_LIGHT=1, JCE_THEME_SSMS=2). The on-disk
 * label "Blue" maps to JCE_THEME_SSMS — kept for legacy compatibility
 * with the older Preferences dialog. */
struct UserPrefs {
    /* Truly new fields persisted to .jce/prefs.json. */
    int   autosave        = AUTOSAVE_5MIN;
    int   startup         = STARTUP_LAST;
    int   recent_max      = 10;
};

/* Theme/font/ui_scale/renderer are NOT duplicated here — they live in
 * the canonical JceEditorConfig (.jce/editor-config.json) so this panel
 * and the legacy Preferences dialog stay in sync. */

UserPrefs        s_prefs;
JceEditorConfig  s_cfg;       /* mirror of editor-config for live editing */
bool             s_loaded = false;
int              s_active_tab = 0;   /* 0=General, 1=Appearance, 2=Fonts,
                                        3=Viewport, 4=Input, 5=Paths, 6=Hotkeys */

constexpr const char *PREFS_PATH = ".jce/prefs.json";

int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float clamp_float(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ── Live apply ────────────────────────────────────────────────────── */

/* String <-> theme index — matches the legacy dialog's normalization. */
int theme_str_to_idx(const char *s)
{
    if (!s || !*s) return JCE_THEME_DARK;
    if (jce_strcasecmp(s, "Light") == 0) return JCE_THEME_LIGHT;
    if (jce_strcasecmp(s, "Blue")  == 0) return JCE_THEME_SSMS;
    if (jce_strcasecmp(s, "SSMS")  == 0) return JCE_THEME_SSMS;
    return JCE_THEME_DARK;
}

const char *theme_idx_to_str(int idx)
{
    switch (idx) {
    case JCE_THEME_LIGHT: return "Light";
    case JCE_THEME_SSMS:  return "Blue";
    default:              return "Dark";
    }
}

void apply_font_size(int sz)
{
    /* LIVE PREVIEW (no atlas re-bake): bump FontSizeBase so glyphs
     * rendered next frame use the new logical size via ImGui 1.92's
     * dynamic rasterizer, AND mirror the change as a transient
     * FontScaleMain proxy so the UI visibly grows/shrinks instantly
     * even on builds where dynamic re-bake hasn't kicked in yet (the
     * atlas might not contain a glyph baked at exactly that size).
     * FontScaleMain composes cheaply with the user's ui_scale.
     *
     * The full atlas rebuild (jce_editor_request_font_reload) is
     * deferred to commit_font_size() — only fired once when the
     * slider is released — so dragging stays frame-perfect smooth
     * while still ending on a crisp re-bake. */
    ImGui::GetStyle().FontSizeBase = (float)sz;

    float baked = jce_editor_get_baked_font_size();
    if (baked > 0.0f) {
        ImGui::GetStyle().FontScaleMain =
            s_cfg.ui_scale * ((float)sz / baked);
    }
}

/* Commit the final font size after the slider is released — does a
 * full atlas re-bake at the new base size so glyphs are sharp at
 * the user's chosen scale, and resets FontScaleMain back to the
 * user's ui_scale (no longer needs the live-preview proxy). */
void commit_font_size(int sz)
{
    ImGui::GetStyle().FontScaleMain = s_cfg.ui_scale;
    jce_editor_request_font_reload((float)sz,
                                   s_cfg.font_en_path,
                                   s_cfg.font_zh_path);
}

void apply_ui_scale(float s)
{
    /* ImGui 1.92: prefer style.FontScaleMain over the legacy
     * io.FontGlobalScale (which is being phased out and only scales
     * text — not paddings derived from FontSize via dynamic baking). */
    ImGui::GetStyle().FontScaleMain = s;
    ImGui::GetIO().FontGlobalScale = 1.0f;
}

void apply_all()
{
    jce_editor_apply_theme(theme_str_to_idx(s_cfg.theme));
    apply_ui_scale(s_cfg.ui_scale);
    /* Font size applies on next frame via the deferred font reload. */
}

/* ── Disk I/O ──────────────────────────────────────────────────────── */

void load_from_disk()
{
    JceJson *root = jce_json_parse_file(PREFS_PATH);
    if (!root) return;

    s_prefs.autosave   = clamp_int(jce_json_get_int(root, "autosave",   s_prefs.autosave),
                                   0, AUTOSAVE_COUNT - 1);
    s_prefs.startup    = clamp_int(jce_json_get_int(root, "startup",    s_prefs.startup),
                                   0, STARTUP_COUNT - 1);
    s_prefs.recent_max = clamp_int(jce_json_get_int(root, "recent_max", s_prefs.recent_max),
                                   1, 20);
    jce_json_free(root);
}

void save_to_disk()
{
    /* Ensure the .jce directory exists (host_write_all also creates it
     * but we keep this explicit for clarity). */
    jce_fs_host_create_directory(".jce");

    JceJson *root = jce_json_object();
    if (!root) return;
    jce_json_set_int   (root, "autosave",   s_prefs.autosave);
    jce_json_set_int   (root, "startup",    s_prefs.startup);
    jce_json_set_int   (root, "recent_max", s_prefs.recent_max);
    jce_json_write_file(PREFS_PATH, root, /*pretty=*/true,
                        /*take_ownership=*/true);
}

void ensure_loaded()
{
    if (s_loaded) return;
    /* New-fields prefs first, then mirror canonical editor config. */
    load_from_disk();
    if (!jce_editor_config_load(&s_cfg))
        jce_editor_config_defaults(&s_cfg);
    s_loaded = true;
}

/* ── UI helpers ────────────────────────────────────────────────────── */

const char *autosave_label(int v)
{
    switch (v) {
    case AUTOSAVE_OFF:   return jce_editor_i18n("panel.preferences.autosave.off");
    case AUTOSAVE_1MIN:  return jce_editor_i18n("panel.preferences.autosave.1min");
    case AUTOSAVE_5MIN:  return jce_editor_i18n("panel.preferences.autosave.5min");
    case AUTOSAVE_15MIN: return jce_editor_i18n("panel.preferences.autosave.15min");
    default:             return "?";
    }
}

const char *startup_label(int v)
{
    switch (v) {
    case STARTUP_LAST:   return jce_editor_i18n("panel.preferences.startup.last");
    case STARTUP_EMPTY:  return jce_editor_i18n("panel.preferences.startup.empty");
    case STARTUP_PICKER: return jce_editor_i18n("panel.preferences.startup.picker");
    default:             return "?";
    }
}

const char *theme_label_i18n(int v)
{
    switch (v) {
    case JCE_THEME_DARK:  return jce_editor_i18n("panel.preferences.theme.dark");
    case JCE_THEME_LIGHT: return jce_editor_i18n("panel.preferences.theme.light");
    case JCE_THEME_SSMS:  return jce_editor_i18n("panel.preferences.theme.blue");
    default:              return "?";
    }
}

template <typename LabelFn>
bool combo_enum(const char *label, int *v, int count, LabelFn label_for)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, label_for(*v))) {
        for (int i = 0; i < count; ++i) {
            bool sel = (*v == i);
            if (ImGui::Selectable(label_for(i), sel)) {
                if (*v != i) { *v = i; changed = true; }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void draw_tab_general()
{
    bool dirty = false;
    dirty |= combo_enum(jce_editor_i18n("panel.preferences.autosave_interval"),
                        &s_prefs.autosave, AUTOSAVE_COUNT, autosave_label);
    dirty |= combo_enum(jce_editor_i18n("panel.preferences.startup"),
                        &s_prefs.startup, STARTUP_COUNT, startup_label);
    if (ImGui::SliderInt(jce_editor_i18n("panel.preferences.recent_max"),
                         &s_prefs.recent_max, 1, 20)) {
        s_prefs.recent_max = clamp_int(s_prefs.recent_max, 1, 20);
        dirty = true;
    }
    if (dirty) save_to_disk();
}

void draw_tab_appearance()
{
    bool prefs_dirty = false;
    bool cfg_dirty   = false;

    /* Language — picker is data-driven: it walks every installed
     * locale via jce_editor_i18n_locale_count() and asks each one for
     * its own "language.native_name" string. Adding a new locale only
     * requires (a) a JSON file in assets/i18n/ and (b) one enum entry
     * + filename in jce_editor_i18n.cpp — no edits to this panel. */
    {
        const int  n_loc   = jce_editor_i18n_locale_count();
        const int  cur_loc = (int)jce_editor_i18n_get_locale();
        const char *cur_label = jce_editor_i18n_lookup_locale(
            (JceLocale)cur_loc, "language.native_name");
        if (!cur_label) cur_label = "?";

        if (ImGui::BeginCombo(jce_editor_i18n("preferences.language"),
                              cur_label)) {
            for (int i = 0; i < n_loc; ++i) {
                const char *lbl = jce_editor_i18n_lookup_locale(
                    (JceLocale)i, "language.native_name");
                if (!lbl || !*lbl) lbl = "?";
                /* Defensive: append a per-index suffix so any future
                 * collision between two locales sharing the same
                 * native_name (e.g. en.json key missing in another
                 * locale and falling back to "English") can't ever
                 * blow up ImGui's "2 visible items with conflicting
                 * ID" assert. */
                char unique[96];
                std::snprintf(unique, sizeof(unique), "%s##loc_%d",
                              lbl, i);
                bool sel = (i == cur_loc);
                if (ImGui::Selectable(unique, sel) && i != cur_loc) {
                    jce_editor_i18n_set_locale((JceLocale)i);
                    /* Persist the new locale into editor-config.json so it
                       survives a restart. Without this the runtime switch
                       happens but the config save below writes the OLD
                       language back to disk. */
                    const char *code = jce_editor_i18n_locale_code((JceLocale)i);
                    if (code && *code) {
                        snprintf(s_cfg.language, sizeof(s_cfg.language),
                                 "%s", code);
                    }
                    cfg_dirty = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    /* Theme — 3 choices (Dark/Light/Blue) matching the canonical
     * jce_editor_style.cpp custom palettes. */
    int  cur_theme = theme_str_to_idx(s_cfg.theme);
    int  new_theme = cur_theme;
    if (combo_enum(jce_editor_i18n("panel.preferences.theme"),
                   &new_theme, JCE_THEME_COUNT, theme_label_i18n)) {
        if (new_theme != cur_theme) {
            jce_strlcpy(s_cfg.theme, theme_idx_to_str(new_theme),
                        sizeof(s_cfg.theme));
            jce_editor_apply_theme(new_theme);
            cfg_dirty = true;
        }
    }

    /* Font size — canonical range 12..32 (matches editor-config schema).
     * Live-preview via FontSizeBase on every change (smooth, no atlas
     * rebuild); commit a sharp re-bake when the user releases the
     * slider. */
    if (ImGui::SliderInt(jce_editor_i18n("panel.preferences.font_size"),
                         &s_cfg.font_size, 12, 32)) {
        s_cfg.font_size = clamp_int(s_cfg.font_size, 12, 32);
        apply_font_size(s_cfg.font_size);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        commit_font_size(s_cfg.font_size);
        cfg_dirty = true;
    }

    /* DPI / UI scale — canonical range 0.5..3.0 (matches editor-config). */
    if (ImGui::SliderFloat(jce_editor_i18n("panel.preferences.ui_scale"),
                           &s_cfg.ui_scale, 0.5f, 3.0f, "%.2fx")) {
        s_cfg.ui_scale = clamp_float(s_cfg.ui_scale, 0.5f, 3.0f);
        apply_ui_scale(s_cfg.ui_scale);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        cfg_dirty = true;

    /* Renderer backend — populated once from engine. Changing requires
     * a restart (matches the legacy dialog). */
    ImGui::Separator();
    enum { MAX_BACKENDS = 8 };
    static JceRendererBackend s_backends[MAX_BACKENDS];
    static const char        *s_backend_names[MAX_BACKENDS];
    static int                s_backend_count = -1;
    if (s_backend_count < 0) {
        s_backend_count = jce_renderer_caps_list_backends(s_backends, MAX_BACKENDS);
        for (int i = 0; i < s_backend_count; ++i)
            s_backend_names[i] = jce_renderer_backend_name(s_backends[i]);
    }
    int rend_idx = 0;
    for (int i = 0; i < s_backend_count; ++i) {
        if (jce_strcasecmp(s_cfg.renderer, s_backend_names[i]) == 0) {
            rend_idx = i;
            break;
        }
    }
    if (s_backend_count > 0 &&
        ImGui::Combo(jce_editor_i18n("panel.preferences.renderer"),
                     &rend_idx, s_backend_names, s_backend_count)) {
        jce_strlcpy(s_cfg.renderer, s_backend_names[rend_idx],
                    sizeof(s_cfg.renderer));
        cfg_dirty = true;
    }
    {
        const char *active = jce_renderer_get_backend_name(NULL);
        if (active && active[0]) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s: %s",
                jce_editor_i18n("panel.preferences.renderer_active"), active);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
            jce_editor_i18n("panel.preferences.renderer_hint"));
    }

    if (cfg_dirty)   jce_editor_config_save(&s_cfg);
    if (prefs_dirty) save_to_disk();
}

void draw_tab_fonts()
{
    bool cfg_dirty = false;

    ImGui::TextWrapped("%s", jce_editor_i18n("preferences.fonts.help"));
    ImGui::Spacing();

    enum { JCE_FONT_PICKER_MAX = 512 };
    static JceFontEntry  s_entries[JCE_FONT_PICKER_MAX];
    static const char   *s_labels[JCE_FONT_PICKER_MAX + 1];
    static int           s_count = -1;
    if (s_count < 0) {
        s_count = jce_editor_enumerate_fonts(s_entries, JCE_FONT_PICKER_MAX);
        for (int i = 0; i < s_count; ++i)
            s_labels[i + 1] = s_entries[i].display_name;
    }
    s_labels[0] = jce_editor_i18n("preferences.fonts.auto");

    auto find_idx = [](const char *path) -> int {
        if (!path || !*path) return 0;
        for (int i = 0; i < s_count; ++i)
            if (jce_strcasecmp(s_entries[i].path, path) == 0) return i + 1;
        return 0;
    };

    int en_idx = find_idx(s_cfg.font_en_path);
    int zh_idx = find_idx(s_cfg.font_zh_path);

    ImGui::PushItemWidth(-160);
    if (ImGui::Combo(jce_editor_i18n("preferences.fonts.latin"),
                     &en_idx, s_labels, s_count + 1)) {
        if (en_idx <= 0) s_cfg.font_en_path[0] = '\0';
        else jce_strlcpy(s_cfg.font_en_path, s_entries[en_idx - 1].path,
                         sizeof(s_cfg.font_en_path));
        cfg_dirty = true;
    }
    if (ImGui::Combo(jce_editor_i18n("preferences.fonts.cjk"),
                     &zh_idx, s_labels, s_count + 1)) {
        if (zh_idx <= 0) s_cfg.font_zh_path[0] = '\0';
        else jce_strlcpy(s_cfg.font_zh_path, s_entries[zh_idx - 1].path,
                         sizeof(s_cfg.font_zh_path));
        cfg_dirty = true;
    }
    ImGui::PopItemWidth();

    if (en_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s: %s",
                           jce_editor_i18n("fonts.latin"),
                           s_entries[en_idx - 1].path);
    if (zh_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s:   %s",
                           jce_editor_i18n("fonts.cjk"),
                           s_entries[zh_idx - 1].path);

    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s (%d %s)",
                       jce_editor_i18n("preferences.fonts.scanned"),
                       s_count,
                       jce_editor_i18n("preferences.fonts.fontsFound"));

    if (cfg_dirty) jce_editor_config_save(&s_cfg);
}

void draw_tab_viewport()
{
    bool cfg_dirty = false;

    if (ImGui::Checkbox(jce_editor_i18n("preferences.editor.showGrid"),
                        &s_cfg.show_grid))
        cfg_dirty = true;

    const char *view_modes[] = { "Shaded", "Wireframe", "Textured" };
    int vm = s_cfg.view_mode;
    if (vm < 0 || vm > 2) vm = 0;
    if (ImGui::Combo(jce_editor_i18n("preferences.editor.viewMode"),
                     &vm, view_modes, 3)) {
        s_cfg.view_mode = vm;
        cfg_dirty = true;
    }

    const char *asset_view[] = { "Grid", "Details" };
    int av = s_cfg.asset_browser_view_mode;
    if (av < 0 || av > 1) av = 0;
    if (ImGui::Combo(jce_editor_i18n("preferences.editor.assetView"),
                     &av, asset_view, 2)) {
        s_cfg.asset_browser_view_mode = av;
        cfg_dirty = true;
    }

    if (cfg_dirty) jce_editor_config_save(&s_cfg);
}

void draw_tab_input()
{
    bool cfg_dirty = false;

    ImGui::SeparatorText(jce_editor_i18n("preferences.input.mouseGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.invertScrollZoom"),
                        &s_cfg.invert_scroll_zoom)) {
        cfg_dirty = true;
        jce_editor_pref_invert_scroll_zoom = s_cfg.invert_scroll_zoom;
    }
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.invertDragY"),
                        &s_cfg.invert_drag_y)) {
        cfg_dirty = true;
        jce_editor_pref_invert_drag_y = s_cfg.invert_drag_y;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("preferences.input.touchpadGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.touchpadHInvert"),
                        &s_cfg.touchpad_h_invert)) {
        cfg_dirty = true;
        jce_editor_pref_touchpad_h_invert = s_cfg.touchpad_h_invert;
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.input.touchpadHint"));

    if (cfg_dirty) jce_editor_config_save(&s_cfg);
}

void draw_tab_paths()
{
    bool cfg_dirty = false;

    ImGui::TextWrapped("%s", jce_editor_i18n("preferences.paths.help"));
    ImGui::Spacing();

    if (jce_draw_path_input(jce_editor_i18n("preferences.paths.buildOutput"),
                            s_cfg.build_output_path,
                            sizeof(s_cfg.build_output_path),
                            JcePathKind::FolderAbs))
        cfg_dirty = true;

    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("preferences.paths.autoRepackGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.paths.autoRepackOnSave"),
                        &s_cfg.auto_repack_on_save))
        cfg_dirty = true;
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.paths.autoRepackHint"));

    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("preferences.run.devModeGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.run.devMode"),
                        &s_cfg.run_dev_mode))
        cfg_dirty = true;
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.run.devModeHint"));

    if (cfg_dirty) jce_editor_config_save(&s_cfg);
}

void draw_tab_hotkeys()
{
    static char s_filter[64] = {0};
    static int  s_capturing  = -1;       /* JceHotkeyId being captured */

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##hk_filter",
                             jce_editor_i18n("panel.hotkeys.filter"),
                             s_filter, sizeof(s_filter));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("panel.hotkeys.reset_all"))) {
        jce_hotkeys_reset_all();
        jce_hotkeys_save();
    }

    /* Cancel capture if the panel loses focus or the user hits Esc. */
    if (s_capturing >= 0 &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
        s_capturing = -1;
    }

    const ImGuiTableFlags tflags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##hk_table", 4, tflags, ImVec2(0, 0)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.hotkeys.action"),
                            ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.hotkeys.binding"),
                            ImGuiTableColumnFlags_WidthStretch, 0.25f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.hotkeys.default"),
                            ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableSetupColumn("##actions",
                            ImGuiTableColumnFlags_WidthStretch, 0.15f);
    ImGui::TableHeadersRow();

    const int total = jce_hotkeys_count();
    for (int i = 0; i < total; ++i) {
        const char *name = jce_hotkey_name((JceHotkeyId)i);
        if (s_filter[0]) {
            /* case-insensitive substring filter on display name + id */
            const char *id_str = jce_hotkey_id_string((JceHotkeyId)i);
            const char *sources[2] = { name, id_str };
            bool match = false;
            for (int si = 0; si < 2 && !match; ++si) {
                const char *src = sources[si];
                if (!src) continue;
                size_t flen = std::strlen(s_filter);
                size_t slen = std::strlen(src);
                if (flen == 0) { match = true; break; }
                if (flen > slen) continue;
                for (size_t k = 0; k + flen <= slen && !match; ++k) {
                    size_t j = 0;
                    for (; j < flen; ++j) {
                        char a = src[k + j], b = s_filter[j];
                        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                        if (a != b) break;
                    }
                    if (j == flen) match = true;
                }
            }
            if (!match) continue;
        }

        ImGui::PushID(i);
        ImGui::TableNextRow();

        /* Column 0: action name. */
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);

        /* Column 1: current binding (+ conflict marker). */
        ImGui::TableSetColumnIndex(1);
        JceHotkeyChord cur = jce_hotkey_get((JceHotkeyId)i);
        JceHotkeyId conflict =
            jce_hotkey_find_conflict((JceHotkeyId)i, cur);
        char clabel[64];
        jce_hotkey_chord_label(cur, clabel, sizeof(clabel));

        if (s_capturing == i) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                jce_editor_i18n("panel.hotkeys.capturing"));
        } else if (conflict < JCE_HK_COUNT) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "%s  !", clabel);
            if (ImGui::IsItemHovered()) {
                char tip[192];
                std::snprintf(tip, sizeof(tip),
                    jce_editor_i18n("panel.hotkeys.conflict"),
                    jce_hotkey_name(conflict));
                ImGui::SetTooltip("%s", tip);
            }
        } else {
            ImGui::TextUnformatted(clabel);
        }

        /* Column 2: default. */
        ImGui::TableSetColumnIndex(2);
        char dlabel[64];
        jce_hotkey_chord_label(jce_hotkey_get_default((JceHotkeyId)i),
                               dlabel, sizeof(dlabel));
        ImGui::TextUnformatted(dlabel);

        /* Column 3: [Bind] [Reset]. */
        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton(jce_editor_i18n("panel.hotkeys.bind"))) {
            s_capturing = i;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("panel.hotkeys.reset"))) {
            jce_hotkey_reset((JceHotkeyId)i);
            jce_hotkeys_save();
        }

        /* Capture loop: when this row is being bound, watch for the
         * next non-modifier key press and commit. */
        if (s_capturing == i) {
            ImGuiIO &io = ImGui::GetIO();
            uint8_t mods = 0;
            if (io.KeyCtrl)  mods |= JCE_HKM_CTRL;
            if (io.KeyShift) mods |= JCE_HKM_SHIFT;
            if (io.KeyAlt)   mods |= JCE_HKM_ALT;
            if (io.KeySuper) mods |= JCE_HKM_SUPER;

            for (int k = ImGuiKey_NamedKey_BEGIN;
                 k < ImGuiKey_NamedKey_END; ++k) {
                ImGuiKey kk = (ImGuiKey)k;
                /* Skip pure modifier keys and Esc (Esc cancels above). */
                if (kk == ImGuiKey_LeftCtrl  || kk == ImGuiKey_RightCtrl  ||
                    kk == ImGuiKey_LeftShift || kk == ImGuiKey_RightShift ||
                    kk == ImGuiKey_LeftAlt   || kk == ImGuiKey_RightAlt   ||
                    kk == ImGuiKey_LeftSuper || kk == ImGuiKey_RightSuper ||
                    kk == ImGuiKey_ReservedForModCtrl  ||
                    kk == ImGuiKey_ReservedForModShift ||
                    kk == ImGuiKey_ReservedForModAlt   ||
                    kk == ImGuiKey_ReservedForModSuper ||
                    kk == ImGuiKey_Escape) continue;
                if (ImGui::IsKeyPressed(kk, /*repeat=*/false)) {
                    JceHotkeyChord chord = { k, mods };
                    jce_hotkey_set((JceHotkeyId)i, chord);
                    jce_hotkeys_save();
                    s_capturing = -1;
                    break;
                }
            }
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

} /* anonymous namespace */

/* ── Public entry points ───────────────────────────────────────────── */

extern "C" void jce_editor_prefs_load_and_apply(void)
{
    ensure_loaded();
    apply_all();
}

extern "C" void jce_editor_panel_user_preferences(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_USER_PREFERENCES);
    if (!vis) return;

    /* P8-C: Preferences is a modal popup (non-dockable). When the
     * visibility flag is toggled to true (menu / hotkey / search), we
     * request the popup to open this frame and clear the flag — modal
     * lifecycle is fully owned by ImGui from here on. */
    static bool s_modal_open = false;
    if (*vis) {
        s_modal_open = true;
        *vis = false;
        ImGui::OpenPopup("###UserPreferences");
    }
    if (!s_modal_open) return;

    ensure_loaded();

    /* Center on the main viewport (no docking, no save settings). */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 size(720.0f, 480.0f);
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
               vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    char title[160];
    std::snprintf(title, sizeof(title), "%s###UserPreferences",
                  jce_editor_i18n("panel.preferences.title"));

    if (ImGui::BeginPopupModal(title, &s_modal_open,
                               ImGuiWindowFlags_NoDocking |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoCollapse)) {

        const float left_w = 160.0f * ImGui::GetIO().FontGlobalScale;

        ImGui::BeginChild("##prefs_tabs", ImVec2(left_w, -ImGui::GetFrameHeightWithSpacing()), true);
        struct TabSpec { int id; const char *key; };
        static const TabSpec kTabs[] = {
            { 0, "panel.preferences.tab.general"    },
            { 1, "panel.preferences.tab.appearance" },
            { 2, "panel.preferences.tab.fonts"      },
            { 3, "panel.preferences.tab.viewport"   },
            { 4, "panel.preferences.tab.input"      },
            { 5, "panel.preferences.tab.paths"      },
            { 6, "panel.preferences.tab.hotkeys"    },
        };
        for (const auto &t : kTabs) {
            bool sel = (s_active_tab == t.id);
            if (ImGui::Selectable(jce_editor_i18n(t.key), sel))
                s_active_tab = t.id;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##prefs_content", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        switch (s_active_tab) {
        case 0: draw_tab_general();    break;
        case 1: draw_tab_appearance(); break;
        case 2: draw_tab_fonts();      break;
        case 3: draw_tab_viewport();   break;
        case 4: draw_tab_input();      break;
        case 5: draw_tab_paths();      break;
        case 6: draw_tab_hotkeys();    break;
        default: break;
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n_or("dialog.close", "Close"),
                          ImVec2(120, 0))) {
            s_modal_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
