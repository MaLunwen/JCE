/*
 * jce_editor_panels.cpp  Shared panel state and lifecycle.
 *
 * Individual panel implementations are in jce_panel_*.cpp files.
 * This file keeps: visibility array, console ring buffer + API,
 * about dialog, preferences (temporary until Settings dialog replaces it).
 */

#include "jce_editor_panels.h"

#include "dialogs/jce_path_input.h"
#include "core/jce_editor.h"
#include "core/jce_editor_alloc.h"
#include "jce_editor_colors.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_toast.h"
#include "jce_editor_style.h"
#include "jce_editor_ui_state.h"
#include "viewers/jce_file_viewer.h"

#include <ctype.h>
#include <jce/os/core/jce_str.h>
#include <jce/tools/jce_imgui.hpp>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_config.h>
#include <jce/jce_version.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_filesystem.h>

#include "io/jce_editor_file_util.h"
}

#define LOG_TAG "editor_panels"

/* Forward declaration for settings persistence. */
static void settings_ensure_init(void);

/* ══════════════════════════════════════════════════════════════════════
 *  PANEL VISIBILITY
 * ══════════════════════════════════════════════════════════════════════ */

static bool s_visible[JCE_PANEL_COUNT];

bool *jce_editor_panel_visible_ptr(JceEditorPanel panel)
{
    if (panel < 0 || panel >= JCE_PANEL_COUNT) return NULL;
    return &s_visible[panel];
}

void jce_editor_panels_persist_visibility(void)
{
    static uint64_t s_last_saved_mask = ~(uint64_t)0;
    uint64_t mask = 0;
    for (int i = 0; i < JCE_PANEL_COUNT && i < 64; i++) {
        if (s_visible[i]) mask |= ((uint64_t)1u << i);
    }
    if (mask == s_last_saved_mask) return;
    JceEditorConfig _cfg = {};
    jce_editor_config_load(&_cfg);
    uint64_t cur = ((uint64_t)_cfg.panels_visible_mask_hi << 32) |
                   (uint64_t)_cfg.panels_visible_mask;
    /* On first save the low half holds the unset sentinel (0xFFFFFFFF);
       force a write in that case to migrate from 32-bit format. */
    bool first_save = (_cfg.panels_visible_mask == JCE_EDITOR_PANELS_MASK_UNSET);
    if (!first_save && cur == mask) {
        s_last_saved_mask = mask;
        return;
    }
    _cfg.panels_visible_mask    = (uint32_t)(mask & 0xFFFFFFFFu);
    _cfg.panels_visible_mask_hi = (uint32_t)(mask >> 32);
    if (jce_editor_config_save(&_cfg)) {
        s_last_saved_mask = mask;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  CONSOLE RING BUFFER
 * ══════════════════════════════════════════════════════════════════════ */

#define CONSOLE_MAX_LINES 1024
#define CONSOLE_LINE_LEN  256

typedef struct {
    char            text[CONSOLE_LINE_LEN];
    char            timestamp[24];
    JceConsoleLevel level;
} ConsoleLine;

static struct {
    ConsoleLine lines[CONSOLE_MAX_LINES];
    int         count;
    int         head;
} s_console;

static void console_add(JceConsoleLevel level, const char *text)
{
    int idx = s_console.head % CONSOLE_MAX_LINES;
    s_console.lines[idx].level = level;
    snprintf(s_console.lines[idx].text, CONSOLE_LINE_LEN, "%s", text);

    int64_t now_s = jce_time_now_epoch_seconds();
    if (jce_time_format_local(now_s, "%Y-%m-%d %H:%M:%S",
                              s_console.lines[idx].timestamp,
                              sizeof(s_console.lines[idx].timestamp)) == 0)
        snprintf(s_console.lines[idx].timestamp,
                 sizeof(s_console.lines[idx].timestamp),
                 "----------  --:--:--");

    s_console.head++;
    if (s_console.count < CONSOLE_MAX_LINES)
        s_console.count++;
}

void jce_editor_console_log(const char *fmt, ...)
{
    char buf[CONSOLE_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_add(JCE_CONSOLE_INFO, buf);
}

void jce_editor_console_log_level(JceConsoleLevel level, const char *fmt, ...)
{
    char buf[CONSOLE_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_add(level, buf);

    /* Auto-surface warnings & errors as toasts so users notice them
       without having to open the Console panel. */
    if (level == JCE_CONSOLE_ERROR)
        jce_toast_error("%s", buf);
    else if (level == JCE_CONSOLE_WARNING)
        jce_toast_warn("%s", buf);
}

void jce_editor_console_clear(void)
{
    s_console.count = 0;
    s_console.head  = 0;
}

/* Console iteration API (used by jce_panel_console.cpp). */

int jce_editor_console_entry_count(void)
{
    return s_console.count;
}

bool jce_editor_console_entry_get(int display_idx, JceConsoleEntry *out)
{
    if (!out || display_idx < 0 || display_idx >= s_console.count)
        return false;

    int start = 0;
    if (s_console.count >= CONSOLE_MAX_LINES)
        start = s_console.head % CONSOLE_MAX_LINES;

    int raw = (start + display_idx) % CONSOLE_MAX_LINES;
    out->text      = s_console.lines[raw].text;
    out->timestamp = s_console.lines[raw].timestamp;
    out->level     = s_console.lines[raw].level;
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  LIFECYCLE
 * ══════════════════════════════════════════════════════════════════════ */

void jce_editor_panels_init(void)
{
    s_visible[JCE_PANEL_HIERARCHY]   = true;
    s_visible[JCE_PANEL_INSPECTOR]   = true;
    s_visible[JCE_PANEL_CONSOLE]     = true;
    s_visible[JCE_PANEL_SCENE_VIEW]  = true;
    s_visible[JCE_PANEL_GAME_VIEW]   = true;
    s_visible[JCE_PANEL_ASSETS]      = true;
    s_visible[JCE_PANEL_FILE_VIEWER] = true;
    /* Specialized panels (Unity-style: shown only on demand). Reduces
       startup tab clutter and matches what users expect from the
       Window menu being where you go to enable them. */
    s_visible[JCE_PANEL_TIMELINE]         = false;
    s_visible[JCE_PANEL_POSTFX]           = false;
    s_visible[JCE_PANEL_PROFILER]         = false;
    s_visible[JCE_PANEL_PARTICLE_EDITOR]  = false;
    s_visible[JCE_PANEL_MATERIAL_GRAPH]   = false;
    s_visible[JCE_PANEL_IMPORT_PRESETS]   = false;
    s_visible[JCE_PANEL_LIGHTMAP_BAKE]    = false;
    s_visible[JCE_PANEL_LIGHTING_DEPRECATED] = false;
    s_visible[JCE_PANEL_AUDIO_MIXER]      = false;
    s_visible[JCE_PANEL_INPUT_MANAGER]    = false;
    s_visible[JCE_PANEL_CURVE_EDITOR]     = false;
    s_visible[JCE_PANEL_ANIMATION_EDITOR] = false;
    s_visible[JCE_PANEL_ANIMATOR_SM]      = false;
    s_visible[JCE_PANEL_SEQUENCER]        = false;
    s_visible[JCE_PANEL_NAVMESH]          = false;
    s_visible[JCE_PANEL_TERRAIN]          = false;
    s_visible[JCE_PANEL_PREFERENCES]      = false;
    s_visible[JCE_PANEL_PACKAGE_MANAGER]  = false;
    s_visible[JCE_PANEL_FRAME_DEBUGGER]   = false;
    s_visible[JCE_PANEL_SPRITE_EDITOR]    = false;
    s_visible[JCE_PANEL_TILE_PALETTE]     = false;
    s_visible[JCE_PANEL_TOOLBAR]          = true;
    s_visible[JCE_PANEL_STATUS_BAR]       = true;
    s_visible[JCE_PANEL_MEMORY_PROFILER]  = false;
    s_visible[JCE_PANEL_PHYSICS_DEBUGGER] = false;
    s_visible[JCE_PANEL_LIGHT_EXPLORER]   = false;
    s_visible[JCE_PANEL_REFLECTION_PROBES]= false;
    s_visible[JCE_PANEL_SHADER_GRAPH]     = false;
    s_visible[JCE_PANEL_SEARCH]           = false;
    s_visible[JCE_PANEL_VERSION_CONTROL]  = false;
    s_visible[JCE_PANEL_TIME_OF_DAY]      = false;
    s_visible[JCE_PANEL_VCAM_MANAGER]     = false;
    s_visible[JCE_PANEL_REVERB_ZONES]     = false;
    s_visible[JCE_PANEL_SAVE_BROWSER]     = false;
    s_visible[JCE_PANEL_BUNDLE_BROWSER]   = false;
    s_visible[JCE_PANEL_LIGHTING_SETTINGS]= false;
    s_visible[JCE_PANEL_BUILD_REPORT]     = false;
    s_visible[JCE_PANEL_SYSTEMS]          = false;
    s_visible[JCE_PANEL_PROJECT_SETTINGS] = false;
    s_visible[JCE_PANEL_USER_PREFERENCES] = false;
    s_visible[JCE_PANEL_BT_VISUALIZER]    = false;
    s_visible[JCE_PANEL_WORLD_STREAMING]  = false;

    /* Console ring buffer. */
    memset(&s_console, 0, sizeof(s_console));

    /* Load persisted editor settings (language, theme, font, renderer). */
    settings_ensure_init();

    /* Restore window panel visibility from editor-config (overrides defaults
       set above). Sentinel JCE_EDITOR_PANELS_MASK_UNSET means "never saved",
       which keeps the per-panel defaults. */
    {
        JceEditorConfig _ecfg;
        if (jce_editor_config_load(&_ecfg) &&
            _ecfg.panels_visible_mask != JCE_EDITOR_PANELS_MASK_UNSET) {
            uint64_t mask = ((uint64_t)_ecfg.panels_visible_mask_hi << 32) |
                            (uint64_t)_ecfg.panels_visible_mask;
            for (int i = 0; i < JCE_PANEL_COUNT && i < 64; i++) {
                s_visible[i] = (mask >> i) & 1u;
            }
        }
    }

    jce_editor_console_log_level(JCE_CONSOLE_INFO, "editor panels initialized");
}

void jce_editor_panels_shutdown(void)
{
    jce_file_viewer_shutdown();
}

/* ══════════════════════════════════════════════════════════════════════
 *  FILE VIEWER — delegated to jce_panel_file_viewer.cpp
 * ══════════════════════════════════════════════════════════════════════ */

void jce_editor_panel_file_viewer_content(void)
{
    jce_file_viewer_draw_content();
}

void jce_editor_panel_file_viewer(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER);
    if (!*vis) return;
    jce_file_viewer_draw_window(vis);
}

/* ══════════════════════════════════════════════════════════════════════
 *  SETTINGS DIALOG (full implementation)
 * ══════════════════════════════════════════════════════════════════════ */

/* Renderer backend list — populated at runtime from compiled-in
   bgfx backends via jce_renderer_caps_list_backends().  Falls back
   to a single "Auto" entry if the renderer has not been initialized
   yet (e.g. settings panel opened before first frame). */
#define JCE_EDITOR_MAX_BACKENDS 8
static JceRendererBackend s_renderer_values[JCE_EDITOR_MAX_BACKENDS] = {
    JCE_BACKEND_AUTO
};
static const char        *s_renderer_names[JCE_EDITOR_MAX_BACKENDS]  = {
    "Auto"
};
static int                s_renderer_count = 1;
static bool               s_renderer_list_built = false;

static void build_renderer_list_if_needed(void)
{
    if (s_renderer_list_built) return;
    int n = jce_renderer_caps_list_backends(s_renderer_values,
                                            JCE_EDITOR_MAX_BACKENDS);
    if (n <= 0) return; /* bgfx not initialized yet — keep defaults. */
    if (n > JCE_EDITOR_MAX_BACKENDS) n = JCE_EDITOR_MAX_BACKENDS;
    for (int i = 0; i < n; ++i)
        s_renderer_names[i] = jce_renderer_backend_name(s_renderer_values[i]);
    s_renderer_count = n;
    s_renderer_list_built = true;
}

extern "C" int jce_editor_renderer_backends(const char *const **out_names)
{
    build_renderer_list_if_needed();
    if (out_names) *out_names = s_renderer_names;
    return s_renderer_count;
}

static struct {
    int   language_idx;
    int   theme_idx;
    int   renderer_idx;
    float font_size;
    char  font_en_path[512];
    char  font_zh_path[512];
    /* Saved originals for Cancel. */
    int   orig_language_idx;
    int   orig_theme_idx;
    int   orig_renderer_idx;
    float orig_font_size;
    char  orig_font_en_path[512];
    char  orig_font_zh_path[512];
    bool  needs_restart;
    bool  initialized;
} s_settings;

static int renderer_backend_to_idx(JceRendererBackend b)
{
    for (int i = 0; i < s_renderer_count; i++)
        if (s_renderer_values[i] == b) return i;
    return 0;
}

static void settings_ensure_init(void)
{
    if (s_settings.initialized) return;
    memset(&s_settings, 0, sizeof(s_settings));

    /* Load persisted editor config. */
    JceEditorConfig ecfg;
        if (jce_editor_config_load(&ecfg)) {
            /* Language — derived from the persisted stable code (handles
               any number of locales without index-magic). */
            JceLocale _persisted = jce_editor_i18n_locale_from_code(ecfg.language);
            s_settings.language_idx = (int)_persisted;
            jce_editor_i18n_set_locale(_persisted);

        /* Theme — accept Blue (preferred) and SSMS (legacy) for the SSMS engine theme. */
        if (strcmp(ecfg.theme, "Light") == 0) s_settings.theme_idx = JCE_THEME_LIGHT;
        else if (strcmp(ecfg.theme, "Blue") == 0) s_settings.theme_idx = JCE_THEME_SSMS;
        else if (strcmp(ecfg.theme, "SSMS") == 0) s_settings.theme_idx = JCE_THEME_SSMS;
        else s_settings.theme_idx = JCE_THEME_DARK;
        jce_editor_apply_theme(s_settings.theme_idx);

        /* Font size */
        if (ecfg.font_size >= 12 && ecfg.font_size <= 48)
            s_settings.font_size = (float)ecfg.font_size;
        else
            s_settings.font_size = jce_editor_get_font_size();

        /* Renderer */
        build_renderer_list_if_needed();
        s_settings.renderer_idx = 0;
        for (int i = 0; i < s_renderer_count; i++) {
            if (strcmp(s_renderer_names[i], ecfg.renderer) == 0) {
                s_settings.renderer_idx = i;
                break;
            }
        }

        /* Font path overrides (applied on next restart). */
        snprintf(s_settings.font_en_path, sizeof(s_settings.font_en_path),
                 "%s", ecfg.font_en_path);
        snprintf(s_settings.font_zh_path, sizeof(s_settings.font_zh_path),
                 "%s", ecfg.font_zh_path);
    } else {
        s_settings.language_idx  = (int)jce_editor_i18n_get_locale();
        s_settings.theme_idx     = jce_editor_get_theme();
        s_settings.renderer_idx  = 0;
        s_settings.font_size     = jce_editor_get_font_size();
    }

    s_settings.initialized   = true;
}

static void settings_snapshot(void)
{
    s_settings.orig_language_idx = s_settings.language_idx;
    s_settings.orig_theme_idx    = s_settings.theme_idx;
    s_settings.orig_renderer_idx = s_settings.renderer_idx;
    s_settings.orig_font_size    = s_settings.font_size;
    snprintf(s_settings.orig_font_en_path, sizeof(s_settings.orig_font_en_path),
             "%s", s_settings.font_en_path);
    snprintf(s_settings.orig_font_zh_path, sizeof(s_settings.orig_font_zh_path),
             "%s", s_settings.font_zh_path);
}

static void settings_apply(void)
{
    /* Language — already applied immediately via Combo callback. */

    /* Theme — already applied immediately via Combo callback. */

    /* Font size — saved to config; requires restart. */

    /* Renderer — requires restart; also sync to engine .config/jce.ini. */
    if (s_settings.renderer_idx != s_settings.orig_renderer_idx)
        s_settings.needs_restart = true;

    /* Persist to editor config file. */
    {
        JceEditorConfig ecfg;
        jce_editor_config_load(&ecfg);

        snprintf(ecfg.language, sizeof(ecfg.language), "%s",
                 jce_editor_i18n_locale_code((JceLocale)s_settings.language_idx));
        ecfg.font_size = (int)s_settings.font_size;

        const char *theme_names[] = { "Dark", "Light", "Blue" };
        snprintf(ecfg.theme, sizeof(ecfg.theme), "%s",
                 theme_names[s_settings.theme_idx]);

        if (s_settings.renderer_idx >= 0 && s_settings.renderer_idx < s_renderer_count)
            snprintf(ecfg.renderer, sizeof(ecfg.renderer), "%s",
                     s_renderer_names[s_settings.renderer_idx]);

        snprintf(ecfg.font_en_path, sizeof(ecfg.font_en_path),
                 "%s", s_settings.font_en_path);
        snprintf(ecfg.font_zh_path, sizeof(ecfg.font_zh_path),
                 "%s", s_settings.font_zh_path);

        jce_editor_config_save(&ecfg);
    }

    /* Renderer backend change is picked up on next editor startup via
       configure_engine_renderer_from_editor_config() in editor_main.cpp,
       which writes a temporary .jce/editor-engine.ini for the engine.
       No need to touch .config/jce.ini. */

    /* Update snapshot so Cancel won't revert applied changes. */
    settings_snapshot();

    jce_editor_console_log("Settings applied and saved");
}

static void settings_cancel(void)
{
    /* Revert language. */
    if (s_settings.language_idx != s_settings.orig_language_idx) {
        s_settings.language_idx = s_settings.orig_language_idx;
        jce_editor_i18n_set_locale((JceLocale)s_settings.language_idx);
    }
    /* Revert theme. */
    if (s_settings.theme_idx != s_settings.orig_theme_idx) {
        s_settings.theme_idx = s_settings.orig_theme_idx;
        jce_editor_apply_theme(s_settings.theme_idx);
    }
    /* Revert font size (restart-only, no live revert needed). */
    s_settings.font_size = s_settings.orig_font_size;
    /* Revert renderer. */
    s_settings.renderer_idx = s_settings.orig_renderer_idx;
    /* Revert font path overrides. */
    snprintf(s_settings.font_en_path, sizeof(s_settings.font_en_path),
             "%s", s_settings.orig_font_en_path);
    snprintf(s_settings.font_zh_path, sizeof(s_settings.font_zh_path),
             "%s", s_settings.orig_font_zh_path);
    s_settings.needs_restart = false;
}

void jce_editor_settings_dialog(bool *p_open)
{
    if (!p_open) return;
    settings_ensure_init();

    static bool was_open = false;
    if (*p_open && !was_open) {
        settings_snapshot();
        was_open = true;
    }
    if (!*p_open && was_open) {
        settings_cancel();
        was_open = false;
    }
    if (!*p_open) return;

    const char *popup_id = "###SettingsDialog";
    if (*p_open && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char _title[256];
    snprintf(_title, sizeof(_title), "%s%s",
             jce_editor_i18n("settings.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking)) {
        return;
    }

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("settings.description"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    char _lbl[256];

    /* Language — built from the live locale registry so adding a locale
       only requires updating the enum + JSON file. */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_lang", jce_editor_i18n("settings.language"));
    const int   n_loc = jce_editor_i18n_locale_count();
    const char *languages[JCE_MAX_LOCALES];
    for (int i = 0; i < n_loc; i++)
        languages[i] = jce_editor_i18n_locale_native_name((JceLocale)i);
    ImGui::PushItemWidth(200);
    if (ImGui::Combo(_lbl, &s_settings.language_idx, languages, n_loc)) {
        jce_editor_i18n_set_locale((JceLocale)s_settings.language_idx);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.appliesImmediately"));

    /* Theme */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_theme", jce_editor_i18n("settings.theme"));
    const char *themes[] = { jce_editor_i18n("panel.preferences.theme.dark"), jce_editor_i18n("panel.preferences.theme.light"), jce_editor_i18n("panel.preferences.theme.blue") };
    if (ImGui::Combo(_lbl, &s_settings.theme_idx, themes, 3))
        jce_editor_apply_theme(s_settings.theme_idx);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.appliesImmediately"));

    /* Font Size — requires restart to take effect. */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_fontsize", jce_editor_i18n("settings.fontSize"));
    ImGui::SliderFloat(_lbl, &s_settings.font_size, 12.0f, 48.0f, "%.0f px");
    if (s_settings.font_size != s_settings.orig_font_size)
        s_settings.needs_restart = true;
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.requiresRestart"));

    /* Renderer Backend (selector) */
    build_renderer_list_if_needed();
    snprintf(_lbl, sizeof(_lbl), "%s###settings_renderer", jce_editor_i18n("settings.renderBackend"));
    if (ImGui::Combo(_lbl, &s_settings.renderer_idx, s_renderer_names, s_renderer_count)) {
        s_settings.needs_restart = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.requiresRestart"));

    /* Current active backend display. */
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "%s: %s",
                       jce_editor_i18n("settings.activeBackend"),
                       jce_renderer_get_backend_name(NULL));

    ImGui::PopItemWidth();

    /* Font picker — pick from fonts discovered on the host system.
     * "(Auto)" => system font auto-detection at startup; if no system
     * font matches, ImGui's built-in default is used.
     * Changes take effect on next editor restart. */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("settings.fonts.title"));
    ImGui::TextWrapped("%s", jce_editor_i18n("settings.fonts.help"));
    ImGui::Spacing();

    /* Lazy one-shot scan of available fonts on the host. */
    enum { JCE_FONT_PICKER_MAX = 512 };
    static JceFontEntry  s_font_entries[JCE_FONT_PICKER_MAX];
    static const char   *s_font_labels[JCE_FONT_PICKER_MAX + 1]; /* +1 for Auto */
    static int           s_font_count   = -1;   /* -1 => not scanned yet */
    if (s_font_count < 0) {
        s_font_count = jce_editor_enumerate_fonts(s_font_entries,
                                                  JCE_FONT_PICKER_MAX);
        s_font_labels[0] = jce_editor_i18n("settings.fonts.auto");
        for (int i = 0; i < s_font_count; ++i)
            s_font_labels[i + 1] = s_font_entries[i].display_name;
    }
    /* Refresh "Auto" label every frame so locale changes apply live. */
    s_font_labels[0] = jce_editor_i18n("settings.fonts.auto");

    /* Helper lambda to find the combo index that matches a stored path. */
    auto find_idx_for_path = [](const char *path) -> int {
        if (!path || !*path) return 0;          /* Auto */
        for (int i = 0; i < s_font_count; ++i)
            if (jce_strcasecmp(s_font_entries[i].path, path) == 0)
                return i + 1;
        return 0;                               /* unknown -> show Auto */
    };

    int en_idx = find_idx_for_path(s_settings.font_en_path);
    int zh_idx = find_idx_for_path(s_settings.font_zh_path);

    ImGui::PushItemWidth(-160);

    snprintf(_lbl, sizeof(_lbl), "%s###settings_font_en",
             jce_editor_i18n("settings.fonts.latin"));
    if (ImGui::Combo(_lbl, &en_idx, s_font_labels, s_font_count + 1)) {
        if (en_idx <= 0)
            s_settings.font_en_path[0] = '\0';
        else
            jce_strlcpy(s_settings.font_en_path,
                        s_font_entries[en_idx - 1].path,
                        sizeof(s_settings.font_en_path));
        s_settings.needs_restart = true;
    }

    snprintf(_lbl, sizeof(_lbl), "%s###settings_font_zh",
             jce_editor_i18n("settings.fonts.cjk"));
    if (ImGui::Combo(_lbl, &zh_idx, s_font_labels, s_font_count + 1)) {
        if (zh_idx <= 0)
            s_settings.font_zh_path[0] = '\0';
        else
            jce_strlcpy(s_settings.font_zh_path,
                        s_font_entries[zh_idx - 1].path,
                        sizeof(s_settings.font_zh_path));
        s_settings.needs_restart = true;
    }
    ImGui::PopItemWidth();

    /* Show the resolved absolute path of the current selection (helpful
       when two installed fonts share a display name). */
    if (en_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s: %s",
                           jce_editor_i18n("fonts.latin"),
                           s_font_entries[en_idx - 1].path);
    if (zh_idx > 0)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s:   %s",
                           jce_editor_i18n("fonts.cjk"),
                           s_font_entries[zh_idx - 1].path);

    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "%s (%d %s)",
                       jce_editor_i18n("settings.fonts.scanned"),
                       s_font_count,
                       jce_editor_i18n("settings.fonts.fontsFound"));

    /* Restart warning */
    if (s_settings.needs_restart) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("settings.restartNote"));
    }

    /* Buttons: Apply | OK | Cancel (right-aligned) */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 80.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 3 + spacing * 2;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w + ImGui::GetCursorPosX());

    if (ImGui::Button(jce_editor_i18n("dialog.apply"), ImVec2(btn_w, 0))) {
        settings_apply();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.ok"), ImVec2(btn_w, 0))) {
        settings_apply();
        ImGui::CloseCurrentPopup();
        *p_open = false;
        was_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        settings_cancel();
        ImGui::CloseCurrentPopup();
        *p_open = false;
        was_open = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        settings_cancel();
        ImGui::CloseCurrentPopup();
        *p_open = false;
        was_open = false;
    }

    ImGui::EndPopup();
}


/* ══════════════════════════════════════════════════════════════════════
 *  ABOUT DIALOG (modal)
 * ══════════════════════════════════════════════════════════════════════ */

/* Lazy-loaded license text, sourced from the editor PAK
 * (engine/resources/THIRD_PARTY_LICENSES.md, baked at build time). */
static char  *s_tpl_text = NULL;
static size_t s_tpl_len  = 0;

static void load_tpl_once(void)
{
    if (s_tpl_text) return;

    const JcePakArchive *pak = jce_editor_get_pak();
    if (pak) {
        const JcePakAsset *asset = jce_pak_find(pak, "THIRD_PARTY_LICENSES.md");
        if (asset && asset->original_size > 0) {
            size_t sz = (size_t)asset->original_size;
            s_tpl_text = (char *)ED_MALLOC(sz + 1);
            if (s_tpl_text) {
                size_t got = jce_pak_decompress(asset, s_tpl_text, sz);
                if (got == 0) got = sz;
                s_tpl_text[got] = '\0';
                s_tpl_len = got;
                return;
            }
        }
    }

    static const char fallback[] =
        "THIRD_PARTY_LICENSES.md not packaged into editor PAK.";
    s_tpl_text = (char *)ED_MALLOC(sizeof(fallback));
    if (s_tpl_text) {
        memcpy(s_tpl_text, fallback, sizeof(fallback));
        s_tpl_len = sizeof(fallback) - 1;
    }
}

static void render_third_party_popup(bool *p_open)
{
    const char *popup_id = "###ThirdPartyDialog";
    if (*p_open && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char title[256];
    snprintf(title, sizeof(title), "%s%s",
             jce_editor_i18n("about.thirdPartyTitle"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 640), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(title, p_open,
                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        return;
    }

    load_tpl_once();
    ImGui::BeginChild("##tpl_scroll",
                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                      true, ImGuiWindowFlags_HorizontalScrollbar);
    if (s_tpl_text)
        ImGui::TextUnformatted(s_tpl_text, s_tpl_text + s_tpl_len);
    ImGui::EndChild();

    if (ImGui::Button(jce_editor_i18n("dialog.close"), ImVec2(100, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void jce_editor_about_dialog(bool *p_open)
{
    static bool s_show_tpl = false;
    static bool s_sha_loaded = false;
    static char s_sha[96];   /* "<64 hex>  jce_editor.exe\n" comfortably fits */

    if (!s_sha_loaded) {
        s_sha_loaded = true;
        s_sha[0] = '\0';
        char base[1024];
        if (jce_fs_host_get_base_path(base, sizeof(base))) {
            char path[1024];
            snprintf(path, sizeof(path), "%sjce_editor_sha256.txt", base);
            size_t sz = 0;
            char *buf = (char *)ed_read_file(path, &sz);
            if (buf) {
                /* The file is "<hex>  <filename>\n" — keep just the hex prefix. */
                size_t take = sz < sizeof(s_sha) - 1 ? sz : sizeof(s_sha) - 1;
                size_t hex_end = 0;
                while (hex_end < take && ((buf[hex_end] >= '0' && buf[hex_end] <= '9') ||
                                          (buf[hex_end] >= 'a' && buf[hex_end] <= 'f') ||
                                          (buf[hex_end] >= 'A' && buf[hex_end] <= 'F')))
                    hex_end++;
                memcpy(s_sha, buf, hex_end);
                s_sha[hex_end] = '\0';
                ED_FREE(buf);
            }
        }
    }

    if (!p_open) {
        /* Allow standalone tpl popup to keep working even if about closed. */
        if (s_show_tpl) render_third_party_popup(&s_show_tpl);
        return;
    }

    const char *popup_id = "###AboutDialog";
    if (*p_open && !ImGui::IsPopupOpen(popup_id)) {
        ImGui::OpenPopup(popup_id);
    }

    char _title[256];
    snprintf(_title, sizeof(_title), "%s%s",
             jce_editor_i18n("about.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking)) {
        if (s_show_tpl) render_third_party_popup(&s_show_tpl);
        return;
    }

    ImGui::Text("%s: " JCE_VERSION_STR " (Editor%s)",
                jce_editor_i18n("about.versionLabel"),
#if defined(JCE_BUILD_VARIANT_STR)
                strcmp(JCE_BUILD_VARIANT_STR, "dist") == 0 ? "" : " Preview"
#else
                " Preview"
#endif
    );
    ImGui::Text("%s: %s",
                jce_editor_i18n("about.buildLabel"),
                JCE_BUILD_TIMESTAMP_UTC);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_SECONDARY);
    if (ImGui::BeginTable("##about_build_info", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("##bk", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##bv", ImGuiTableColumnFlags_WidthStretch);

        auto build_row = [](const char *key, const char *val) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(key);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(val);
        };

#if defined(JCE_PLATFORM_STR)
        build_row("Platform", JCE_PLATFORM_STR);
#endif
#if defined(JCE_BUILD_VARIANT_STR)
        build_row("Variant", JCE_BUILD_VARIANT_STR);
#endif
        if (s_sha[0]) {
            /* dist build: show binary SHA-256 (tamper-evident). */
            build_row("SHA-256", s_sha);
        } else {
            /* Non-dist: show source git commit so devs can reproduce the build. */
#if defined(JCE_GIT_COMMIT)
            build_row("Commit", JCE_GIT_COMMIT);
#endif
        }
#if JCE_TRACY_ENABLED
        build_row("Profiling (Tracy)", "Enabled");
#endif
#if defined(JCE_ENABLE_PATENTED_CODECS) && JCE_ENABLE_PATENTED_CODECS
        build_row("Patented Codecs", "AAC \xc2\xb7 H.264 \xc2\xb7 H.265");
#else
        build_row("Patented Codecs", "Off (patent-free only)");
#endif

        ImGui::EndTable();
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Scrollable region — pins buttons to the bottom regardless of text length. */
    const float bottom_reserve = ImGui::GetFrameHeightWithSpacing()
                               + ImGui::GetStyle().ItemSpacing.y + 6.0f;
    ImGui::BeginChild("##about_scroll",
                      ImVec2(0.0f, ImGui::GetContentRegionAvail().y - bottom_reserve),
                      false, ImGuiWindowFlags_None);
    ImGui::TextWrapped("%s", jce_editor_i18n("about.description"));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", jce_editor_i18n("about.platforms"));
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_SECONDARY);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", jce_editor_i18n("about.copyright"));
    ImGui::PopStyleColor();
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("about.thirdParty"), ImVec2(180, 0))) {
        s_show_tpl = true;
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("menu.help.guide"), ImVec2(180, 0))) {
        *jce_editor_panel_visible_ptr(JCE_PANEL_USER_GUIDE) = true;
        jce_editor_panel_request_focus("###user_guide");
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.close"), ImVec2(100, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    if (s_show_tpl) render_third_party_popup(&s_show_tpl);
}
