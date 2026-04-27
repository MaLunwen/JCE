/*
 * jce_dialog_preferences.cpp  Centralized Preferences dialog.
 *
 * Global per-user editor preferences. Mirrors the structure of
 * jce_dialog_project_settings.cpp (left category tree + right page,
 * modal, non-dockable, top-most). Categories:
 *   General  - language, theme, font size, ui scale, renderer
 *   Fonts    - latin / CJK font picker (system font scan)
 *   Editor   - viewport visuals (grid, gizmos, sensitivity)
 *   Input    - mouse & touchpad behaviour
 *   Paths    - default project paths
 *
 * Project-scoped configuration lives in jce_dialog_project_settings.cpp.
 * Both dialogs read/write JceEditorConfig (single file) but address
 * non-overlapping field subsets.
 */

#include "jce_editor_dialogs.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_style.h"

#include <jce/tools/jce_imgui.h>
#include <jce/os/core/jce_str.h>
#include <jce/application/jce_config.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_renderer.h>
#include <stdio.h>
#include <string.h>

namespace {

enum Category {
    CAT_GENERAL = 0,
    CAT_FONTS,
    CAT_EDITOR,
    CAT_INPUT,
    CAT_PATHS,
    CAT_COUNT
};

const char *category_name(int i)
{
    switch (i) {
        case CAT_GENERAL: return jce_editor_i18n("preferences.cat.general");
        case CAT_FONTS:   return jce_editor_i18n("preferences.cat.fonts");
        case CAT_EDITOR:  return jce_editor_i18n("preferences.cat.editor");
        case CAT_INPUT:   return jce_editor_i18n("preferences.cat.input");
        case CAT_PATHS:   return jce_editor_i18n("preferences.cat.paths");
        default:          return "?";
    }
}

struct PrefsState {
    bool             initialized   = false;
    bool             snapshot_done = false;
    int              category      = CAT_GENERAL;
    JceEditorConfig  cfg;
    JceEditorConfig  cfg_orig;
    bool             dirty         = false;
    bool             needs_restart = false;
};

PrefsState s_p;

void take_snapshot()
{
    if (!jce_editor_config_load(&s_p.cfg))
        jce_editor_config_defaults(&s_p.cfg);
    /* Note: legacy 'SSMS' values (from a previous build) are accepted at
       runtime but are normalized to 'Blue' on next save. */
    if (jce_strcasecmp(s_p.cfg.theme, "SSMS") == 0)
        jce_strlcpy(s_p.cfg.theme, "Blue", sizeof(s_p.cfg.theme));
    s_p.cfg_orig      = s_p.cfg;
    s_p.dirty         = false;
    s_p.needs_restart = false;
}

/* Live-apply the visual preferences (theme / locale / DPI scale).
   Called after every combo/slider change so the user sees an
   immediate preview, but persistence still requires Apply/OK. */
void live_apply_visuals()
{
    int theme_idx = 0;
    if      (jce_strcasecmp(s_p.cfg.theme, "Light") == 0) theme_idx = 1;
    else if (jce_strcasecmp(s_p.cfg.theme, "Blue")  == 0) theme_idx = 2;
    else if (jce_strcasecmp(s_p.cfg.theme, "SSMS")  == 0) theme_idx = 2;
    jce_editor_apply_theme(theme_idx);

    if (jce_strcasecmp(s_p.cfg.language, "zh_cn") == 0)
        jce_editor_i18n_set_locale(JCE_LOCALE_ZH_CN);
    else
        jce_editor_i18n_set_locale(JCE_LOCALE_EN);
}

void apply_changes()
{
    jce_editor_config_save(&s_p.cfg);
    live_apply_visuals();
    s_p.cfg_orig = s_p.cfg;
    s_p.dirty    = false;
}

void revert_changes()
{
    s_p.cfg   = s_p.cfg_orig;
    s_p.dirty = false;
    /* Also revert any live-applied visual state. */
    live_apply_visuals();
    /* Restore cached input prefs. */
    jce_editor_pref_invert_scroll_zoom = s_p.cfg.invert_scroll_zoom;
    jce_editor_pref_invert_drag_y      = s_p.cfg.invert_drag_y;
    jce_editor_pref_touchpad_h_invert  = s_p.cfg.touchpad_h_invert;
}

#define MARK_DIRTY() (s_p.dirty = true)

/* ── General page ──────────────────────────────────────────────────── */
void draw_general_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("preferences.cat.general"));
    ImGui::Separator();

    /* Language - live preview */
    int lang_idx = (jce_strcasecmp(s_p.cfg.language, "zh_cn") == 0) ? 1 : 0;
    const char *langs[] = { "English", "中文" };
    if (ImGui::Combo(jce_editor_i18n("preferences.general.language"),
                     &lang_idx, langs, 2)) {
        jce_strlcpy(s_p.cfg.language, lang_idx == 1 ? "zh_cn" : "en",
                    sizeof(s_p.cfg.language));
        MARK_DIRTY();
        live_apply_visuals();
    }

    /* Theme - live preview (Dark/Light/Blue stored as-is in config; the
       'Blue' label maps to the engine's SSMS theme implementation). */
    int theme_idx = 0;
    if      (jce_strcasecmp(s_p.cfg.theme, "Light") == 0) theme_idx = 1;
    else if (jce_strcasecmp(s_p.cfg.theme, "Blue")  == 0) theme_idx = 2;
    else if (jce_strcasecmp(s_p.cfg.theme, "SSMS")  == 0) theme_idx = 2;
    const char *themes[] = { "Dark", "Light", "Blue" };
    if (ImGui::Combo(jce_editor_i18n("preferences.general.theme"),
                     &theme_idx, themes, 3)) {
        jce_strlcpy(s_p.cfg.theme, themes[theme_idx], sizeof(s_p.cfg.theme));
        MARK_DIRTY();
        live_apply_visuals();
    }

    /* Font size - dirty only; live size change requires font atlas rebuild
       which is outside this dialog. Mark needs_restart. */
    if (ImGui::SliderInt(jce_editor_i18n("preferences.general.fontSize"),
                         &s_p.cfg.font_size, 12, 32)) {
        MARK_DIRTY();
        s_p.needs_restart = true;
    }

    /* UI scale - dirty only (applied at restart). */
    if (ImGui::SliderFloat(jce_editor_i18n("preferences.general.uiScale"),
                           &s_p.cfg.ui_scale, 0.5f, 3.0f, "%.2fx"))
        MARK_DIRTY();

    /* Renderer - enumerate from engine (platform-aware). */
    enum { MAX_BACKENDS = 8 };
    static JceRendererBackend s_backends[MAX_BACKENDS];
    static const char        *s_backend_names[MAX_BACKENDS];
    static int                s_backend_count = -1;
    if (s_backend_count < 0) {
        s_backend_count = jce_renderer_caps_list_backends(s_backends, MAX_BACKENDS);
        for (int i = 0; i < s_backend_count; ++i)
            s_backend_names[i] = jce_renderer_backend_name(s_backends[i]);
    }
    /* Find current selection by matching cfg.renderer string against names. */
    int rend_idx = 0;
    for (int i = 0; i < s_backend_count; ++i) {
        if (jce_strcasecmp(s_p.cfg.renderer, s_backend_names[i]) == 0) {
            rend_idx = i;
            break;
        }
    }
    if (ImGui::Combo(jce_editor_i18n("preferences.general.renderer"),
                     &rend_idx, s_backend_names, s_backend_count)) {
        jce_strlcpy(s_p.cfg.renderer, s_backend_names[rend_idx],
                    sizeof(s_p.cfg.renderer));
        MARK_DIRTY();
        s_p.needs_restart = true;
    }
    /* Active backend (resolved at engine init) — green so users can see
       which backend Auto chose. */
    {
        const char *active = jce_renderer_get_backend_name(NULL);
        if (active && active[0]) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s: %s",
                               jce_editor_i18n("settings.activeBackend"),
                               active);
        }
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.general.rendererHint"));
}

/* ── Fonts page ────────────────────────────────────────────────────── */
void draw_fonts_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("preferences.cat.fonts"));
    ImGui::Separator();
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

    int en_idx = find_idx(s_p.cfg.font_en_path);
    int zh_idx = find_idx(s_p.cfg.font_zh_path);

    ImGui::PushItemWidth(-160);
    if (ImGui::Combo(jce_editor_i18n("preferences.fonts.latin"),
                     &en_idx, s_labels, s_count + 1)) {
        if (en_idx <= 0) s_p.cfg.font_en_path[0] = '\0';
        else jce_strlcpy(s_p.cfg.font_en_path, s_entries[en_idx - 1].path,
                         sizeof(s_p.cfg.font_en_path));
        MARK_DIRTY();
        s_p.needs_restart = true;
    }
    if (ImGui::Combo(jce_editor_i18n("preferences.fonts.cjk"),
                     &zh_idx, s_labels, s_count + 1)) {
        if (zh_idx <= 0) s_p.cfg.font_zh_path[0] = '\0';
        else jce_strlcpy(s_p.cfg.font_zh_path, s_entries[zh_idx - 1].path,
                         sizeof(s_p.cfg.font_zh_path));
        MARK_DIRTY();
        s_p.needs_restart = true;
    }
    ImGui::PopItemWidth();

    if (en_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Latin: %s",
                           s_entries[en_idx - 1].path);
    if (zh_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "CJK:   %s",
                           s_entries[zh_idx - 1].path);

    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s (%d %s)",
                       jce_editor_i18n("preferences.fonts.scanned"),
                       s_count,
                       jce_editor_i18n("preferences.fonts.fontsFound"));
}

/* ── Editor page (viewport visuals) ────────────────────────────────── */
void draw_editor_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("preferences.cat.editor"));
    ImGui::Separator();

    if (ImGui::Checkbox(jce_editor_i18n("preferences.editor.showGrid"),
                        &s_p.cfg.show_grid))
        MARK_DIRTY();

    const char *view_modes[] = { "Shaded", "Wireframe", "Textured" };
    int vm = s_p.cfg.view_mode;
    if (vm < 0 || vm > 2) vm = 0;
    if (ImGui::Combo(jce_editor_i18n("preferences.editor.viewMode"),
                     &vm, view_modes, 3)) {
        s_p.cfg.view_mode = vm;
        MARK_DIRTY();
    }

    const char *asset_view[] = { "Grid", "Details" };
    int av = s_p.cfg.asset_browser_view_mode;
    if (av < 0 || av > 1) av = 0;
    if (ImGui::Combo(jce_editor_i18n("preferences.editor.assetView"),
                     &av, asset_view, 2)) {
        s_p.cfg.asset_browser_view_mode = av;
        MARK_DIRTY();
    }
}

/* ── Input page ────────────────────────────────────────────────────── */
void draw_input_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("preferences.cat.input"));
    ImGui::Separator();

    ImGui::SeparatorText(jce_editor_i18n("preferences.input.mouseGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.invertScrollZoom"),
                        &s_p.cfg.invert_scroll_zoom)) {
        MARK_DIRTY();
        jce_editor_pref_invert_scroll_zoom = s_p.cfg.invert_scroll_zoom;
    }
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.invertDragY"),
                        &s_p.cfg.invert_drag_y)) {
        MARK_DIRTY();
        jce_editor_pref_invert_drag_y = s_p.cfg.invert_drag_y;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("preferences.input.touchpadGroup"));
    if (ImGui::Checkbox(jce_editor_i18n("preferences.input.touchpadHInvert"),
                        &s_p.cfg.touchpad_h_invert)) {
        MARK_DIRTY();
        jce_editor_pref_touchpad_h_invert = s_p.cfg.touchpad_h_invert;
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       jce_editor_i18n("preferences.input.touchpadHint"));
}

/* ── Paths page ────────────────────────────────────────────────────── */
void draw_paths_page()
{
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("preferences.cat.paths"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("preferences.paths.help"));
    ImGui::Spacing();

    if (ImGui::InputText(jce_editor_i18n("preferences.paths.buildOutput"),
                         s_p.cfg.build_output_path,
                         sizeof(s_p.cfg.build_output_path)))
        MARK_DIRTY();
}

} // namespace

extern "C" void jce_editor_dialog_preferences(bool *p_open)
{
    if (!p_open || !*p_open) {
        s_p.snapshot_done = false;
        return;
    }
    if (!s_p.initialized) { s_p = PrefsState{}; s_p.initialized = true; }

    if (!s_p.snapshot_done) {
        take_snapshot();
        s_p.snapshot_done = true;
    }

    const char *popup_id = "###PreferencesDialog";
    if (!ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char title[128];
    snprintf(title, sizeof(title), "%s%s",
             jce_editor_i18n("preferences.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(title, p_open,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        return;
    }

    /* Top status row */
    if (s_p.dirty)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("preferences.unsaved"));
    else if (s_p.needs_restart)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s",
                           jce_editor_i18n("preferences.requiresRestart"));
    else
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), " ");

    /* Category tree | page content */
    ImGui::BeginChild("##prefcat",
                       ImVec2(170, -ImGui::GetFrameHeightWithSpacing() - 8),
                       ImGuiChildFlags_Borders);
    for (int i = 0; i < CAT_COUNT; i++) {
        bool sel = (s_p.category == i);
        if (ImGui::Selectable(category_name(i), sel))
            s_p.category = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##prefpage",
                       ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 8),
                       ImGuiChildFlags_Borders);
    switch (s_p.category) {
        case CAT_GENERAL: draw_general_page(); break;
        case CAT_FONTS:   draw_fonts_page();   break;
        case CAT_EDITOR:  draw_editor_page();  break;
        case CAT_INPUT:   draw_input_page();   break;
        case CAT_PATHS:   draw_paths_page();   break;
    }
    ImGui::EndChild();

    /* Bottom buttons (Apply / Revert / OK / Cancel) */
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("preferences.button.apply"), ImVec2(100, 0))) {
        if (s_p.dirty) apply_changes();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("preferences.button.revert"), ImVec2(100, 0))) {
        revert_changes();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("preferences.button.ok"), ImVec2(100, 0))) {
        if (s_p.dirty) apply_changes();
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("preferences.button.cancel"), ImVec2(100, 0))) {
        revert_changes();
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
