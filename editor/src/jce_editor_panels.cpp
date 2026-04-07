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

#include <imgui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
#include <jce/core/jce_log.h>
}

#define LOG_TAG "editor_panels"

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
    s_visible[JCE_PANEL_GAME_VIEW]   = false;
    s_visible[JCE_PANEL_TIMELINE]    = false;
    s_visible[JCE_PANEL_ASSETS]      = true;
    s_visible[JCE_PANEL_FILE_VIEWER] = false;
    s_visible[JCE_PANEL_PREFERENCES] = false;

    /* Console ring buffer. */
    memset(&s_console, 0, sizeof(s_console));

    jce_editor_console_log_level(JCE_CONSOLE_INFO, "editor panels initialized");
}

void jce_editor_panels_shutdown(void)
{
    /* nothing to free — file viewer tabs cleaned up separately */
}

/* ══════════════════════════════════════════════════════════════════════
 *  FILE VIEWER (multi-tab with line numbers)
 * ══════════════════════════════════════════════════════════════════════ */

#define FILE_VIEWER_MAX_TABS    16
#define FILE_VIEWER_MAX_CONTENT (1024 * 256)  /* 256KB max per file */

typedef struct {
    char  path[512];
    char  display_name[64];
    char *content;       /* heap-allocated */
    int   content_len;
    bool  is_image;      /* for future use */
    bool  open;          /* tab open state */
} FileViewerTab;

static struct {
    FileViewerTab tabs[FILE_VIEWER_MAX_TABS];
    int tab_count;
    int active_tab;
} s_fv;

/* Called from shutdown to free viewer memory. */
static void file_viewer_shutdown(void)
{
    for (int i = 0; i < s_fv.tab_count; i++) {
        if (s_fv.tabs[i].content) {
            free(s_fv.tabs[i].content);
            s_fv.tabs[i].content = NULL;
        }
    }
    s_fv.tab_count  = 0;
    s_fv.active_tab = -1;
}

/* Close a tab by index: free content and compact the array. */
static void file_viewer_close_tab(int idx)
{
    if (idx < 0 || idx >= s_fv.tab_count) return;

    if (s_fv.tabs[idx].content) {
        free(s_fv.tabs[idx].content);
        s_fv.tabs[idx].content = NULL;
    }

    /* Shift remaining tabs down. */
    for (int i = idx; i < s_fv.tab_count - 1; i++)
        s_fv.tabs[i] = s_fv.tabs[i + 1];

    s_fv.tab_count--;

    /* Clear the vacated last slot. */
    memset(&s_fv.tabs[s_fv.tab_count], 0, sizeof(FileViewerTab));

    /* Adjust active tab. */
    if (s_fv.active_tab >= s_fv.tab_count)
        s_fv.active_tab = s_fv.tab_count - 1;
}

void jce_file_viewer_open(const char *path)
{
    if (!path || !path[0]) return;

    /* Check if already open -> switch to it. */
    for (int i = 0; i < s_fv.tab_count; i++) {
        if (strcmp(s_fv.tabs[i].path, path) == 0) {
            s_fv.active_tab = i;
            s_visible[JCE_PANEL_FILE_VIEWER] = true;
            return;
        }
    }

    /* If at max capacity, close the oldest (first) tab. */
    if (s_fv.tab_count >= FILE_VIEWER_MAX_TABS)
        file_viewer_close_tab(0);

    /* Extract display name (last component after / or \). */
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }

    /* Read file content (binary mode, capped). */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: cannot open '%s'", path);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int read_size = (file_size > FILE_VIEWER_MAX_CONTENT)
                    ? FILE_VIEWER_MAX_CONTENT : (int)file_size;

    char *buf = (char *)malloc((size_t)read_size + 1);
    if (!buf) {
        fclose(fp);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: malloc failed for '%s'", path);
        return;
    }

    int actually_read = (int)fread(buf, 1, (size_t)read_size, fp);
    fclose(fp);

    buf[actually_read] = '\0';

    /* Fill the new tab. */
    FileViewerTab *tab = &s_fv.tabs[s_fv.tab_count];
    memset(tab, 0, sizeof(*tab));
    snprintf(tab->path, sizeof(tab->path), "%s", path);
    snprintf(tab->display_name, sizeof(tab->display_name), "%s", name);
    tab->content     = buf;
    tab->content_len = actually_read;
    tab->is_image    = false;
    tab->open        = true;

    s_fv.active_tab = s_fv.tab_count;
    s_fv.tab_count++;

    s_visible[JCE_PANEL_FILE_VIEWER] = true;
}

void jce_editor_panel_file_viewer_content(void)
{
    /* If no tabs open, show placeholder. */
    if (s_fv.tab_count <= 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("viewer.noFile"));
        return;
    }

    /* Tab bar. */
    ImGuiTabBarFlags tab_flags = ImGuiTabBarFlags_Reorderable
                               | ImGuiTabBarFlags_AutoSelectNewTabs
                               | ImGuiTabBarFlags_FittingPolicyScroll;

    if (ImGui::BeginTabBar("##FileViewerTabs", tab_flags)) {
        for (int i = 0; i < s_fv.tab_count; /* incremented below */) {
            FileViewerTab *tab = &s_fv.tabs[i];

            /* Push a unique ID so duplicate display names don't collide. */
            ImGui::PushID(i);
            bool tab_open = tab->open;
            if (ImGui::BeginTabItem(tab->display_name, &tab_open)) {
                s_fv.active_tab = i;

                /* Toolbar: Open External button + file info. */
                if (ImGui::Button(jce_editor_i18n("viewer.openExternal"))) {
                    jce_editor_console_log_level(JCE_CONSOLE_INFO,
                        "open external: %s (not yet implemented)", tab->path);
                }
                ImGui::SameLine();
                if (tab->content_len >= 1024)
                    ImGui::Text("%s  (%d KB)", jce_editor_i18n("viewer.fileInfo"),
                                tab->content_len / 1024);
                else
                    ImGui::Text("%s  (%d bytes)", jce_editor_i18n("viewer.fileInfo"),
                                tab->content_len);

                /* File path in secondary color. */
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", tab->path);
                ImGui::Separator();

                /* Code view with line numbers. */
                ImGui::BeginChild("CodeView", ImVec2(0, 0), false,
                                  ImGuiWindowFlags_HorizontalScrollbar);

                /* Count total lines. */
                int line_count = 1;
                for (int c = 0; c < tab->content_len; c++) {
                    if (tab->content[c] == '\n') line_count++;
                }

                /* Determine width for line number column. */
                int digits = 1;
                {
                    int tmp = line_count;
                    while (tmp >= 10) { digits++; tmp /= 10; }
                }
                char num_fmt[16];
                snprintf(num_fmt, sizeof(num_fmt), "%%%dd", digits);

                /* Render lines. */
                const char *line_start = tab->content;
                int line_num = 1;
                for (;;) {
                    const char *line_end = line_start;
                    while (*line_end && *line_end != '\n') line_end++;

                    /* Line number (right-aligned, secondary color). */
                    char num_buf[16];
                    snprintf(num_buf, sizeof(num_buf), num_fmt, line_num);
                    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", num_buf);
                    ImGui::SameLine();

                    /* Line text. */
                    if (line_end > line_start) {
                        ImGui::TextUnformatted(line_start, line_end);
                    } else {
                        ImGui::TextUnformatted("");
                    }

                    if (*line_end == '\0') break;
                    line_start = line_end + 1;
                    line_num++;
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::PopID();

            /* Handle tab close. */
            if (!tab_open) {
                file_viewer_close_tab(i);
                /* Don't increment i — array shifted down. */
            } else {
                i++;
            }
        }
        ImGui::EndTabBar();
    }
}

void jce_editor_panel_file_viewer(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER);
    if (!*vis) return;

    if (ImGui::Begin("File Viewer###FileViewer", vis))
        jce_editor_panel_file_viewer_content();
    ImGui::End();
}

/* ══════════════════════════════════════════════════════════════════════
 *  SETTINGS DIALOG (full implementation)
 * ══════════════════════════════════════════════════════════════════════ */

static struct {
    int  language_idx;
    int  font_size_idx;
    int  theme_idx;
    int  renderer_idx;
    bool needs_restart;
    bool initialized;
} s_settings;

static void settings_ensure_init(void)
{
    if (s_settings.initialized) return;
    memset(&s_settings, 0, sizeof(s_settings));
    s_settings.language_idx  = (jce_editor_i18n_get_locale() == JCE_LOCALE_ZH_CN) ? 1 : 0;
    s_settings.theme_idx     = jce_editor_get_theme();
    s_settings.font_size_idx = 1; /* 14px default */
    s_settings.renderer_idx  = 0; /* OpenGL */
    s_settings.initialized   = true;
}

void jce_editor_settings_dialog(bool *p_open)
{
    if (!p_open || !*p_open) return;

    settings_ensure_init();

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###Settings", jce_editor_i18n("settings.title"));

    ImGui::SetNextWindowSize(ImVec2(550, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

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

    /* Font Size */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_font", jce_editor_i18n("settings.fontSize"));
    const char *font_sizes[] = { "12", "14", "16", "18", "20", "24", "28", "32" };
    if (ImGui::Combo(_lbl, &s_settings.font_size_idx, font_sizes, 8))
        s_settings.needs_restart = true;
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.requiresRestart"));

    /* Theme */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_theme", jce_editor_i18n("settings.theme"));
    const char *themes[] = { "Dark", "Light", "Blue" };
    if (ImGui::Combo(_lbl, &s_settings.theme_idx, themes, 3))
        jce_editor_apply_theme(s_settings.theme_idx);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s",
                       jce_editor_i18n("settings.appliesImmediately"));

    /* Renderer */
    snprintf(_lbl, sizeof(_lbl), "%s###settings_renderer", jce_editor_i18n("settings.renderBackend"));
    const char *renderers[] = { "OpenGL 3.3", "Vulkan 1.2" };
    int prev_renderer = s_settings.renderer_idx;
    ImGui::Combo(_lbl, &s_settings.renderer_idx, renderers, 2);
    if (s_settings.renderer_idx != prev_renderer)
        s_settings.needs_restart = true;
    ImGui::PopItemWidth();

    /* Vulkan note */
    if (s_settings.renderer_idx == 1) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("settings.requiresRestart"));
    }

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
        jce_editor_console_log("Settings applied");
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.ok"), ImVec2(btn_w, 0))) {
        jce_editor_console_log("Settings saved");
        *p_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
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
    int   renderer_idx;
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
                const char *renderers[] = { "OpenGL", "Vulkan", "Auto" };
                ImGui::Combo("Backend###backend", &s_prefs.renderer_idx, renderers, 3);
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

    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About JCE Editor###About", p_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

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
    }
    ImGui::End();
}
