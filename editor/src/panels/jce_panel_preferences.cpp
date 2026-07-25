/*
 * jce_panel_preferences.cpp  User-level Preferences panel (P4-A.2).
 *
 * Unity-style two-pane layout (vertical tab list on the left, page
 * content on the right) for user-scoped editor settings.  Tabs:
 *
 *   General      autosave interval, startup behaviour, recent_max
 *   Appearance   language, theme, font size, UI scale, renderer
 *   Fonts        Latin / CJK font-file overrides
 *   Viewport     show grid, show gizmos, gizmo scale, view/asset modes
 *   Input        scroll/drag inversion, touchpad
 *   Paths        build output, auto-repack, dev mode
 *   Toolchains   per-kind tool path overrides
 *   Hotkeys      rebinding editor (filter / capture / reset)
 *
 * Persistence
 * -----------
 * Everything this panel edits lives in the canonical JceEditorConfig
 * (mirrored in s_cfg), persisted as the split per-user stores
 *   ~/.jce/editor-preferences.json  — appearance / fonts / viewport /
 *                                     input / paths / general (autosave,
 *                                     startup, recent_max); toolchain
 *                                     overrides as "toolchain.<kind>" KV
 *   ~/.jce/editor-session.json      — last-state keys (view mode, recents…)
 * plus hotkeys.json via the hotkey registry.  The former stores prefs.json
 * and editor-config.json are RETIRED: jce_editor_config_load merges them
 * forward one time and the next flush renames them *.migrated (hand-edits
 * to a dead file can never be silently inert).  s_cfg is re-synced from
 * disk on every panel OPEN so an external edit between sessions is not
 * clobbered by a whole-struct save.  The live gizmo-display accessors
 * (jce_editor_prefs_show_gizmos / _gizmo_scale) read s_cfg, so those prefs
 * now persist across restarts (they previously lived only in a retired
 * panel's in-memory struct and were lost every launch).
 *
 * The engine does not expose an "OS user pref dir" accessor, and the rest
 * of the editor already uses the ".jce/<file>.json" convention, so we stay
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
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_state.h"
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
#include <jce/os/core/jce_toolchain.h>
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

/* Theme indices map 1:1 to the canonical jce_editor_apply_theme() values
 * (JCE_THEME_DARK=0, JCE_THEME_LIGHT=1, JCE_THEME_SSMS=2). The on-disk
 * label "Blue" maps to JCE_THEME_SSMS — kept for legacy compatibility
 * with the older Preferences dialog. */
struct UserPrefs {
    /* UI mirror of the general fields; persisted via JceEditorConfig
     * (autosave_interval / startup_mode / recent_max in
     * ~/.jce/editor-preferences.json — the old prefs.json is retired). */
    int   autosave        = AUTOSAVE_5MIN;
    int   startup         = JCE_EDITOR_STARTUP_LAST;
    int   recent_max      = 10;
};

/* Theme/font/ui_scale/renderer are NOT duplicated here — they live in
 * the canonical JceEditorConfig (~/.jce/editor-preferences.json) so every
 * consumer of those keys stays in sync. */

UserPrefs        s_prefs;
JceEditorConfig  s_cfg;       /* mirror of the canonical config for live editing */
bool             s_loaded = false;
int              s_active_tab = 0;   /* 0=General, 1=Appearance, 2=Fonts,
                                        3=Viewport, 4=Input, 5=Paths, 6=Hotkeys */

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

/* String -> theme index parsing is the shared canonical helper
 * jce_editor_theme_from_string (jce_editor_style.h) — do NOT re-implement
 * it here, or the accepted spellings drift from the boot restore path. */

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
    jce_editor_apply_theme(jce_editor_theme_from_string(s_cfg.theme));
    apply_ui_scale(s_cfg.ui_scale);
    /* Font size applies on next frame via the deferred font reload. */
}

/* ── Disk I/O ──────────────────────────────────────────────────────── */

/* General prefs + toolchain overrides now live in the canonical config
 * (the retired ~/.jce/prefs.json is merged forward by
 * jce_editor_config_load and renamed *.migrated on the next flush).
 * Toolchain per-kind path overrides persist as "toolchain.<kind>" string
 * KV on the config singleton. */
void load_from_disk()
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);
    s_prefs.autosave   = clamp_int(cfg.autosave_interval, 0, AUTOSAVE_COUNT - 1);
    s_prefs.startup    = clamp_int(cfg.startup_mode, 0,
                                   JCE_EDITOR_STARTUP_COUNT - 1);
    s_prefs.recent_max = clamp_int(cfg.recent_max, 1, 20);

    for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
        char key[64], path[512];
        snprintf(key, sizeof(key), "toolchain.%s",
                 jce_toolchain_kind_name((JceToolchainKind)i));
        if (jce_editor_config_get_ui_str(key, path, sizeof(path)) && path[0])
            jce_toolchain_set_override((JceToolchainKind)i, path);
    }
}

void save_to_disk()
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);
    cfg.autosave_interval = s_prefs.autosave;
    cfg.startup_mode      = s_prefs.startup;
    cfg.recent_max        = s_prefs.recent_max;
    jce_editor_config_save(&cfg);

    /* Toolchain overrides: persist kinds the user actually set; clear the
     * KV for kinds whose override was removed this session. */
    for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "toolchain.%s",
                 jce_toolchain_kind_name((JceToolchainKind)i));
        const JceToolchain *t = jce_toolchain_get((JceToolchainKind)i);
        if (t && t->from_override && t->path[0])
            jce_editor_config_set_ui_str(key, t->path);
        else
            jce_editor_config_set_ui_str(key, "");
    }
}

void ensure_loaded()
{
    if (s_loaded) return;
    /* Canonical config first (it performs the one-time prefs.json merge),
     * then mirror the general fields + toolchains out of it. */
    if (!jce_editor_config_load(&s_cfg))
        jce_editor_config_defaults(&s_cfg);
    load_from_disk();
    /* Push the user's recent-list cap into the config layer so add_recent
     * honors it from startup (previously recent_max was inert). */
    jce_editor_config_set_recent_cap(s_prefs.recent_max);
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
    case JCE_EDITOR_STARTUP_LAST:   return jce_editor_i18n("panel.preferences.startup.last");
    case JCE_EDITOR_STARTUP_EMPTY:  return jce_editor_i18n("panel.preferences.startup.empty");
    case JCE_EDITOR_STARTUP_PICKER: return jce_editor_i18n("panel.preferences.startup.picker");
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
                        &s_prefs.startup, JCE_EDITOR_STARTUP_COUNT, startup_label);
    if (ImGui::SliderInt(jce_editor_i18n("panel.preferences.recent_max"),
                         &s_prefs.recent_max, 1, 20)) {
        s_prefs.recent_max = clamp_int(s_prefs.recent_max, 1, 20);
        /* Apply the new cap immediately: bound future adds and trim the
         * already-stored lists so the change is observable now, not only
         * after entries age out. */
        jce_editor_config_set_recent_cap(s_prefs.recent_max);
        int cap = s_prefs.recent_max;   /* arrays hold 20 (was min(.,10)) */
        bool cfg_trimmed = false;
        if (s_cfg.recent_count > cap)       { s_cfg.recent_count = cap;       cfg_trimmed = true; }
        if (s_cfg.recent_scene_count > cap) { s_cfg.recent_scene_count = cap; cfg_trimmed = true; }
        if (cfg_trimmed) jce_editor_config_save(&s_cfg);
        dirty = true;
    }
    if (dirty) save_to_disk();
}

void draw_tab_appearance()
{
    bool prefs_dirty = false;
    bool cfg_dirty   = false;

    /* The language picker below shows every locale's NATIVE name; the
     * Korean/Cyrillic glyphs those names need are locale-gated out of
     * non-ko/ru/uk sessions (memory charter).  Lift the gates the moment
     * the user reaches this tab — one deferred atlas rebuild, latched for
     * the session — so "한국어" / "Русский" never show as "?" here. */
    jce_editor_style_ensure_locale_picker_glyphs();

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
                    /* Persist the new locale so it survives a restart.
                       Without this the runtime switch happens but the config
                       save below writes the OLD language back to disk. */
                    const char *code = jce_editor_i18n_locale_code((JceLocale)i);
                    if (code && *code) {
                        snprintf(s_cfg.language, sizeof(s_cfg.language),
                                 "%s", code);
                    }
                    /* Rebuild the font atlas for the new locale.  The heavy
                       locale-gated glyph passes (Korean Hangul ~11k glyphs,
                       Cyrillic) only enter the atlas when the font builder
                       runs WITH that locale active — without this rebuild,
                       switching to ko/ru/uk rendered '?' until the next
                       editor restart (the boot path loads the right font
                       from the saved language).  set_locale above already
                       switched the active locale, so the deferred reload
                       sees the new language. */
                    jce_editor_request_font_reload((float)s_cfg.font_size,
                                                   s_cfg.font_en_path,
                                                   s_cfg.font_zh_path);
                    cfg_dirty = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    /* Theme — 3 choices (Dark/Light/Blue) matching the canonical
     * jce_editor_style.cpp custom palettes. */
    int  cur_theme = jce_editor_theme_from_string(s_cfg.theme);
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
                        &s_cfg.show_grid)) {
        /* Keep the live scene state in sync so the change is visible
         * immediately instead of only after the next config reload. */
        jce_state_set_show_grid(s_cfg.show_grid);
        cfg_dirty = true;
    }

    if (ImGui::Checkbox(jce_editor_i18n("preferences.editorTab.showGizmos"),
                        &s_cfg.show_gizmos))
        cfg_dirty = true;
    if (ImGui::SliderFloat(jce_editor_i18n("preferences.editorTab.gizmoScale"),
                           &s_cfg.gizmo_scale,
                           JCE_PREF_GIZMO_SCALE_MIN, JCE_PREF_GIZMO_SCALE_MAX)) {
        if (s_cfg.gizmo_scale < JCE_PREF_GIZMO_SCALE_MIN)
            s_cfg.gizmo_scale = JCE_PREF_GIZMO_SCALE_MIN;
        if (s_cfg.gizmo_scale > JCE_PREF_GIZMO_SCALE_MAX)
            s_cfg.gizmo_scale = JCE_PREF_GIZMO_SCALE_MAX;
        cfg_dirty = true;
    }

    const char *view_modes[] = { jce_editor_i18n("panel.preferences.viewMode.shaded"), jce_editor_i18n("panel.preferences.viewMode.wireframe"), jce_editor_i18n("panel.preferences.viewMode.textured") };
    int vm = s_cfg.view_mode;
    if (vm < 0 || vm > 2) vm = 0;
    if (ImGui::Combo(jce_editor_i18n("preferences.editor.viewMode"),
                     &vm, view_modes, 3)) {
        s_cfg.view_mode = vm;
        cfg_dirty = true;
    }

    const char *asset_view[] = { jce_editor_i18n("panel.preferences.assetView.grid"), jce_editor_i18n("panel.preferences.assetView.details") };
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

    /* build_output_path lives in Project Settings > Build (it is project build
     * output, not a per-user path) — a second copy here just let two panels
     * write the same field and confused which one was authoritative.  Edit it
     * there; this tab keeps the genuinely per-user path/tool + repack toggles. */

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

    /* External tools (per-user, per-machine) — Unity's Preferences >
       External Tools.  Used by the code / image viewers' "open externally"
       actions; empty falls back to the OS default handler. */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("preferences.externalTools.group"));
    if (jce_draw_path_input(jce_editor_i18n("preferences.externalTools.scriptEditor"),
                            s_cfg.external_script_editor,
                            sizeof(s_cfg.external_script_editor),
                            JcePathKind::FileAbs))
        cfg_dirty = true;
    if (jce_draw_path_input(jce_editor_i18n("preferences.externalTools.imageEditor"),
                            s_cfg.external_image_editor,
                            sizeof(s_cfg.external_image_editor),
                            JcePathKind::FileAbs))
        cfg_dirty = true;
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.externalTools.hint"));

    /* Reload-before-save (defense-in-depth): the Paths tab also authors
       build/run SESSION fields (build_output_path) shared with the Project
       Settings panel.  Re-read disk and overlay only the fields THIS tab
       owns, so a save here never reverts a sibling field another surface
       changed while this panel was open.  s_cfg stays the live mirror
       (gizmo accessors read it) so we sync it to what we wrote. */
    if (cfg_dirty) {
        JceEditorConfig disk;
        jce_editor_config_load(&disk);
        snprintf(disk.build_output_path, sizeof(disk.build_output_path), "%s",
                 s_cfg.build_output_path);
        disk.auto_repack_on_save = s_cfg.auto_repack_on_save;
        disk.run_dev_mode        = s_cfg.run_dev_mode;
        snprintf(disk.external_script_editor, sizeof(disk.external_script_editor),
                 "%s", s_cfg.external_script_editor);
        snprintf(disk.external_image_editor, sizeof(disk.external_image_editor),
                 "%s", s_cfg.external_image_editor);
        jce_editor_config_save(&disk);
        s_cfg = disk;
    }
}

void draw_tab_toolchains()
{
    /* Localised display names for each kind. */
    static const char *const k_titles[JCE_TOOLCHAIN_COUNT] = {
        "CMake", "Ninja", "MSVC (Visual Studio)", "GCC", "Clang",
        "Android NDK", "Emscripten SDK", "Xcode"
    };

    ImGui::TextWrapped("%s",
        jce_editor_i18n_or("panel.preferences.toolchains.hint",
            "Toolchains the editor detected on this machine. "
            "Override a path manually if auto-detection picked the wrong one."));

    if (ImGui::Button(jce_editor_i18n_or(
            "panel.preferences.toolchains.refresh", "Re-detect"))) {
        jce_toolchain_refresh();
    }
    ImGui::Separator();

    const ImGuiTableFlags tflags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##tc_table", 4, tflags))
        return;
    ImGui::TableSetupColumn(jce_editor_i18n("panel.preferences.tools.col.tool"),     ImGuiTableColumnFlags_WidthStretch, 0.18f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.preferences.tools.col.status"),   ImGuiTableColumnFlags_WidthStretch, 0.10f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.preferences.tools.col.version"),  ImGuiTableColumnFlags_WidthStretch, 0.15f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.preferences.tools.col.pathOverride"),
                                        ImGuiTableColumnFlags_WidthStretch, 0.57f);
    ImGui::TableHeadersRow();

    /* Per-kind input buffer cache.  Initialised lazily from current
     * toolchain path so the user sees the existing override / detected
     * path and can edit it in place. */
    static char  s_buf[JCE_TOOLCHAIN_COUNT][JCE_TOOLCHAIN_PATH_MAX] = {{0}};
    static bool  s_buf_init = false;
    if (!s_buf_init) {
        for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
            const JceToolchain *t = jce_toolchain_get((JceToolchainKind)i);
            if (t && t->from_override) {
                std::snprintf(s_buf[i], sizeof s_buf[i], "%s", t->path);
            }
        }
        s_buf_init = true;
    }

    for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
        const JceToolchain *t = jce_toolchain_get((JceToolchainKind)i);
        if (!t) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(k_titles[i]);

        ImGui::TableSetColumnIndex(1);
        if (t->present) {
            ImVec4 col = t->from_override
                ? ImVec4(0.4f, 0.7f, 1.0f, 1.0f)
                : ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
            ImGui::TextColored(col, "%s", t->from_override
                ? jce_editor_i18n_or("panel.preferences.toolchain.override", "override")
                : jce_editor_i18n_or("panel.preferences.toolchain.ok", "OK"));
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "%s",
                jce_editor_i18n_or("panel.preferences.toolchain.missing", "missing"));
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(t->version[0] ? t->version : "-");

        ImGui::TableSetColumnIndex(3);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-1.0f);
        char label[64];
        std::snprintf(label, sizeof label, "##tcpath_%d", i);
        /* Commit only on deactivate-after-edit: Enter both returns true
         * (EnterReturnsTrue) AND deactivates the widget, so a second
         * commit branch fired set+save TWICE per edit.  Deactivation
         * covers every commit path (Enter, Tab, click-away). */
        ImGui::InputTextWithHint(label, t->path, s_buf[i], sizeof s_buf[i]);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            jce_toolchain_set_override((JceToolchainKind)i, s_buf[i]);
            save_to_disk();
        }
        if (t->from_override) {
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                s_buf[i][0] = 0;
                jce_toolchain_set_override((JceToolchainKind)i, NULL);
                save_to_disk();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
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

/* Gizmo display prefs — live, per-frame accessors for the scene-view
 * renderer.  Backed by editor-config (s_cfg), so they now persist across
 * sessions (previously a retired panel's in-memory struct lost them on
 * every restart).  ensure_loaded() lazily mirrors disk on first use. */
extern "C" bool jce_editor_prefs_show_gizmos(void)
{
    ensure_loaded();
    return s_cfg.show_gizmos;
}

extern "C" float jce_editor_prefs_gizmo_scale(void)
{
    ensure_loaded();
    float s = s_cfg.gizmo_scale;
    if (s < JCE_PREF_GIZMO_SCALE_MIN) s = JCE_PREF_GIZMO_SCALE_MIN;
    if (s > JCE_PREF_GIZMO_SCALE_MAX) s = JCE_PREF_GIZMO_SCALE_MAX;
    return s;
}

/* Autosave interval in seconds (0 = disabled), from the General tab's
 * AutosaveInterval enum in prefs.json.  The editor main loop polls this to
 * drive a real autosave timer (previously the setting persisted but nothing
 * consumed it). */
extern "C" int jce_editor_prefs_autosave_interval_sec(void)
{
    ensure_loaded();
    switch (s_prefs.autosave) {
    case AUTOSAVE_1MIN:  return 60;
    case AUTOSAVE_5MIN:  return 300;
    case AUTOSAVE_15MIN: return 900;
    case AUTOSAVE_OFF:
    default:             return 0;
    }
}

extern "C" JceEditorStartupBehavior jce_editor_prefs_startup_behavior(void)
{
    ensure_loaded();
    return (JceEditorStartupBehavior)clamp_int(s_prefs.startup,
                                               0,
                                               JCE_EDITOR_STARTUP_COUNT - 1);
}

extern "C" void jce_editor_prefs_set_startup_behavior(
    JceEditorStartupBehavior behavior)
{
    ensure_loaded();
    int next = clamp_int((int)behavior, 0, JCE_EDITOR_STARTUP_COUNT - 1);
    if (s_prefs.startup == next)
        return;
    s_prefs.startup = next;
    save_to_disk();
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
        /* Re-sync the editor-config mirror from disk on every OPEN, not just
         * the first ever load: other subsystems (scene-view toolbar, asset
         * browser, theme apply) write editor-config between panel sessions,
         * and the long-lived s_cfg would otherwise re-save STALE values and
         * silently revert those external changes (whole-struct save). */
        s_loaded = false;
        ensure_loaded();
        ImGui::OpenPopup("###UserPreferences");
    }
    if (!s_modal_open) return;

    ensure_loaded();

    /* Close the stale-snapshot clobber window: when another writer bumps the
     * config singleton mid-modal (autosave recents, a toolbar toggle), re-sync
     * the mirror before drawing, so the next whole-struct save from this
     * panel carries their values instead of reverting them.  Generation-
     * gated (not per-frame) so in-flight widget edits — a font-size slider
     * drag lives in s_cfg across frames before its release-commit — are
     * never fought.  Our own saves bump the generation too, causing one
     * no-op re-sync next frame (s_cfg already equals the singleton). */
    static uint64_t s_seen_gen = 0;
    if (jce_editor_config_generation() != s_seen_gen) {
        if (!jce_editor_config_load(&s_cfg))
            jce_editor_config_defaults(&s_cfg);
        s_seen_gen = jce_editor_config_generation();
    }

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
            { 7, "panel.preferences.tab.toolchains" },
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
        case 7: draw_tab_toolchains(); break;
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
