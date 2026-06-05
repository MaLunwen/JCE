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
    const char *themes[] = { "Dark", "Light", "Blue" };
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
 *  PREFERENCES PANEL (temporary — will be replaced by Settings dialog)
 * ══════════════════════════════════════════════════════════════════════ */

static struct {
    int   window_width;
    int   window_height;
    char  window_title[128];
    bool  fullscreen;
    bool  vsync;
    int   target_fps;
    int   msaa_idx;
    int   shadow_idx;
    bool  hdr;
    float master_vol;
    float music_vol;
    float sfx_vol;
    int   physics_substeps;
    bool  debug_physics;
    bool  show_grid;
    bool  show_gizmos;
    float gizmo_scale;
    float camera_sensitivity;
    int   language_idx;
    char  assets_path[512];
    char  scenes_path[512];
    char  prefabs_path[512];
    char  build_output_path[512];
    char  font_en_path[512];
    char  font_zh_path[512];
    /* Quality */
    int   quality_preset;
    float lod_bias;
    int   texture_quality_idx;
    int   anisotropic_idx;
    float shadow_distance;
    int   max_pixel_lights;
    bool  soft_particles;
    bool  realtime_reflections;
    /* Time */
    float time_scale;
    float fixed_dt;
    float max_dt;
    bool  pause_when_unfocused;
    /* Graphics (tier render-pipeline knobs) */
    int   gfx_tier;
    bool  gfx_dynamic_resolution;
    int   gfx_color_space_idx;       /* 0 = Linear, 1 = Gamma */
    bool  gfx_async_compute;
    /* Tags & Layers */
    char  tags_csv[512];             /* comma-separated tag names */
    char  layers_csv[512];           /* comma-separated layer names */
    char  sorting_layers_csv[512];
    /* Player */
    char  player_company[128];
    char  player_product[128];
    char  player_version[32];
    int   player_orientation_idx;    /* 0 = Auto, 1 = Portrait, 2 = Landscape */
    bool  player_run_in_background;
    bool  player_resizable_window;
    char  status_msg[128];
    float status_timer;
    bool  initialized;
} s_prefs;

static int  s_prefs_tab = 0;
static int  s_prefs_request_tab = -1;
static bool s_prefs_tab_state_loaded = false;

static const char *k_prefs_tab_state_key = "panel.preferences.current_tab";

static void prefs_ensure_tab_state_loaded(void)
{
    if (s_prefs_tab_state_loaded)
        return;

    s_prefs_tab = jce_editor_ui_state_load_int(k_prefs_tab_state_key, 0, 0, 11);
    s_prefs_request_tab = s_prefs_tab;
    s_prefs_tab_state_loaded = true;
}

static ImGuiTabItemFlags prefs_tab_flags(int idx)
{
    return (s_prefs_request_tab == idx) ? ImGuiTabItemFlags_SetSelected : 0;
}

static void prefs_select_tab(int idx)
{
    if (idx < 0 || idx > 11 || s_prefs_tab == idx)
        return;

    s_prefs_tab = idx;
    if (s_prefs_tab_state_loaded)
        jce_editor_ui_state_save_int(k_prefs_tab_state_key, idx);
}

static void prefs_ensure_init(void)
{
    if (s_prefs.initialized) return;
    memset(&s_prefs, 0, sizeof(s_prefs));
    s_prefs.window_width       = JCE_WINDOW_WIDTH;
    s_prefs.window_height      = JCE_WINDOW_HEIGHT;
    s_prefs.vsync              = true;
    s_prefs.target_fps         = JCE_TARGET_FPS;
    s_prefs.msaa_idx           = 2;
    s_prefs.shadow_idx         = 2;
    s_prefs.master_vol         = 1.0f;
    s_prefs.music_vol          = 0.8f;
    s_prefs.sfx_vol            = 1.0f;
    s_prefs.physics_substeps   = 4;
    s_prefs.show_grid          = true;
    s_prefs.show_gizmos        = true;
    s_prefs.gizmo_scale        = 1.0f;
    s_prefs.camera_sensitivity = 1.0f;
    s_prefs.quality_preset       = 2;
    s_prefs.lod_bias             = 1.0f;
    s_prefs.texture_quality_idx  = 0;
    s_prefs.anisotropic_idx      = 2;
    s_prefs.shadow_distance      = 50.0f;
    s_prefs.max_pixel_lights     = 4;
    s_prefs.soft_particles       = true;
    s_prefs.realtime_reflections = false;
    s_prefs.time_scale           = 1.0f;
    s_prefs.fixed_dt             = 0.02f;
    s_prefs.max_dt               = 0.1f;
    s_prefs.pause_when_unfocused = false;
    s_prefs.gfx_tier               = 1;          /* Mid */
    s_prefs.gfx_dynamic_resolution = false;
    s_prefs.gfx_color_space_idx    = 0;          /* Linear */
    s_prefs.gfx_async_compute      = false;
    snprintf(s_prefs.tags_csv,           sizeof(s_prefs.tags_csv),
             "Untagged,Player,Enemy,MainCamera,Respawn,Finish");
    snprintf(s_prefs.layers_csv,         sizeof(s_prefs.layers_csv),
             "Default,TransparentFX,Ignore Raycast,Water,UI");
    snprintf(s_prefs.sorting_layers_csv, sizeof(s_prefs.sorting_layers_csv),
             "Default");
    snprintf(s_prefs.player_company, sizeof(s_prefs.player_company), "DefaultCompany");
    snprintf(s_prefs.player_product, sizeof(s_prefs.player_product), "JCE Game");
    snprintf(s_prefs.player_version, sizeof(s_prefs.player_version), "0.1.0");
    s_prefs.player_orientation_idx   = 0;
    s_prefs.player_run_in_background = false;
    s_prefs.player_resizable_window  = true;
    snprintf(s_prefs.window_title, sizeof(s_prefs.window_title), "JCE Editor");
    snprintf(s_prefs.assets_path, sizeof(s_prefs.assets_path), "assets");
    snprintf(s_prefs.scenes_path, sizeof(s_prefs.scenes_path), "assets/scenes");
    snprintf(s_prefs.prefabs_path, sizeof(s_prefs.prefabs_path), "assets/prefabs");
    snprintf(s_prefs.build_output_path, sizeof(s_prefs.build_output_path), "build");
    s_prefs.language_idx = (int)jce_editor_i18n_get_locale();

    /* Pull persisted font overrides from editor config. */
    JceEditorConfig _ecfg;
    if (jce_editor_config_load(&_ecfg)) {
        snprintf(s_prefs.font_en_path, sizeof(s_prefs.font_en_path),
                 "%s", _ecfg.font_en_path);
        snprintf(s_prefs.font_zh_path, sizeof(s_prefs.font_zh_path),
                 "%s", _ecfg.font_zh_path);
    }
    s_prefs.initialized        = true;
}

bool jce_editor_prefs_show_gizmos(void)
{
    prefs_ensure_init();
    return s_prefs.show_gizmos;
}

float jce_editor_prefs_gizmo_scale(void)
{
    prefs_ensure_init();
    return s_prefs.gizmo_scale;
}

void jce_editor_panel_preferences(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PREFERENCES);
    if (!*vis) return;

    prefs_ensure_init();
    prefs_ensure_tab_state_loaded();

    char panel_title[256];
    snprintf(panel_title, sizeof(panel_title), "%s###Preferences",
             jce_editor_i18n("preferences.title"));
    if (ImGui::Begin(panel_title, vis, ImGuiWindowFlags_NoFocusOnAppearing)) {

        char _lbl[256];

        if (ImGui::BeginTabBar("PrefTabs")) {

            snprintf(_lbl, sizeof(_lbl), "%s###pref_display", jce_editor_i18n("preferences.display.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(0))) {
                prefs_select_tab(0);
                snprintf(_lbl, sizeof(_lbl), "%s###winTitle", jce_editor_i18n("preferences.display.windowTitle"));
                ImGui::InputText(_lbl, s_prefs.window_title, sizeof(s_prefs.window_title));
                snprintf(_lbl, sizeof(_lbl), "%s###windowWidth", jce_editor_i18n("preferences.display.windowWidth"));
                ImGui::InputInt(_lbl,  &s_prefs.window_width);
                snprintf(_lbl, sizeof(_lbl), "%s###windowHeight", jce_editor_i18n("preferences.display.windowHeight"));
                ImGui::InputInt(_lbl, &s_prefs.window_height);
                snprintf(_lbl, sizeof(_lbl), "%s###fullscreen", jce_editor_i18n("preferences.display.fullscreen"));
                ImGui::Checkbox(_lbl, &s_prefs.fullscreen);
                snprintf(_lbl, sizeof(_lbl), "%s###vsync", jce_editor_i18n("preferences.display.vsync"));
                ImGui::Checkbox(_lbl, &s_prefs.vsync);
                snprintf(_lbl, sizeof(_lbl), "%s###targetFps", jce_editor_i18n("preferences.display.targetFps"));
                ImGui::SliderInt(_lbl, &s_prefs.target_fps,
                                 JCE_PREF_FPS_MIN, JCE_PREF_FPS_MAX);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_rendering", jce_editor_i18n("preferences.rendering.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(1))) {
                prefs_select_tab(1);
                ImGui::Text("%s: %s", jce_editor_i18n("preferences.rendering.backend"),
                            jce_renderer_get_backend_name(NULL));
                const char *msaa[] = { "Off", "2x", "4x", "8x", "16x" };
                snprintf(_lbl, sizeof(_lbl), "%s###msaa", jce_editor_i18n("preferences.rendering.msaa"));
                ImGui::Combo(_lbl, &s_prefs.msaa_idx, msaa, 5);
                const char *shadows[] = { "512", "1024", "2048", "4096", "8192" };
                snprintf(_lbl, sizeof(_lbl), "%s###shadowMap", jce_editor_i18n("preferences.rendering.shadowMapSize"));
                ImGui::Combo(_lbl, &s_prefs.shadow_idx, shadows, 5);
                snprintf(_lbl, sizeof(_lbl), "%s###hdr", jce_editor_i18n("preferences.rendering.hdr"));
                ImGui::Checkbox(_lbl, &s_prefs.hdr);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_audio", jce_editor_i18n("preferences.audio.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(2))) {
                prefs_select_tab(2);
                snprintf(_lbl, sizeof(_lbl), "%s###master", jce_editor_i18n("preferences.audio.masterVolume"));
                ImGui::SliderFloat(_lbl, &s_prefs.master_vol, 0.0f, 1.0f);
                snprintf(_lbl, sizeof(_lbl), "%s###music", jce_editor_i18n("preferences.audio.musicVolume"));
                ImGui::SliderFloat(_lbl,  &s_prefs.music_vol,  0.0f, 1.0f);
                snprintf(_lbl, sizeof(_lbl), "%s###sfx", jce_editor_i18n("preferences.audio.sfxVolume"));
                ImGui::SliderFloat(_lbl,    &s_prefs.sfx_vol,    0.0f, 1.0f);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_physics", jce_editor_i18n("preferences.physics.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(3))) {
                prefs_select_tab(3);
                snprintf(_lbl, sizeof(_lbl), "%s###substeps", jce_editor_i18n("preferences.physics.substeps"));
                ImGui::SliderInt(_lbl, &s_prefs.physics_substeps,
                                 JCE_PREF_PHYSICS_SUBSTEP_MIN, JCE_PREF_PHYSICS_SUBSTEP_MAX);
                snprintf(_lbl, sizeof(_lbl), "%s###debugGeom", jce_editor_i18n("preferences.physics.debugGeometry"));
                ImGui::Checkbox(_lbl, &s_prefs.debug_physics);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_quality", jce_editor_i18n("preferences.quality.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(4))) {
                prefs_select_tab(4);
                const char *presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };
                snprintf(_lbl, sizeof(_lbl), "%s###qpreset", jce_editor_i18n("preferences.quality.preset"));
                if (ImGui::Combo(_lbl, &s_prefs.quality_preset, presets, 5)
                    && s_prefs.quality_preset != 4 /* Custom */) {
                    /* Apply tier defaults so users can see the spread. */
                    static const struct {
                        float lod;  int tex; int aniso; float sdist;
                        int   maxL; bool soft; bool refl;
                    } P[4] = {
                        { 2.0f, 3, 0, 15.0f, 2, false, false }, /* Low */
                        { 1.5f, 1, 1, 30.0f, 3, true,  false }, /* Medium */
                        { 1.0f, 0, 2, 50.0f, 4, true,  false }, /* High */
                        { 0.5f, 0, 4, 80.0f, 8, true,  true  }, /* Ultra */
                    };
                    int qi = s_prefs.quality_preset;
                    s_prefs.lod_bias             = P[qi].lod;
                    s_prefs.texture_quality_idx  = P[qi].tex;
                    s_prefs.anisotropic_idx      = P[qi].aniso;
                    s_prefs.shadow_distance      = P[qi].sdist;
                    s_prefs.max_pixel_lights     = P[qi].maxL;
                    s_prefs.soft_particles       = P[qi].soft;
                    s_prefs.realtime_reflections = P[qi].refl;
                }
                snprintf(_lbl, sizeof(_lbl), "%s###lodbias", jce_editor_i18n("preferences.quality.lodBias"));
                ImGui::SliderFloat(_lbl, &s_prefs.lod_bias, 0.1f, 4.0f);
                const char *texq[] = { "Full", "Half", "Quarter", "Eighth" };
                snprintf(_lbl, sizeof(_lbl), "%s###texq", jce_editor_i18n("preferences.quality.textureQuality"));
                ImGui::Combo(_lbl, &s_prefs.texture_quality_idx, texq, 4);
                const char *aniso[] = { "Off", "2x", "4x", "8x", "16x" };
                snprintf(_lbl, sizeof(_lbl), "%s###aniso", jce_editor_i18n("preferences.quality.anisotropic"));
                ImGui::Combo(_lbl, &s_prefs.anisotropic_idx, aniso, 5);
                snprintf(_lbl, sizeof(_lbl), "%s###sdist", jce_editor_i18n("preferences.quality.shadowDistance"));
                ImGui::SliderFloat(_lbl, &s_prefs.shadow_distance, 1.0f, 500.0f);
                snprintf(_lbl, sizeof(_lbl), "%s###maxlights", jce_editor_i18n("preferences.quality.maxPixelLights"));
                ImGui::SliderInt(_lbl, &s_prefs.max_pixel_lights, 1, 16);
                snprintf(_lbl, sizeof(_lbl), "%s###softp", jce_editor_i18n("preferences.quality.softParticles"));
                ImGui::Checkbox(_lbl, &s_prefs.soft_particles);
                snprintf(_lbl, sizeof(_lbl), "%s###rtrefl", jce_editor_i18n("preferences.quality.realtimeReflections"));
                ImGui::Checkbox(_lbl, &s_prefs.realtime_reflections);
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.quality.notWired"));
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_time", jce_editor_i18n("preferences.time.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(5))) {
                prefs_select_tab(5);
                snprintf(_lbl, sizeof(_lbl), "%s###tscale", jce_editor_i18n("preferences.time.timeScale"));
                ImGui::SliderFloat(_lbl, &s_prefs.time_scale, 0.0f, 4.0f);
                snprintf(_lbl, sizeof(_lbl), "%s###fixed", jce_editor_i18n("preferences.time.fixedTimestep"));
                ImGui::SliderFloat(_lbl, &s_prefs.fixed_dt, 0.001f, 0.1f, "%.4f s");
                snprintf(_lbl, sizeof(_lbl), "%s###maxdt", jce_editor_i18n("preferences.time.maxDeltaTime"));
                ImGui::SliderFloat(_lbl, &s_prefs.max_dt, 0.01f, 1.0f, "%.3f s");
                snprintf(_lbl, sizeof(_lbl), "%s###pwu", jce_editor_i18n("preferences.time.pauseWhenUnfocused"));
                ImGui::Checkbox(_lbl, &s_prefs.pause_when_unfocused);
                if (s_prefs.fixed_dt > 0.0f) {
                    ImGui::TextDisabled("≈ %.1f Hz", 1.0 / (double)s_prefs.fixed_dt);
                }
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.time.notWired"));
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_graphics", jce_editor_i18n("preferences.graphics.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(6))) {
                prefs_select_tab(6);
                const char *tiers[] = { "Tier 1 (Low)", "Tier 2 (Mid)", "Tier 3 (High)" };
                if (s_prefs.gfx_tier < 0 || s_prefs.gfx_tier > 2) s_prefs.gfx_tier = 1;
                snprintf(_lbl, sizeof(_lbl), "%s###gfxTier", jce_editor_i18n("preferences.graphics.tier"));
                ImGui::Combo(_lbl, &s_prefs.gfx_tier, tiers, 3);
                const char *cs[] = { "Linear", "Gamma" };
                if (s_prefs.gfx_color_space_idx < 0 || s_prefs.gfx_color_space_idx > 1)
                    s_prefs.gfx_color_space_idx = 0;
                snprintf(_lbl, sizeof(_lbl), "%s###gfxCS", jce_editor_i18n("preferences.graphics.colorSpace"));
                ImGui::Combo(_lbl, &s_prefs.gfx_color_space_idx, cs, 2);
                snprintf(_lbl, sizeof(_lbl), "%s###gfxDR", jce_editor_i18n("preferences.graphics.dynamicResolution"));
                ImGui::Checkbox(_lbl, &s_prefs.gfx_dynamic_resolution);
                snprintf(_lbl, sizeof(_lbl), "%s###gfxAC", jce_editor_i18n("preferences.graphics.asyncCompute"));
                ImGui::Checkbox(_lbl, &s_prefs.gfx_async_compute);
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.graphics.notWired"));
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_tagslayers", jce_editor_i18n("preferences.tagsLayers.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(7))) {
                prefs_select_tab(7);
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.tagsLayers.help"));
                snprintf(_lbl, sizeof(_lbl), "%s###tagsCSV", jce_editor_i18n("preferences.tagsLayers.tags"));
                ImGui::InputTextMultiline(_lbl, s_prefs.tags_csv,
                    sizeof(s_prefs.tags_csv), ImVec2(-1, 60));
                snprintf(_lbl, sizeof(_lbl), "%s###layersCSV", jce_editor_i18n("preferences.tagsLayers.layers"));
                ImGui::InputTextMultiline(_lbl, s_prefs.layers_csv,
                    sizeof(s_prefs.layers_csv), ImVec2(-1, 60));
                snprintf(_lbl, sizeof(_lbl), "%s###sortLayCSV", jce_editor_i18n("preferences.tagsLayers.sortingLayers"));
                ImGui::InputTextMultiline(_lbl, s_prefs.sorting_layers_csv,
                    sizeof(s_prefs.sorting_layers_csv), ImVec2(-1, 40));
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.tagsLayers.notWired"));
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_player", jce_editor_i18n("preferences.player.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(8))) {
                prefs_select_tab(8);
                snprintf(_lbl, sizeof(_lbl), "%s###plyCompany", jce_editor_i18n("preferences.player.company"));
                ImGui::InputText(_lbl, s_prefs.player_company, sizeof(s_prefs.player_company));
                snprintf(_lbl, sizeof(_lbl), "%s###plyProduct", jce_editor_i18n("preferences.player.product"));
                ImGui::InputText(_lbl, s_prefs.player_product, sizeof(s_prefs.player_product));
                snprintf(_lbl, sizeof(_lbl), "%s###plyVer", jce_editor_i18n("preferences.player.version"));
                ImGui::InputText(_lbl, s_prefs.player_version, sizeof(s_prefs.player_version));
                const char *orients[] = { "Auto", "Portrait", "Landscape" };
                if (s_prefs.player_orientation_idx < 0 || s_prefs.player_orientation_idx > 2)
                    s_prefs.player_orientation_idx = 0;
                snprintf(_lbl, sizeof(_lbl), "%s###plyOri", jce_editor_i18n("preferences.player.orientation"));
                ImGui::Combo(_lbl, &s_prefs.player_orientation_idx, orients, 3);
                snprintf(_lbl, sizeof(_lbl), "%s###plyBg", jce_editor_i18n("preferences.player.runInBackground"));
                ImGui::Checkbox(_lbl, &s_prefs.player_run_in_background);
                snprintf(_lbl, sizeof(_lbl), "%s###plyRz", jce_editor_i18n("preferences.player.resizableWindow"));
                ImGui::Checkbox(_lbl, &s_prefs.player_resizable_window);
                ImGui::TextDisabled("%s", jce_editor_i18n("preferences.player.notWired"));
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_editor", jce_editor_i18n("preferences.editorTab.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(9))) {
                prefs_select_tab(9);
                snprintf(_lbl, sizeof(_lbl), "%s###showGrid", jce_editor_i18n("preferences.editorTab.showGrid"));
                ImGui::Checkbox(_lbl, &s_prefs.show_grid);
                snprintf(_lbl, sizeof(_lbl), "%s###showGizmos", jce_editor_i18n("preferences.editorTab.showGizmos"));
                ImGui::Checkbox(_lbl, &s_prefs.show_gizmos);
                snprintf(_lbl, sizeof(_lbl), "%s###gizmoScale", jce_editor_i18n("preferences.editorTab.gizmoScale"));
                ImGui::SliderFloat(_lbl, &s_prefs.gizmo_scale,
                                   JCE_PREF_GIZMO_SCALE_MIN, JCE_PREF_GIZMO_SCALE_MAX);
                snprintf(_lbl, sizeof(_lbl), "%s###camSens", jce_editor_i18n("preferences.editorTab.cameraSensitivity"));
                ImGui::SliderFloat(_lbl, &s_prefs.camera_sensitivity,
                                   JCE_PREF_CAM_SENS_MIN, JCE_PREF_CAM_SENS_MAX);
                ImGui::Separator();
                /* Input direction preferences — clearly grouped so users
                   distinguish mouse wheel from touchpad. All three live-bind
                   to editor-config and persist immediately on toggle. */
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                                   jce_editor_i18n("preferences.input.mouseGroup"));
                snprintf(_lbl, sizeof(_lbl), "%s###invScrollZoom",
                         jce_editor_i18n("preferences.input.invertScrollZoom"));
                if (ImGui::Checkbox(_lbl, &jce_editor_pref_invert_scroll_zoom)) {
                    bool _new = jce_editor_pref_invert_scroll_zoom;
                    JceEditorConfig _cfg = {};
                    jce_editor_config_load(&_cfg);
                    _cfg.invert_scroll_zoom = _new;
                    jce_editor_config_save(&_cfg);
                    jce_editor_pref_invert_scroll_zoom = _new;
                }
                snprintf(_lbl, sizeof(_lbl), "%s###invDragY",
                         jce_editor_i18n("preferences.input.invertDragY"));
                if (ImGui::Checkbox(_lbl, &jce_editor_pref_invert_drag_y)) {
                    bool _new = jce_editor_pref_invert_drag_y;
                    JceEditorConfig _cfg = {};
                    jce_editor_config_load(&_cfg);
                    _cfg.invert_drag_y = _new;
                    jce_editor_config_save(&_cfg);
                    jce_editor_pref_invert_drag_y = _new;
                }
                ImGui::Spacing();
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                                   jce_editor_i18n("preferences.input.touchpadGroup"));
                snprintf(_lbl, sizeof(_lbl), "%s###touchpadHInv",
                         jce_editor_i18n("preferences.editorTab.touchpadHInvert"));
                if (ImGui::Checkbox(_lbl, &jce_editor_pref_touchpad_h_invert)) {
                    bool _new = jce_editor_pref_touchpad_h_invert;
                    JceEditorConfig _cfg = {};
                    jce_editor_config_load(&_cfg);
                    _cfg.touchpad_h_invert = _new;
                    jce_editor_config_save(&_cfg);
                    jce_editor_pref_touchpad_h_invert = _new;
                }
                ImGui::Separator();
                const int   _n_loc = jce_editor_i18n_locale_count();
                const char *languages[JCE_MAX_LOCALES];
                for (int i = 0; i < _n_loc; i++)
                    languages[i] = jce_editor_i18n_locale_native_name((JceLocale)i);
                snprintf(_lbl, sizeof(_lbl), "%s###language", jce_editor_i18n("preferences.language"));
                if (ImGui::Combo(_lbl, &s_prefs.language_idx, languages, _n_loc)) {
                    jce_editor_i18n_set_locale((JceLocale)s_prefs.language_idx);
                    s_settings.language_idx = s_prefs.language_idx;
                    settings_apply();
                }
                /* Theme — live apply + persist via shared s_settings/apply path. */
                settings_ensure_init();
                const char *themes[] = { "Dark", "Light", "Blue" };
                snprintf(_lbl, sizeof(_lbl), "%s###pref_theme",
                         jce_editor_i18n("settings.theme"));
                if (ImGui::Combo(_lbl, &s_settings.theme_idx, themes, 3)) {
                    jce_editor_apply_theme(s_settings.theme_idx);
                    settings_apply();
                }
                /* Font size — requires restart, but persists immediately. */
                snprintf(_lbl, sizeof(_lbl), "%s###pref_fontsize",
                         jce_editor_i18n("settings.fontSize"));
                if (ImGui::SliderFloat(_lbl, &s_settings.font_size,
                                       12.0f, 48.0f, "%.0f px")) {
                    s_settings.needs_restart = true;
                    settings_apply();
                }
                if (s_settings.needs_restart) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
                                       jce_editor_i18n("settings.requiresRestart"));
                }
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_paths", jce_editor_i18n("preferences.paths.title"));
            if (ImGui::BeginTabItem(_lbl, nullptr, prefs_tab_flags(10))) {
                prefs_select_tab(10);
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                                   jce_editor_i18n("preferences.paths.projectPaths"));
                ImGui::Spacing();
                snprintf(_lbl, sizeof(_lbl), "%s###assetsPath", jce_editor_i18n("preferences.paths.assetsPath"));
                jce_draw_path_input(_lbl, s_prefs.assets_path, sizeof(s_prefs.assets_path), JcePathKind::FolderAbs);
                snprintf(_lbl, sizeof(_lbl), "%s###scenesPath", jce_editor_i18n("preferences.paths.scenesPath"));
                jce_draw_path_input(_lbl, s_prefs.scenes_path, sizeof(s_prefs.scenes_path), JcePathKind::FolderAbs);
                snprintf(_lbl, sizeof(_lbl), "%s###prefabsPath", jce_editor_i18n("preferences.paths.prefabsPath"));
                jce_draw_path_input(_lbl, s_prefs.prefabs_path, sizeof(s_prefs.prefabs_path), JcePathKind::FolderAbs);
                snprintf(_lbl, sizeof(_lbl), "%s###buildPath", jce_editor_i18n("preferences.paths.buildOutputPath"));
                jce_draw_path_input(_lbl, s_prefs.build_output_path, sizeof(s_prefs.build_output_path), JcePathKind::FolderAbs);
                ImGui::EndTabItem();
            }

            /* Fonts tab: user-supplied font overrides (e.g. system Ink Free /
             * KaiTi on Windows). Empty string -> use bundled OFL fallback.
             * Changes apply on next editor restart. */
            if (ImGui::BeginTabItem(jce_editor_i18n_id("preferences.tab.fonts", "pref_fonts"),
                                    nullptr, prefs_tab_flags(11))) {
                prefs_select_tab(11);
                ImGui::TextWrapped("%s", jce_editor_i18n("preferences.fonts.help"));
                ImGui::Spacing();
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("preferences.fonts.latin"));
                jce_draw_path_input_file("##fontEn", s_prefs.font_en_path, sizeof(s_prefs.font_en_path), "Font (*.ttf *.otf *.ttc);;All Files (*.*)");
                ImGui::SameLine();
                if (ImGui::SmallButton(jce_editor_i18n_id("preferences.fonts.clear", "fontEnClr"))) s_prefs.font_en_path[0] = '\0';
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("preferences.fonts.cjk"));
                jce_draw_path_input_file("##fontZh", s_prefs.font_zh_path, sizeof(s_prefs.font_zh_path), "Font (*.ttf *.otf *.ttc);;All Files (*.*)");
                ImGui::SameLine();
                if (ImGui::SmallButton(jce_editor_i18n_id("preferences.fonts.clear", "fontZhClr"))) s_prefs.font_zh_path[0] = '\0';
                ImGui::Spacing();
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("preferences.fonts.hintWindows"));
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("preferences.fonts.hintMac"));
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("preferences.fonts.hintLinux"));
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
            s_prefs_request_tab = -1;
        }

        ImGui::Separator();

        if (ImGui::Button(jce_editor_i18n("preferences.buttons.save"))) {
            JceEditorConfig _ecfg;
            jce_editor_config_load(&_ecfg);
            snprintf(_ecfg.font_en_path, sizeof(_ecfg.font_en_path),
                     "%s", s_prefs.font_en_path);
            snprintf(_ecfg.font_zh_path, sizeof(_ecfg.font_zh_path),
                     "%s", s_prefs.font_zh_path);
            jce_editor_config_save(&_ecfg);
            snprintf(s_prefs.status_msg, sizeof(s_prefs.status_msg),
                     "%s", jce_editor_i18n("preferences.status.saved"));
            s_prefs.status_timer = 3.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("preferences.buttons.reset"))) {
            s_prefs.target_fps = JCE_TARGET_FPS;
            s_prefs.gizmo_scale = 1.0f;
            s_prefs.camera_sensitivity = 1.0f;
            snprintf(s_prefs.status_msg, sizeof(s_prefs.status_msg),
                     "%s", jce_editor_i18n("preferences.status.reset"));
            s_prefs.status_timer = 3.0f;
        }

        if (s_prefs.status_timer > 0.0f) {
            ImGui::SameLine();
            ImGui::TextColored(JCE_COLOR_TEXT_SUCCESS, "%s", s_prefs.status_msg);
            s_prefs.status_timer -= ImGui::GetIO().DeltaTime;
        }
    }
    ImGui::End();
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
    if (ImGui::Button(jce_editor_i18n("dialog.close"), ImVec2(100, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *p_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    if (s_show_tpl) render_third_party_popup(&s_show_tpl);
}
