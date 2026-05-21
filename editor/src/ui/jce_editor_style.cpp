/*
 * jce_editor_style.cpp  Dark editor theme and font loading.
 *
 * Matches the reference Java editor's dark theme and style.
 */

#include "jce_editor_style.h"

#include "jce_editor_colors.h"
#include "core/jce_editor_alloc.h"
#include <jce/ui/jce_imgui_renderer.h>

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_str.h>
}

#define LOG_TAG "editor_style"

static int s_current_theme = JCE_THEME_DARK;
static float s_baked_font_size = 14.0f;  /* last size passed to load_fonts() */

/* ── Style parameters (shared by all themes) ──────────────────────── */

static void apply_style_params(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.FrameRounding     = 2.0f;
    s.ScrollbarRounding = 3.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 2.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(8, 8);
    s.FramePadding      = ImVec2(6, 4);
    s.ItemSpacing       = ImVec2(8, 4);
    s.ItemInnerSpacing  = ImVec2(4, 4);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 8.0f;
}

/* ── Dark theme (matches reference EditorUI.applyDarkTheme) ────────── */

static void apply_dark_theme(void)
{
    /* Reset all color slots first to avoid stale values when switching from
       other themes that override additional ImGuiCol entries. */
    ImGui::StyleColorsDark();

    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    c[ImGuiCol_Header]                = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    c[ImGuiCol_Separator]             = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    c[ImGuiCol_Tab]                   = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.33f, 0.34f, 0.36f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.23f, 0.23f, 0.24f, 1.00f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.68f);
}

/* ── Light theme ──────────────────────────────────────────────────── */

static void apply_light_theme(void)
{
    /* Start from full light defaults, then apply JCE-tuned overrides. */
    ImGui::StyleColorsLight();

    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.70f, 0.70f, 0.70f, 0.50f);
    c[ImGuiCol_FrameBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.90f, 0.90f, 0.90f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.46f, 0.54f, 0.80f, 0.60f);
    c[ImGuiCol_Button]                = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.70f, 0.70f, 0.70f, 0.50f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    c[ImGuiCol_Tab]                   = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
}

/* ── SSMS theme (SQL Server Management Studio style) ──────────────── */

static void apply_ssms_theme(void)
{
    /* SSMS theme is derived from light defaults. */
    ImGui::StyleColorsLight();

    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.969f, 0.976f, 0.996f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.969f, 0.976f, 0.996f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.969f, 0.976f, 0.996f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.251f, 0.314f, 0.553f, 0.60f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.365f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.365f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.365f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.365f, 0.420f, 0.600f, 0.80f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.365f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.365f, 0.420f, 0.600f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.800f, 0.835f, 0.941f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.365f, 0.420f, 0.600f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.251f, 0.314f, 0.553f, 0.95f);
    c[ImGuiCol_Tab]                   = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.365f, 0.420f, 0.600f, 0.80f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.969f, 0.976f, 0.996f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.365f, 0.420f, 0.600f, 0.70f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.365f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.800f, 0.835f, 0.941f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.800f, 0.835f, 0.941f, 0.60f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.937f, 0.957f, 0.976f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.365f, 0.420f, 0.600f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.251f, 0.314f, 0.553f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.251f, 0.314f, 0.553f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.62f);
}

/* ── Public API ───────────────────────────────────────────────────── */

void jce_editor_setup_style(void)
{
    apply_style_params();
    apply_dark_theme();
    s_current_theme = JCE_THEME_DARK;
    LOG_INFO(LOG_TAG, "dark theme applied (reference style)");
}

void jce_editor_apply_theme(int theme_idx)
{
    apply_style_params();
    switch (theme_idx) {
    case JCE_THEME_LIGHT: apply_light_theme(); break;
    case JCE_THEME_SSMS:  apply_ssms_theme();  break;
    default:              apply_dark_theme();   theme_idx = JCE_THEME_DARK; break;
    }
    s_current_theme = theme_idx;

    /* Dedupe identical re-applies — apply_theme is invoked from multiple
     * sites during startup (config load → style init → font reload).
     * Log only on actual theme change. */
    static int s_last_logged_theme = -1;
    if (s_last_logged_theme != theme_idx) {
        const char *names[] = { "Dark", "Light", "SSMS" };
        LOG_INFO(LOG_TAG, "%s theme applied", names[theme_idx]);
        s_last_logged_theme = theme_idx;
    }
}

int jce_editor_get_theme(void)
{
    return s_current_theme;
}

/* ── Font loading ──────────────────────────────────────────────────── */

/* ── Font resolution helpers ────────────────────────────────────────
 *
 * Editor fonts are resolved in this priority order:
 *   1. User override path (from JceEditorConfig.font_en_path / _zh_path)
 *   2. System font installed on the host OS (Ink Free / KaiTi)
 *   3. ImGui built-in default (proggy) — no fonts are bundled by JCE
 *
 * System font lookup uses runtime platform detection via jce_platform_name()
 * — no compile-time #ifdef branches. We probe a fixed list of well-known
 * filesystem locations per platform string. Loading the file off the
 * user's machine is legal because the user holds a license to use it
 * (e.g. their Windows install ships Ink Free); we never bundle these
 * files in the editor PAK.
 */

static bool file_exists_readable(const char *path)
{
    return jce_fs_host_exists_file(path);
}

/* Read a TTF/OTF font file fully into a heap buffer.
   On success returns a malloc'd buffer (caller transfers ownership to
   ImGui via FontDataOwnedByAtlas) and writes the size to *out_size. */
static void *read_font_file(const char *path, size_t *out_size)
{
    if (!path || !*path) return NULL;
    uint64_t n = 0;
    void *raw = jce_fs_host_read_all(path, &n);
    if (!raw || n == 0) {
        if (raw) jce_free(raw);
        return NULL;
    }
    /* ImGui frees FontDataOwnedByAtlas via IM_FREE, which is wired to
     * ED_FREE in jce_editor_init() — so allocate via ED_MALLOC here. */
    void *buf = ED_MALLOC((size_t)n);
    if (!buf) { jce_free(raw); return NULL; }
    memcpy(buf, raw, (size_t)n);
    jce_free(raw);
    if (out_size) *out_size = (size_t)n;
    return buf;
}

/* Build a candidate path for a system font by joining a directory with
   a filename. Empty dir or empty file -> empty result. */
static void join_path(char *out, size_t out_size,
                      const char *dir, const char *file)
{
    if (!dir || !*dir || !file || !*file) { out[0] = '\0'; return; }
    snprintf(out, out_size, "%s/%s", dir, file);
}

/* Look up a system-installed font by family. Tries platform-specific
   well-known locations and returns the first one that exists.
   `family` is one of: "InkFree", "KaiTi". Returns true if found,
   writing the absolute path to `out`. */
static bool find_system_font(const char *family, char *out, size_t out_size)
{
    out[0] = '\0';
    const char *plat = jce_platform_name();   /* "Windows" / "macOS" / "Linux" / ... */

    /* Map family name to per-platform candidate filenames.
       Filenames are documented OS install names; if the user has the
       font installed it lives at one of these paths. */
    const char *win_file = NULL;
    const char *mac_file = NULL;
    const char *linux_file = NULL;
    if (jce_strcasecmp(family, "InkFree") == 0) {
        win_file   = "inkfree.ttf";
        mac_file   = "Ink Free.ttf";   /* uncommon on macOS, harmless probe */
        linux_file = "InkFree.ttf";    /* uncommon on Linux, harmless probe */
    } else if (jce_strcasecmp(family, "KaiTi") == 0) {
        win_file   = "simkai.ttf";
        mac_file   = "Kaiti.ttc";
        linux_file = "ukai.ttc";       /* AR PL UKai (common Linux Kaiti) */
    } else {
        /* Generic family lookup: treat `family` as a filename stem and
         * probe common font extensions in the per-platform font dirs.
         * This is how the Korean / Japanese / Latin fallback chains
         * actually resolve (e.g. "malgun" -> "malgun.ttf" in C:\Windows
         * \Fonts, "msyh" -> "msyh.ttc", "AppleSDGothicNeo" -> ".ttc"
         * under /System/Library/Fonts). Without this branch the
         * fallback walk silently no-ops and Hangul renders as ?. */
        static const char *exts[] = { ".ttf", ".ttc", ".otf", NULL };
        char tryname[160];
        char candidate2[1024];

        if (strcmp(plat, "Windows") == 0) {
            const char *windir = getenv("WINDIR");
            if (!windir || !*windir) windir = getenv("SystemRoot");
            if (windir && *windir) {
                char fontdir[1024];
                snprintf(fontdir, sizeof(fontdir), "%s/Fonts", windir);
                for (int e = 0; exts[e]; ++e) {
                    snprintf(tryname, sizeof(tryname), "%s%s",
                             family, exts[e]);
                    join_path(candidate2, sizeof(candidate2),
                              fontdir, tryname);
                    if (file_exists_readable(candidate2)) {
                        jce_strlcpy(out, candidate2, out_size);
                        return true;
                    }
                }
            }
        } else if (strcmp(plat, "macOS") == 0) {
            const char *dirs[] = {
                "/System/Library/Fonts/Supplemental",
                "/System/Library/Fonts",
                "/Library/Fonts",
                NULL };
            for (int i = 0; dirs[i]; ++i) {
                for (int e = 0; exts[e]; ++e) {
                    snprintf(tryname, sizeof(tryname), "%s%s",
                             family, exts[e]);
                    join_path(candidate2, sizeof(candidate2),
                              dirs[i], tryname);
                    if (file_exists_readable(candidate2)) {
                        jce_strlcpy(out, candidate2, out_size);
                        return true;
                    }
                }
            }
        } else if (strcmp(plat, "Linux") == 0) {
            const char *dirs[] = {
                "/usr/share/fonts/truetype",
                "/usr/share/fonts/opentype",
                "/usr/share/fonts",
                "/usr/local/share/fonts",
                NULL };
            for (int i = 0; dirs[i]; ++i) {
                for (int e = 0; exts[e]; ++e) {
                    snprintf(tryname, sizeof(tryname), "%s%s",
                             family, exts[e]);
                    join_path(candidate2, sizeof(candidate2),
                              dirs[i], tryname);
                    if (file_exists_readable(candidate2)) {
                        jce_strlcpy(out, candidate2, out_size);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    char candidate[1024];

    if (strcmp(plat, "Windows") == 0) {
        const char *windir = getenv("WINDIR");
        if (!windir || !*windir) windir = getenv("SystemRoot");
        if (windir && *windir) {
            join_path(candidate, sizeof(candidate),
                      windir, "Fonts");
            char fontdir[1024];
            snprintf(fontdir, sizeof(fontdir), "%s", candidate);
            join_path(candidate, sizeof(candidate), fontdir, win_file);
            if (file_exists_readable(candidate)) {
                jce_strlcpy(out, candidate, out_size);
                return true;
            }
        }
    } else if (strcmp(plat, "macOS") == 0) {
        const char *dirs[] = {
            "/System/Library/Fonts/Supplemental",
            "/System/Library/Fonts",
            "/Library/Fonts",
            NULL
        };
        for (int i = 0; dirs[i]; ++i) {
            join_path(candidate, sizeof(candidate), dirs[i], mac_file);
            if (file_exists_readable(candidate)) {
                jce_strlcpy(out, candidate, out_size);
                return true;
            }
        }
    } else if (strcmp(plat, "Linux") == 0) {
        const char *dirs[] = {
            "/usr/share/fonts/truetype/arphic",
            "/usr/share/fonts/truetype",
            "/usr/share/fonts",
            "/usr/local/share/fonts",
            NULL
        };
        for (int i = 0; dirs[i]; ++i) {
            join_path(candidate, sizeof(candidate), dirs[i], linux_file);
            if (file_exists_readable(candidate)) {
                jce_strlcpy(out, candidate, out_size);
                return true;
            }
        }
    }
    return false;
}

/* Resolve final font path for a logical role.
   override -> system_family -> NULL (caller falls back to PAK).
   Writes to out_path; returns true if a filesystem path was resolved. */
static bool resolve_font_path(const char *override_path,
                              const char *system_family,
                              char *out_path, size_t out_size,
                              const char **out_source)
{
    out_path[0] = '\0';
    if (override_path && *override_path && file_exists_readable(override_path)) {
        jce_strlcpy(out_path, override_path, out_size);
        if (out_source) *out_source = "user override";
        return true;
    }
    if (find_system_font(system_family, out_path, out_size)) {
        if (out_source) *out_source = "system font";
        return true;
    }
    if (out_source) *out_source = "bundled fallback";
    return false;
}

/* Helper: try to load a font from override -> system. Returns the loaded
   ImFont* (or NULL if neither path resolves) and logs the resolution.
   When `quiet_on_miss` is true, no per-attempt WARN is emitted on miss
   (used by the Icons fallback walk where misses are expected). */
static ImFont *load_font_with_fallback(
    const char *role,                  /* "Latin" / "CJK" / "Icons" */
    const char *override_path,
    const char *system_family,
    float size_pixels,
    const ImFontConfig *cfg,
    const ImWchar *ranges,
    bool quiet_on_miss = false)
{
    ImGuiIO &io = ImGui::GetIO();

    char fs_path[1024];
    const char *source = NULL;
    bool from_fs = resolve_font_path(override_path, system_family,
                                     fs_path, sizeof(fs_path), &source);

    if (from_fs) {
        size_t n = 0;
        void *buf = read_font_file(fs_path, &n);
        if (buf) {
            ImFont *f = io.Fonts->AddFontFromMemoryTTF(
                buf, (int)n, size_pixels, cfg, ranges);
            if (f) {
                LOG_INFO(LOG_TAG, "%s font: %s (%s)",
                         role, fs_path, source);
                return f;
            }
            LOG_WARN(LOG_TAG, "%s font load failed for %s",
                     role, fs_path);
            ED_FREE(buf);
        } else {
            LOG_WARN(LOG_TAG, "%s font: cannot read %s", role, fs_path);
        }
    }

    if (!quiet_on_miss) {
        LOG_WARN(LOG_TAG, "%s font unavailable (no override, no system match); "
                          "ImGui default will be used", role);
    }
    return NULL;
}

bool jce_editor_load_fonts(const JcePakArchive *pak, float size_pixels,
                           const char *en_override, const char *zh_override)
{
    /* Cache pak across reloads so the deferred-reload path (font size
       change in Preferences passes pak=NULL) still has access to i18n
       JSON for the codepoint scan. */
    static const JcePakArchive *s_pak_cached = NULL;
    if (pak) s_pak_cached = pak;
    const JcePakArchive *use_pak = pak ? pak : s_pak_cached;

    ImGuiIO &io = ImGui::GetIO();

    /* CRITICAL: clear the atlas before re-adding. Without this, repeat
       calls (e.g. from Project Settings 'Save') leave the previous Latin
       and CJK fonts in the atlas and append new ones, then re-point
       io.FontDefault. If the second pass fails to resolve CJK (e.g. user
       cleared the override and KaiTi isn't installed), the new default
       font has only Latin glyphs and every CJK character renders as ?.
       Clearing forces a clean rebuild that walks the full fallback chain. */
    io.Fonts->Clear();

    /* --- Latin font -------------------------------------------------- */
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;

    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin Supplement */
        0x2000, 0x206F,   /* General Punctuation */
        0x2190, 0x21FF,   /* Arrows */
        0x2500, 0x25FF,   /* Box Drawing + Block Elements + Geometric Shapes (◆◉○) */
        0x2600, 0x26FF,   /* Misc Symbols (☼☀ etc.) */
        0,
    };

    ImFont *font = load_font_with_fallback(
        "Latin", en_override, "InkFree",
        size_pixels, &cfg, latin_ranges);

    /* Latin fallback chain: try widely-installed sans-serifs if Ink Free
       is missing, so the editor renders in something readable instead
       of falling back to the proggy bitmap. */
    if (!font) {
        const char *latin_fallbacks[] = {
            "segoeui", "Arial", "Helvetica", "DejaVuSans", NULL };
        for (int i = 0; latin_fallbacks[i] && !font; i++) {
            font = load_font_with_fallback(
                "Latin", NULL, latin_fallbacks[i],
                size_pixels, &cfg, latin_ranges);
        }
    }

    if (!font) {
        /* No override, no system font. Add ImGui's built-in proggy font so
           the atlas isn't empty and we can still merge a CJK font onto it. */
        font = io.Fonts->AddFontDefault();
        LOG_INFO(LOG_TAG, "Latin font: ImGui built-in default (proggy)");
    }

    /* --- CJK font (merged into the Latin font) ----------------------- */
    ImFontConfig merge_cfg;
    merge_cfg.FontDataOwnedByAtlas = true;
    merge_cfg.OversampleH = 2;
    merge_cfg.OversampleV = 2;
    merge_cfg.PixelSnapH  = true;
    merge_cfg.MergeMode   = true;

    static const ImWchar cjk_extra_ranges[] = {
        /* Backup ranges for arrows + box drawing + geometric shapes +
           misc symbols. Most CJK fonts (msyh, simhei, simsun, KaiTi)
           contain these glyphs, so merging them here ensures icons like
           ◆ ◉ ○ render even when the Latin font (e.g. Ink Free) lacks
           them. ImGui's per-glyph fallback walks the merged fonts. */
        0x2190, 0x21FF,   /* Arrows */
        0x2500, 0x25FF,   /* Box Drawing + Block + Geometric Shapes */
        0x2600, 0x26FF,   /* Misc Symbols */
        0,
    };

    /* Build the actual CJK glyph set lazily.
       Previously we baked 0x4E00–0x9FFF (~20 000 CJK Unified Ideographs)
       which dominated atlas-build time (~1.7 s @ 24 px). Now we scan
       the i18n JSON files in the PAK and add only the codepoints that
       actually appear in editor strings. This keeps the atlas tiny
       while guaranteeing every translatable string renders correctly
       (no missing glyphs like 轴 or 管). A small built-in fallback set
       ("ChineseSimplifiedCommon") is also merged so user-typed scene
       names / asset names with common Chinese characters still render
       even if the string isn't in the i18n table. */
    ImFontGlyphRangesBuilder cjk_builder;
    cjk_builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    /* Korean Hangul Syllables + Jamo — covers the entire ko.json domain
       even if a particular CJK font happens to lack glyphs for an
       infrequent character.  Cost: ~11k codepoints, ~50 ms one-time
       atlas bake; negligible at startup. */
    cjk_builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
    cjk_builder.AddRanges(cjk_extra_ranges);

    /* Scan i18n PAK files: for every UTF-8 codepoint encountered, mark
       it as required. This is fast (~1 ms per file) and exact. */
    if (use_pak) {
        const char *i18n_paths[] = {
            "i18n/en.json", "i18n/zh_cn.json", "i18n/ko.json", NULL
        };
        for (int i = 0; i18n_paths[i]; i++) {
            const JcePakAsset *a = jce_pak_find(use_pak, i18n_paths[i]);
            if (!a) continue;
            char *buf = (char *)jce_malloc((size_t)a->original_size + 1);
            if (!buf) continue;
            size_t n = jce_pak_decompress(a, buf, (size_t)a->original_size);
            if (n > 0) {
                buf[n] = '\0';
                const unsigned char *p = (const unsigned char *)buf;
                const unsigned char *end = p + n;
                while (p < end) {
                    /* Inline UTF-8 decoder: returns codepoint and
                       advances `p`. Handles 1/2/3/4-byte sequences. */
                    unsigned int cp = 0;
                    unsigned char c = *p;
                    int adv = 1;
                    if (c < 0x80) {
                        cp = c;
                    } else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
                        cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                        adv = 2;
                    } else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
                        cp = ((c & 0x0F) << 12) |
                             ((p[1] & 0x3F) << 6) |
                             (p[2] & 0x3F);
                        adv = 3;
                    } else if ((c & 0xF8) == 0xF0 && p + 3 < end) {
                        cp = ((c & 0x07) << 18) |
                             ((p[1] & 0x3F) << 12) |
                             ((p[2] & 0x3F) << 6) |
                             (p[3] & 0x3F);
                        adv = 4;
                    }
                    if (cp >= 0x80 && cp <= 0xFFFF)
                        cjk_builder.AddChar((ImWchar)cp);
                    p += adv;
                }
            }
            jce_free(buf);
        }
    }

    static ImVector<ImWchar> cjk_ranges_v;
    cjk_ranges_v.clear();
    cjk_builder.BuildRanges(&cjk_ranges_v);
    const ImWchar *cjk_ranges = cjk_ranges_v.Data;

    /* CJK fallback chain: try a list of commonly-installed CJK fonts so
       Chinese / Japanese text doesn't show as tofu when the preferred
       family (KaiTi / simkai.ttf) is absent. */
    ImFont *cjk = load_font_with_fallback(
        "CJK", zh_override, "KaiTi",
        size_pixels, &merge_cfg, cjk_ranges);
    if (!cjk) {
        const char *cjk_fallbacks[] = {
            "msyh",        /* Microsoft YaHei */
            "simhei",      /* SimHei */
            "simsun",      /* SimSun */
            "msjh",        /* JhengHei */
            "PingFang",    /* macOS */
            "STHeiti",     /* macOS */
            "Hiragino",    /* macOS */
            "wqy-microhei",/* Linux */
            "wqy-zenhei",
            "NotoSansCJK", "NotoSans",
            NULL };
        for (int i = 0; cjk_fallbacks[i] && !cjk; i++) {
            cjk = load_font_with_fallback(
                "CJK", NULL, cjk_fallbacks[i],
                size_pixels, &merge_cfg, cjk_ranges);
        }
    }

    /* --- Korean font (merged) --------------------------------------- */
    /* KaiTi (default zh_override) and many Chinese-first CJK fonts ship
       *no* Hangul glyphs, so ko.json text renders as ? marks once the
       user switches the editor locale to Korean. Force-merge a
       Korean-capable system font onto the atlas so Hangul resolves
       regardless of which Chinese fallback won above. The list mirrors
       common defaults per platform: Malgun Gothic / Gulim / Batang on
       Windows; AppleSDGothicNeo on macOS; NotoSansCJK-KR / NanumGothic
       on Linux. Quiet-on-miss so users without any Korean font installed
       still don't get spurious warnings (translation just falls back). */
    static const ImWchar korean_ranges[] = {
        0x1100, 0x11FF,   /* Hangul Jamo */
        0x3130, 0x318F,   /* Hangul Compatibility Jamo */
        0xA960, 0xA97F,   /* Hangul Jamo Extended-A */
        0xAC00, 0xD7AF,   /* Hangul Syllables (the bulk) */
        0xD7B0, 0xD7FF,   /* Hangul Jamo Extended-B */
        0,
    };
    const char *korean_fallbacks[] = {
        "malgun",                 /* Malgun Gothic — Windows default */
        "malgunbd",
        "gulim",                  /* Gulim / GulimChe */
        "batang",                 /* Batang / BatangChe */
        "dotum",
        "AppleSDGothicNeo",       /* macOS */
        "AppleGothic",
        "NotoSansCJK-KR",         /* Linux */
        "NotoSansKR",
        "NanumGothic",
        "NanumMyeongjo",
        NULL };
    for (int i = 0; korean_fallbacks[i]; i++) {
        ImFont *kf = load_font_with_fallback(
            "Korean", NULL, korean_fallbacks[i],
            size_pixels, &merge_cfg, korean_ranges,
            /*quiet_on_miss=*/true);
        if (kf) break;
    }

    /* --- Icon font (merged) ----------------------------------------- */
    /* Dedicated icon-coverage merge pass: KaiTi and many other CJK fonts
       LACK U+25C6 / U+25C9 / U+25CB even though their character chart
       claims the range. Force-merge a font that demonstrably ships the
       Geometric Shapes block (Segoe UI Symbol on Windows, Apple Symbols
       on macOS, DejaVu Sans on Linux) so the hierarchy panel's diamond
       and eye icons resolve regardless of which CJK fallback won. */
    static const ImWchar icon_ranges[] = {
        0x2190, 0x21FF,   /* Arrows */
        0x2500, 0x25FF,   /* Box Drawing + Geometric Shapes */
        0x2600, 0x26FF,   /* Misc Symbols */
        0x2700, 0x27BF,   /* Dingbats */
        0,
    };
    const char *icon_fallbacks[] = {
        "seguisym",       /* Segoe UI Symbol (Win) */
        "segoeui",        /* Segoe UI also covers most icon glyphs */
        "Apple Symbols",  /* macOS */
        "DejaVuSans",     /* Linux */
        "msyh", "simhei", "simsun",
        NULL };
    for (int i = 0; icon_fallbacks[i]; i++) {
        ImFont *ic = load_font_with_fallback(
            "Icons", NULL, icon_fallbacks[i],
            size_pixels, &merge_cfg, icon_ranges,
            /*quiet_on_miss=*/true);
        if (ic) break;
    }

    io.FontDefault = font;

    /* ImGui 1.92 introduced dynamic font sizing: text is rendered at
     * style.FontSizeBase * (FontScaleMain * FontScaleDpi). Adding a
     * font at a given size only sets its baked / "legacy" size — the
     * displayed size is governed by FontSizeBase. Mirror it here so
     * font_size changes from Preferences and Project Settings actually
     * affect the on-screen pixel height instead of just the atlas. */
    ImGui::GetStyle().FontSizeBase = size_pixels;
    s_baked_font_size = size_pixels;

    /* Rebuild font atlas on bgfx side. */
    jce_imgui_renderer_rebuild_fonts();

    LOG_SUCCESS(LOG_TAG, "loaded fonts (%.0f px, Latin + CJK)", size_pixels);
    return true;
}

/* ──────────────────────────────────────────────────────────────────────
 *  FONT DISCOVERY (for the Settings dropdown picker)
 * ──────────────────────────────────────────────────────────────────── */

/* Common font filename -> human-readable family. Keeps the picker
   readable instead of showing cryptic filenames like "msyh.ttc". */
static const char *prettify_font_name(const char *fname)
{
    static const struct { const char *file; const char *pretty; } map[] = {
        /* Windows */
        { "inkfree.ttf",        "Ink Free" },
        { "simkai.ttf",         "KaiTi" },
        { "simhei.ttf",         "SimHei (黑体)" },
        { "simsun.ttc",         "SimSun (宋体)" },
        { "simfang.ttf",        "FangSong (仿宋)" },
        { "simli.ttf",          "LiSu (隶书)" },
        { "simyou.ttf",         "YouYuan (幼圆)" },
        { "msyh.ttc",           "Microsoft YaHei (微软雅黑)" },
        { "msyhbd.ttc",         "Microsoft YaHei Bold" },
        { "msyhl.ttc",          "Microsoft YaHei Light" },
        { "msjh.ttc",           "Microsoft JhengHei (正黑)" },
        { "mingliu.ttc",        "MingLiU (細明體)" },
        { "yumin.ttf",          "YuMincho" },
        { "yugothm.ttc",        "YuGothic Medium" },
        { "msgothic.ttc",       "MS Gothic" },
        { "msmincho.ttc",       "MS Mincho" },
        { "malgun.ttf",         "Malgun Gothic" },
        { "consola.ttf",        "Consolas" },
        { "cour.ttf",           "Courier New" },
        { "arial.ttf",          "Arial" },
        { "calibri.ttf",        "Calibri" },
        { "segoeui.ttf",        "Segoe UI" },
        { "tahoma.ttf",         "Tahoma" },
        { "times.ttf",          "Times New Roman" },
        { "verdana.ttf",        "Verdana" },
        { "georgia.ttf",        "Georgia" },
        /* macOS */
        { "PingFang.ttc",       "PingFang (苹方)" },
        { "STHeiti Medium.ttc", "Heiti SC" },
        { "Songti.ttc",         "Songti SC" },
        { "Kaiti.ttc",          "Kaiti SC" },
        { "Hiragino Sans GB.ttc","Hiragino Sans GB" },
        /* Linux (CJK) */
        { "ukai.ttc",           "AR PL UKai" },
        { "uming.ttc",          "AR PL UMing" },
        { "wqy-microhei.ttc",   "WenQuanYi Micro Hei" },
        { "wqy-zenhei.ttc",     "WenQuanYi Zen Hei" },
        { NULL, NULL }
    };
    for (int i = 0; map[i].file; ++i)
        if (jce_strcasecmp(fname, map[i].file) == 0) return map[i].pretty;
    return fname;
}

struct font_enum_ctx {
    JceFontEntry *out;
    int           count;
    int           max;
    const char   *root_dir;     /* directory currently being scanned */
};

static bool font_enum_cb(const char *fname, bool is_dir, void *userdata)
{
    (void)is_dir;
    struct font_enum_ctx *ctx = (struct font_enum_ctx *)userdata;
    if (!fname || !*fname) return true;
    if (ctx->count >= ctx->max) return false;

    /* Filter by extension. */
    const char *dot = strrchr(fname, '.');
    if (!dot) return true;
    if (jce_strcasecmp(dot, ".ttf") != 0 &&
        jce_strcasecmp(dot, ".otf") != 0 &&
        jce_strcasecmp(dot, ".ttc") != 0) return true;

    JceFontEntry *e = &ctx->out[ctx->count];
    snprintf(e->path, sizeof(e->path), "%s/%s", ctx->root_dir, fname);
    jce_strlcpy(e->display_name, prettify_font_name(fname), sizeof(e->display_name));
    ctx->count++;
    return true;
}

static void scan_dir(struct font_enum_ctx *ctx, const char *dir)
{
    if (!dir || !*dir) return;
    ctx->root_dir = dir;
    /* Returns false silently if dir doesn't exist — that's expected. */
    jce_fs_host_list_dir(dir, font_enum_cb, ctx);
}

static int font_entry_cmp(const void *a, const void *b)
{
    const JceFontEntry *ea = (const JceFontEntry *)a;
    const JceFontEntry *eb = (const JceFontEntry *)b;
    return jce_strcasecmp(ea->display_name, eb->display_name);
}

extern "C" int jce_editor_enumerate_fonts(JceFontEntry *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;
    struct font_enum_ctx ctx = { out, 0, max_entries, NULL };

    const char *plat = jce_platform_name();

    if (strcmp(plat, "Windows") == 0) {
        const char *windir = getenv("WINDIR");
        if (!windir || !*windir) windir = getenv("SystemRoot");
        if (windir && *windir) {
            char fontdir[1024];
            snprintf(fontdir, sizeof(fontdir), "%s/Fonts", windir);
            scan_dir(&ctx, fontdir);
        }
        const char *localapp = getenv("LOCALAPPDATA");
        if (localapp && *localapp) {
            char ud[1024];
            snprintf(ud, sizeof(ud),
                         "%s/Microsoft/Windows/Fonts", localapp);
            scan_dir(&ctx, ud);
        }
    } else if (strcmp(plat, "macOS") == 0) {
        scan_dir(&ctx, "/System/Library/Fonts");
        scan_dir(&ctx, "/System/Library/Fonts/Supplemental");
        scan_dir(&ctx, "/Library/Fonts");
        const char *home = getenv("HOME");
        if (home && *home) {
            char ud[1024];
            snprintf(ud, sizeof(ud), "%s/Library/Fonts", home);
            scan_dir(&ctx, ud);
        }
    } else {
        /* Linux / BSD / other unix-ish. */
        scan_dir(&ctx, "/usr/share/fonts");
        scan_dir(&ctx, "/usr/share/fonts/truetype");
        scan_dir(&ctx, "/usr/share/fonts/opentype");
        scan_dir(&ctx, "/usr/share/fonts/truetype/arphic");
        scan_dir(&ctx, "/usr/share/fonts/truetype/dejavu");
        scan_dir(&ctx, "/usr/share/fonts/truetype/noto");
        scan_dir(&ctx, "/usr/local/share/fonts");
        const char *home = getenv("HOME");
        if (home && *home) {
            char ud[1024];
            snprintf(ud, sizeof(ud), "%s/.fonts", home);
            scan_dir(&ctx, ud);
            snprintf(ud, sizeof(ud), "%s/.local/share/fonts", home);
            scan_dir(&ctx, ud);
        }
    }

    /* Deduplicate by absolute path (different scan dirs may overlap on
       Linux where /usr/share/fonts already contains truetype/ etc.). */
    if (ctx.count > 1) {
        for (int i = 0; i < ctx.count; ++i) {
            for (int j = i + 1; j < ctx.count; ) {
                if (jce_strcasecmp(out[i].path, out[j].path) == 0) {
                    out[j] = out[ctx.count - 1];
                    ctx.count--;
                } else {
                    ++j;
                }
            }
        }
    }

    if (ctx.count > 1)
        qsort(out, (size_t)ctx.count, sizeof(JceFontEntry), font_entry_cmp);

    return ctx.count;
}

/* ── Deferred font reload (called from Settings dialog while in a frame). */
namespace {
struct PendingReload {
    bool  pending = false;
    float size    = 14.0f;
    char  en[1024] = {0};
    char  zh[1024] = {0};
} g_pending;
} // namespace

extern "C" float jce_editor_get_baked_font_size(void)
{
    return s_baked_font_size;
}

extern "C" void jce_editor_request_font_reload(float size_pixels,
                                               const char *en_override,
                                               const char *zh_override)
{
    g_pending.pending = true;
    g_pending.size    = size_pixels;
    if (en_override) {
        strncpy(g_pending.en, en_override, sizeof(g_pending.en) - 1);
        g_pending.en[sizeof(g_pending.en) - 1] = '\0';
    } else {
        g_pending.en[0] = '\0';
    }
    if (zh_override) {
        strncpy(g_pending.zh, zh_override, sizeof(g_pending.zh) - 1);
        g_pending.zh[sizeof(g_pending.zh) - 1] = '\0';
    } else {
        g_pending.zh[0] = '\0';
    }
}

extern "C" void jce_editor_apply_pending_font_reload(void)
{
    if (!g_pending.pending) return;
    g_pending.pending = false;
    jce_editor_load_fonts(NULL, g_pending.size,
                          g_pending.en[0] ? g_pending.en : NULL,
                          g_pending.zh[0] ? g_pending.zh : NULL);
}
