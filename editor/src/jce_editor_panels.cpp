/*
 * jce_editor_panels.cpp  Shared panel state and lifecycle.
 *
 * Individual panel implementations are in jce_panel_*.cpp files.
 * This file keeps: visibility array, console ring buffer + API,
 * about dialog, preferences (temporary until Settings dialog replaces it).
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "jce_editor_style.h"
#include "jce_editor_config.h"
#include "jce_editor.h"
#include "jce_file_viewer.h"

#include <imgui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#endif

extern "C" {
#include <jce/core/jce_log.h>
#include <jce/app/jce_config.h>
#include <jce/graphics/jce_renderer.h>
}
#include <time.h>

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

/* ══════════════════════════════════════════════════════════════════════
 *  CONSOLE RING BUFFER
 * ══════════════════════════════════════════════════════════════════════ */

#define CONSOLE_MAX_LINES 1024
#define CONSOLE_LINE_LEN  256

typedef struct {
    char            text[CONSOLE_LINE_LEN];
    char            timestamp[16];
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

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t)
        strftime(s_console.lines[idx].timestamp, 16, "%H:%M:%S", t);
    else
        snprintf(s_console.lines[idx].timestamp, 16, "--:--:--");

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
    s_visible[JCE_PANEL_TIMELINE]    = true;
    s_visible[JCE_PANEL_ASSETS]      = true;
    s_visible[JCE_PANEL_FILE_VIEWER] = true;
    s_visible[JCE_PANEL_PREFERENCES] = false;

    /* Console ring buffer. */
    memset(&s_console, 0, sizeof(s_console));

    /* Load persisted editor settings (language, theme, font, renderer). */
    settings_ensure_init();

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

/* Platform-specific renderer backend list. */
#if defined(_WIN32)
static const char *s_renderer_names[] = { "Auto", "D3D12", "D3D11", "Vulkan", "OpenGL" };
static const JceRendererBackend s_renderer_values[] = {
    JCE_BACKEND_AUTO, JCE_BACKEND_D3D12, JCE_BACKEND_D3D11,
    JCE_BACKEND_VULKAN, JCE_BACKEND_OPENGL
};
static const int s_renderer_count = 5;
#elif defined(__APPLE__)
static const char *s_renderer_names[] = { "Auto", "Metal", "OpenGL" };
static const JceRendererBackend s_renderer_values[] = {
    JCE_BACKEND_AUTO, JCE_BACKEND_METAL, JCE_BACKEND_OPENGL
};
static const int s_renderer_count = 3;
#elif defined(__EMSCRIPTEN__)
static const char *s_renderer_names[] = { "Auto", "OpenGL ES" };
static const JceRendererBackend s_renderer_values[] = {
    JCE_BACKEND_AUTO, JCE_BACKEND_OPENGLES
};
static const int s_renderer_count = 2;
#else
static const char *s_renderer_names[] = { "Auto", "Vulkan", "OpenGL" };
static const JceRendererBackend s_renderer_values[] = {
    JCE_BACKEND_AUTO, JCE_BACKEND_VULKAN, JCE_BACKEND_OPENGL
};
static const int s_renderer_count = 3;
#endif

static struct {
    int   language_idx;
    int   theme_idx;
    int   renderer_idx;
    float font_size;
    /* Saved originals for Cancel. */
    int   orig_language_idx;
    int   orig_theme_idx;
    int   orig_renderer_idx;
    float orig_font_size;
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
        /* Language */
        if (strcmp(ecfg.language, "zh_cn") == 0) {
            s_settings.language_idx = 1;
            jce_editor_i18n_set_locale(JCE_LOCALE_ZH_CN);
        } else {
            s_settings.language_idx = 0;
            jce_editor_i18n_set_locale(JCE_LOCALE_EN);
        }

        /* Theme */
        if (strcmp(ecfg.theme, "Light") == 0) s_settings.theme_idx = JCE_THEME_LIGHT;
        else if (strcmp(ecfg.theme, "SSMS") == 0) s_settings.theme_idx = JCE_THEME_SSMS;
        else s_settings.theme_idx = JCE_THEME_DARK;
        jce_editor_apply_theme(s_settings.theme_idx);

        /* Font size */
        if (ecfg.font_size >= 12 && ecfg.font_size <= 48) {
            s_settings.font_size = (float)ecfg.font_size;
            if (s_settings.font_size != jce_editor_get_font_size())
                jce_editor_set_font_size(s_settings.font_size);
        } else {
            s_settings.font_size = jce_editor_get_font_size();
        }

        /* Renderer */
        s_settings.renderer_idx = 0;
        for (int i = 0; i < s_renderer_count; i++) {
            if (strcmp(s_renderer_names[i], ecfg.renderer) == 0) {
                s_settings.renderer_idx = i;
                break;
            }
        }
    } else {
        s_settings.language_idx  = (jce_editor_i18n_get_locale() == JCE_LOCALE_ZH_CN) ? 1 : 0;
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
}

static void settings_apply(void)
{
    /* Language — already applied immediately via Combo callback. */

    /* Theme — already applied immediately via Combo callback. */

    /* Font size — apply if changed. */
    float cur = jce_editor_get_font_size();
    if (s_settings.font_size != cur)
        jce_editor_set_font_size(s_settings.font_size);

    /* Renderer — requires restart; also sync to engine .config/jce.ini. */
    if (s_settings.renderer_idx != s_settings.orig_renderer_idx)
        s_settings.needs_restart = true;

    /* Persist to editor config file. */
    {
        JceEditorConfig ecfg;
        jce_editor_config_load(&ecfg);

        snprintf(ecfg.language, sizeof(ecfg.language), "%s",
                 s_settings.language_idx == 0 ? "en" : "zh_cn");
        ecfg.font_size = (int)s_settings.font_size;

        const char *theme_names[] = { "Dark", "Light", "SSMS" };
        snprintf(ecfg.theme, sizeof(ecfg.theme), "%s",
                 theme_names[s_settings.theme_idx]);

        if (s_settings.renderer_idx >= 0 && s_settings.renderer_idx < s_renderer_count)
            snprintf(ecfg.renderer, sizeof(ecfg.renderer), "%s",
                     s_renderer_names[s_settings.renderer_idx]);

        jce_editor_config_save(&ecfg);
    }

    /* Write renderer backend to .config/jce.ini so the engine picks it up. */
    {
        static const char *backend_ini_names[] = {
#if defined(_WIN32)
            "auto", "d3d12", "d3d11", "vulkan", "opengl"
#elif defined(__APPLE__)
            "auto", "metal", "opengl"
#elif defined(__EMSCRIPTEN__)
            "auto", "opengles"
#else
            "auto", "vulkan", "opengl"
#endif
        };
        const char *be = "auto";
        if (s_settings.renderer_idx >= 0 && s_settings.renderer_idx < s_renderer_count)
            be = backend_ini_names[s_settings.renderer_idx];
#ifdef _WIN32
        _mkdir(".config");
#else
        mkdir(".config", 0755);
#endif
        FILE *ini = fopen(".config/jce.ini", "w");
        if (ini) {
            fprintf(ini, "[renderer]\n");
            fprintf(ini, "backend = %s\n", be);
            fclose(ini);
        }
    }

    /* Update snapshot so Cancel won't revert applied changes. */
    settings_snapshot();

    jce_editor_console_log("Settings applied and saved");
}

static void settings_cancel(void)
{
    /* Revert language. */
    if (s_settings.language_idx != s_settings.orig_language_idx) {
        s_settings.language_idx = s_settings.orig_language_idx;
        jce_editor_i18n_set_locale(
            s_settings.language_idx == 0 ? JCE_LOCALE_EN : JCE_LOCALE_ZH_CN);
    }
    /* Revert theme. */
    if (s_settings.theme_idx != s_settings.orig_theme_idx) {
        s_settings.theme_idx = s_settings.orig_theme_idx;
        jce_editor_apply_theme(s_settings.theme_idx);
    }
    /* Revert font size. */
    if (s_settings.font_size != s_settings.orig_font_size) {
        s_settings.font_size = s_settings.orig_font_size;
        jce_editor_set_font_size(s_settings.orig_font_size);
    }
    /* Revert renderer. */
    s_settings.renderer_idx = s_settings.orig_renderer_idx;
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

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###SettingsDialog",
             jce_editor_i18n("settings.title"));

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::Begin(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("settings.title"));
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                       jce_editor_i18n("settings.description"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    char _lbl[256];

    /* Language */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_lang", jce_editor_i18n("settings.language"));
    const char *languages[] = { "English", "\xe4\xb8\xad\xe6\x96\x87(\xe7\xae\x80\xe4\xbd\x93)" };
    ImGui::PushItemWidth(200);
    if (ImGui::Combo(_lbl, &s_settings.language_idx, languages, 2)) {
        jce_editor_i18n_set_locale(
            s_settings.language_idx == 0 ? JCE_LOCALE_EN : JCE_LOCALE_ZH_CN);
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

    /* Font Size — apply on release to avoid per-frame atlas rebuild. */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_fontsize", jce_editor_i18n("settings.fontSize"));
    ImGui::SliderFloat(_lbl, &s_settings.font_size, 12.0f, 48.0f, "%.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        jce_editor_set_font_size(s_settings.font_size);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.appliesImmediately"));

    /* Renderer Backend (selector) */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_renderer", jce_editor_i18n("settings.renderBackend"));
    if (ImGui::Combo(_lbl, &s_settings.renderer_idx, s_renderer_names, s_renderer_count)) {
        s_settings.needs_restart = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.requiresRestart"));

    /* Current active backend display. */
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "Active: %s",
                       jce_renderer_get_backend_name(NULL));

    ImGui::PopItemWidth();

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
        *p_open = false;
        was_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        settings_cancel();
        *p_open = false;
        was_open = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        settings_cancel();
        *p_open = false;
        was_open = false;
    }

    ImGui::End();
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
    char  status_msg[128];
    float status_timer;
    bool  initialized;
} s_prefs;

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
    snprintf(s_prefs.window_title, sizeof(s_prefs.window_title), "JCE Editor");
    snprintf(s_prefs.assets_path, sizeof(s_prefs.assets_path), "assets");
    snprintf(s_prefs.scenes_path, sizeof(s_prefs.scenes_path), "assets/scenes");
    snprintf(s_prefs.prefabs_path, sizeof(s_prefs.prefabs_path), "assets/prefabs");
    snprintf(s_prefs.build_output_path, sizeof(s_prefs.build_output_path), "build");
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

    if (ImGui::Begin("Preferences###Preferences", vis)) {

        char _lbl[256];

        if (ImGui::BeginTabBar("PrefTabs")) {

            snprintf(_lbl, sizeof(_lbl), "%s###pref_display", jce_editor_i18n("preferences.display.title"));
            if (ImGui::BeginTabItem(_lbl)) {
                snprintf(_lbl, sizeof(_lbl), "%s###winTitle", jce_editor_i18n("preferences.display.windowTitle"));
                ImGui::InputText(_lbl, s_prefs.window_title, sizeof(s_prefs.window_title));
                ImGui::InputInt("Window Width",  &s_prefs.window_width);
                ImGui::InputInt("Window Height", &s_prefs.window_height);
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
            if (ImGui::BeginTabItem(_lbl)) {
                ImGui::Text("Backend: %s", jce_renderer_get_backend_name(NULL));
                const char *msaa[] = { "Off", "2x", "4x", "8x", "16x" };
                ImGui::Combo("MSAA###msaa", &s_prefs.msaa_idx, msaa, 5);
                const char *shadows[] = { "512", "1024", "2048", "4096", "8192" };
                ImGui::Combo("Shadow Map###shadowMap", &s_prefs.shadow_idx, shadows, 5);
                ImGui::Checkbox("HDR###hdr", &s_prefs.hdr);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_audio", jce_editor_i18n("preferences.audio.title"));
            if (ImGui::BeginTabItem(_lbl)) {
                ImGui::SliderFloat("Master###master", &s_prefs.master_vol, 0.0f, 1.0f);
                ImGui::SliderFloat("Music###music",  &s_prefs.music_vol,  0.0f, 1.0f);
                ImGui::SliderFloat("SFX###sfx",    &s_prefs.sfx_vol,    0.0f, 1.0f);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_physics", jce_editor_i18n("preferences.physics.title"));
            if (ImGui::BeginTabItem(_lbl)) {
                ImGui::SliderInt("Substeps###substeps", &s_prefs.physics_substeps,
                                 JCE_PREF_PHYSICS_SUBSTEP_MIN, JCE_PREF_PHYSICS_SUBSTEP_MAX);
                ImGui::Checkbox("Debug Geometry###debugGeom", &s_prefs.debug_physics);
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_editor", jce_editor_i18n("preferences.editorTab.title"));
            if (ImGui::BeginTabItem(_lbl)) {
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
                const char *languages[] = { "English", "Chinese" };
                snprintf(_lbl, sizeof(_lbl), "%s###language", jce_editor_i18n("preferences.language"));
                if (ImGui::Combo(_lbl, &s_prefs.language_idx, languages, 2)) {
                    jce_editor_i18n_set_locale(
                        s_prefs.language_idx == 0 ? JCE_LOCALE_EN : JCE_LOCALE_ZH_CN);
                }
                ImGui::EndTabItem();
            }

            snprintf(_lbl, sizeof(_lbl), "%s###pref_paths", jce_editor_i18n("preferences.paths.title"));
            if (ImGui::BeginTabItem(_lbl)) {
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                                   jce_editor_i18n("preferences.paths.projectPaths"));
                ImGui::Spacing();
                snprintf(_lbl, sizeof(_lbl), "%s###assetsPath", jce_editor_i18n("preferences.paths.assetsPath"));
                ImGui::InputText(_lbl, s_prefs.assets_path, sizeof(s_prefs.assets_path));
                snprintf(_lbl, sizeof(_lbl), "%s###scenesPath", jce_editor_i18n("preferences.paths.scenesPath"));
                ImGui::InputText(_lbl, s_prefs.scenes_path, sizeof(s_prefs.scenes_path));
                snprintf(_lbl, sizeof(_lbl), "%s###prefabsPath", jce_editor_i18n("preferences.paths.prefabsPath"));
                ImGui::InputText(_lbl, s_prefs.prefabs_path, sizeof(s_prefs.prefabs_path));
                snprintf(_lbl, sizeof(_lbl), "%s###buildPath", jce_editor_i18n("preferences.paths.buildOutputPath"));
                ImGui::InputText(_lbl, s_prefs.build_output_path, sizeof(s_prefs.build_output_path));
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();

        if (ImGui::Button(jce_editor_i18n("preferences.buttons.save"))) {
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

void jce_editor_about_dialog(bool *p_open)
{
    if (!p_open || !*p_open) return;

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###AboutDialog",
             jce_editor_i18n("about.title"));

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::Begin(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("about.title"));
    ImGui::Spacing();
    ImGui::Text("Version: 0.3.0 (Editor Preview)");
    ImGui::Text("Build: %s %s", __DATE__, __TIME__);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", jce_editor_i18n("about.description"));
    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
        "Platforms: Windows, macOS, Linux, iOS, Android, Web");
    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("about.copyright"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("dialog.close"), ImVec2(100, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *p_open = false;
    }

    ImGui::End();
}
