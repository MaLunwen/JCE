/*
 * jce_editor_panels.cpp  Shared panel state and lifecycle.
 *
 * Individual panel implementations are in jce_panel_*.cpp files.
 * This file keeps: visibility array, console ring buffer + API,
 * about dialog, renderer backend list, and the boot restore of the
 * persisted locale/theme.  (Settings editing lives in the Preferences
 * panel, jce_panel_preferences.cpp — the old duplicate Settings dialog
 * was retired.)
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

/* Forward declaration: boot restore of persisted locale/theme. */
static void settings_restore_locale_and_theme(void);

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
    /* Record how many panels the mask was built against.  Bit i maps to
     * enum value i, so a build with MORE panels (appended entries) must
     * apply only the low saved-count bits and keep code defaults for the
     * new ones — without this the load treated absent high bits as
     * "hidden", turning every newly added default-on panel off. */
    jce_editor_config_set_ui_int(&_cfg, "panels.visible_count",
                                 (int)JCE_PANEL_COUNT);
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

    /* Restore persisted locale + theme (locale restore is load-bearing:
     * skipping it boots the editor in the default language). */
    settings_restore_locale_and_theme();

    /* Restore window panel visibility from editor-config (overrides defaults
       set above). Sentinel JCE_EDITOR_PANELS_MASK_UNSET means "never saved",
       which keeps the per-panel defaults. */
    {
        JceEditorConfig _ecfg;
        if (jce_editor_config_load(&_ecfg) &&
            _ecfg.panels_visible_mask != JCE_EDITOR_PANELS_MASK_UNSET) {
            uint64_t mask = ((uint64_t)_ecfg.panels_visible_mask_hi << 32) |
                            (uint64_t)_ecfg.panels_visible_mask;
            /* Apply only the bits the saving build actually wrote (see
             * persist_visibility): panels appended since then keep their
             * code defaults instead of inheriting "hidden".  Legacy configs
             * without the count key behave as before (full width). */
            int saved_count = jce_editor_config_get_ui_int_or(
                &_ecfg, "panels.visible_count", 64);
            if (saved_count < 1)  saved_count = 1;
            if (saved_count > 64) saved_count = 64;
            for (int i = 0; i < JCE_PANEL_COUNT && i < saved_count; i++) {
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
 *  RENDERER BACKEND LIST + BOOT SETTINGS RESTORE
 * ══════════════════════════════════════════════════════════════════════ */

/* Renderer backend list — populated at runtime from compiled-in
   bgfx backends via jce_renderer_caps_list_backends().  Falls back
   to a single "Auto" entry if the renderer has not been initialized
   yet (e.g. queried before the first frame). */
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

/* ── Boot restore of persisted locale / theme ─────────────────────────
 *
 * Historically this was the lazy init (settings_ensure_init) of a
 * duplicate Settings dialog that had zero callers; the dialog is retired
 * (the Preferences panel is the single settings surface), but the restore
 * itself is load-bearing: without it the editor boots in the default
 * locale regardless of the persisted language.  Called once from
 * jce_editor_panels_init(). */
static void settings_restore_locale_and_theme(void)
{
    JceEditorConfig ecfg;
    if (!jce_editor_config_load(&ecfg))
        return;

    /* Language — derived from the persisted stable code (handles any
       number of locales without index-magic). */
    jce_editor_i18n_set_locale(jce_editor_i18n_locale_from_code(ecfg.language));

    /* Theme — shared canonical parser (accepts legacy "SSMS" for Blue). */
    jce_editor_apply_theme(jce_editor_theme_from_string(ecfg.theme));
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
