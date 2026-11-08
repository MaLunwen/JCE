/*
 * jce_editor_style.cpp  Dark editor theme and font loading.
 *
 * Matches the reference Java editor's dark theme and style.
 */

#include "jce_editor_style.h"

#include "jce_editor_colors.h"
#include "jce_editor_font_data.h"
#include "core/jce_editor_alloc.h"
#include "core/jce_editor_config.h" /* boot-locale fallback (fonts build pre-i18n) */
#include "core/jce_editor_i18n.h"   /* active locale → bake only its glyphs */
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
#include <jce/os/core/jce_process.h>   /* fc-match (Linux fontconfig query) */
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/platform/jce_host_font.h>
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

int jce_editor_theme_from_string(const char *s)
{
    if (!s || !*s) return JCE_THEME_DARK;
    if (jce_strcasecmp(s, "Light") == 0) return JCE_THEME_LIGHT;
    if (jce_strcasecmp(s, "Blue")  == 0 ||
        jce_strcasecmp(s, "SSMS")  == 0) return JCE_THEME_SSMS;
    return JCE_THEME_DARK;
}

/* ── Font loading ──────────────────────────────────────────────────── */

/* ── Font resolution helpers ────────────────────────────────────────
 *
 * Editor fonts are resolved in this priority order:
 *   1. User override path (from JceEditorConfig.font_en_path / _zh_path)
 *   2. Shared host UI role (installed Ink Free first, then system Sans)
 *   3. Bundled Aileron Latin subset, then ImGui built-in default
 *
 * System font lookup is owned by the public OS/platform font resolver.
 * Loading the file off the
 * user's machine is legal because the user holds a license to use it
 * (e.g. their Windows install supplies Segoe UI); we never bundle these
 * files in the editor PAK.
 */

static bool file_exists_readable(const char *path)
{
    return jce_fs_host_exists_file(path);
}

/* Read a TTF/OTF source once using host IO's tracked default heap.
   Transfer its original buffer to ImGui; dynamic fonts retain it for the
   full atlas lifetime. Invalid sizes or consumer failure free it once. */
static void *read_font_file(const char *path, size_t *out_size)
{
    return ed_font_read_host(path, out_size);
}

/* Installed-font discovery lives in OS/platform, shared with Canvas. */
static bool find_system_font(const char *family, char *out, size_t out_size)
{
    if (family && !strcmp(family, "@ui-latin"))
        return jce_host_default_font(JCE_HOST_FONT_UI_LATIN, out, out_size);
    if (family && !strcmp(family, "@ui-cjk"))
        return jce_host_default_font(JCE_HOST_FONT_UI_CJK, out, out_size);
    return jce_host_find_font(family, out, out_size);
}

static bool linux_fc_match(const char *pattern, char *out, size_t out_size)
{
    return jce_host_find_font(pattern, out, out_size);
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
    if (override_path && *override_path) {
        if (file_exists_readable(override_path)) {
            jce_strlcpy(out_path, override_path, out_size);
            if (out_source) *out_source = "user override";
            return true;
        }
        LOG_WARN(LOG_TAG, "font phase=override_missing_or_nonfile path=%s fallback=%s",
                 override_path, system_family ? system_family : "stock/default");
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
    bool quiet_on_miss = false,
    const char *preferred_family = NULL)
{
    ImGuiIO &io = ImGui::GetIO();

    /* Try the explicit path once, followed by the optional local preference
     * and the shared role. Never reread a rejected resolved path. */
    const char *const families[] = { preferred_family, system_family };
    char rejected_paths[2][1024];
    unsigned rejected_count = 0;
    for (unsigned family = preferred_family ? 0u : 1u; family < 2u;) {
        char fs_path[1024];
        const char *source = NULL;
        const bool resolved = resolve_font_path(override_path, families[family],
                                                fs_path, sizeof(fs_path), &source);
        const bool is_override = resolved && strcmp(source, "user override") == 0;
        override_path = NULL;
        if (!is_override) ++family;
        if (!resolved) continue;
        bool already_rejected = false;
        for (unsigned i = 0; i < rejected_count; ++i)
            if (strcmp(rejected_paths[i], fs_path) == 0) already_rejected = true;
        if (already_rejected) continue;
        size_t n = 0;
        void *buf = read_font_file(fs_path, &n);
        if (buf) {
            ImFont *f = ed_font_transfer_recoverable(
                buf, (int)n, io.ConfigErrorRecoveryEnableAssert,
                [&](void *data, int bytes) {
                    return io.Fonts->AddFontFromMemoryTTF(
                        data, bytes, size_pixels, cfg, ranges);
                });
            if (f) {
                jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                         "font phase=atlas_admitted role=%s path=%s source=%s",
                         role, fs_path, source);
                return f;
            }
            LOG_WARN(LOG_TAG, "font phase=atlas_admission_rejected role=%s source=%s path=%s",
                     role, source, fs_path);
        } else {
            LOG_WARN(LOG_TAG, "font phase=source_read_or_size_rejected role=%s source=%s path=%s",
                     role, source, fs_path);
        }
        if (family < 2u) {
            jce_strlcpy(rejected_paths[rejected_count], fs_path,
                        sizeof(rejected_paths[rejected_count]));
            ++rejected_count;
        }
    }

    if (!quiet_on_miss) {
        LOG_WARN(LOG_TAG, "font phase=host_sources_exhausted role=%s fallback=stock/default", role);
    }
    return NULL;
}

static ImFont *load_editor_cjk_font(const char *locale, const char *override_path,
                                    float pixels, const ImFontConfig *cfg,
                                    const ImWchar *ranges)
{
    const char *preferred = NULL;
    if (locale && strcmp(locale, "zh_cn") == 0) {
        /* The existing host alias probes the known installed KaiTi file on
         * Windows and macOS. Linux's generic alias can return a substitute. */
        const char *platform = jce_platform_name();
        if (strcmp(platform, "Windows") == 0 || strcmp(platform, "macOS") == 0)
            preferred = "KaiTi";
    }
    return load_font_with_fallback("CJK", override_path, "@ui-cjk", pixels,
                                   cfg, ranges, false, preferred);
}

/* The stock source is read from the existing PAK closure and transferred
 * through the same atlas-owned buffer path as host fonts. */
static ImFont *load_stock_font(const JcePakArchive *pak, const char *path,
                              float pixels, const ImFontConfig *cfg,
                              const ImWchar *ranges)
{
    if (!pak) return NULL;
    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) return NULL;
    if (asset->original_size <= 100 || asset->original_size > INT_MAX) {
        LOG_WARN(LOG_TAG, "font phase=stock_source_size_rejected path=%s size=%llu reason=outside_imgui_admission_range",
                 path, (unsigned long long)asset->original_size);
        return NULL;
    }
    size_t size = (size_t)asset->original_size;
    void *data = ED_MALLOC(size);
    if (!data) return NULL;
    if (jce_pak_decompress_ex(pak, asset, data, size) != size ||
        !jce_pak_verify(asset, data, size)) {
        ED_FREE(data);
        LOG_WARN(LOG_TAG, "font phase=stock_decode_failed path=%s", path);
        return NULL;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImFont *font = ed_font_transfer_recoverable(
        data, (int)size, io.ConfigErrorRecoveryEnableAssert,
        [&](void *bytes, int count) {
            return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(bytes, count, pixels, cfg, ranges);
        });
    if (font) {
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                      "font phase=stock_fallback_ready owner=editor_style path=%s bytes=%zu",
                      path, size);
    } else {
        LOG_WARN(LOG_TAG, "font phase=stock_admission_failed path=%s", path);
    }
    return font;
}

/* Latched by jce_editor_style_ensure_locale_picker_glyphs(): lifts the
 * locale gates on the Korean + Cyrillic merge passes so the language
 * picker's native names ("한국어", "Русский", "Українська") render under
 * ANY locale.  Off by default — a session that never opens the language
 * settings keeps the memory-charter savings. */
static bool s_ext_locale_glyphs = false;

bool jce_editor_load_fonts(const JcePakArchive *pak, float size_pixels,
                           const char *en_override, const char *zh_override)
{
    /* Deferred font reloads pass no PAK. Keep the stock sources available. */
    static const JcePakArchive *s_pak_cached = NULL;
    if (pak) s_pak_cached = pak;
    const JcePakArchive *use_pak = pak ? pak : s_pak_cached;

    ImGuiIO &io = ImGui::GetIO();

    /* CRITICAL: clear the atlas before re-adding. Without this, repeat
       calls (e.g. from Project Settings 'Save') leave the previous Latin
       and CJK fonts in the atlas and append new ones, then re-point
       io.FontDefault. If the second pass fails to resolve CJK (e.g. user
       cleared the override and no CJK UI face is installed), the new default
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

    /* A user override wins; otherwise use the shared installed UI Latin policy. */
    ImFont *font = load_font_with_fallback(
        "Latin", en_override, "@ui-latin",
        size_pixels, &cfg, latin_ranges);

    /* Additional explicit Sans candidates retain compatibility with hosts
       lacking their usual UI face. */
    if (!font) {
        const char *latin_fallbacks[] = {
            "segoeui", "Arial", "Helvetica", "DejaVuSans", NULL };
        for (int i = 0; latin_fallbacks[i] && !font; i++) {
            font = load_font_with_fallback(
                "Latin", NULL, latin_fallbacks[i],
                size_pixels, &cfg, latin_ranges);
        }
    }

    if (!font)
        font = load_stock_font(use_pak, "fonts/Aileron-Regular.ttf",
                               size_pixels, &cfg, latin_ranges);
    if (!font) {
        /* No override, system face or stock. Add ImGui's built-in proggy font so
           the atlas isn't empty and we can still merge a CJK font onto it. */
        font = io.Fonts->AddFontDefault();
        LOG_INFO(LOG_TAG, "Latin font: ImGui built-in default (proggy)");
    }

    /* --- CJK font (merged into the Latin font) ----------------------- */
    ImFontConfig merge_cfg;
    merge_cfg.FontDataOwnedByAtlas = true;
    /* OversampleH/V=1 for the CJK/Korean merge passes: oversampling barely
     * helps dense CJK glyphs at 24px but quadruples their rasterization work
     * and atlas area (the dominant font-atlas-build cost).  Latin keeps 2x2
     * (cfg above).  PixelSnapH keeps glyph edges crisp.  Standard ImGui
     * guidance for CJK. */
    merge_cfg.OversampleH = 1;
    merge_cfg.OversampleV = 1;
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
    /* Bake only the ACTIVE locale's heavy glyph blocks (the small icon ranges
       below are always added).  Baking the full Korean (~11k) + Chinese-common
       (~1.5k) blocks unconditionally cost ~8-12MB of atlas on the 512MB
       baseline even for an English session — standard practice is to load the
       glyph set for the language actually in use.  This re-runs on locale
       change (the function is called again from Project Settings → Save), so
       switching to zh/ko/ja rebuilds the atlas with that language's glyphs. */
    const char *active_loc = jce_editor_i18n_locale_code(jce_editor_i18n_get_locale());
    /* Startup ordering: fonts build BEFORE jce_editor_i18n_init
     * (jce_editor.cpp — load_fonts at init, i18n right after), so at boot the
     * i18n locale still reads as the default ("en") regardless of the saved
     * language.  Fall back to the editor config's language field — the source
     * the i18n init itself will apply moments later — so the locale-gated
     * font passes (Korean file load below, Cyrillic merge) see the TRUE boot
     * locale.  static: the config struct is >10KB (recent-file arrays) and
     * active_loc points into it for the rest of this function; the font
     * builder is main-thread, non-reentrant. */
    static JceEditorConfig s_boot_cfg;
    if (!active_loc || strcmp(active_loc, "en") == 0) {
        if (jce_editor_config_load(&s_boot_cfg) && s_boot_cfg.language[0])
            active_loc = s_boot_cfg.language;
    }
    const bool loc_is_ko = (active_loc && strcmp(active_loc, "ko") == 0)
                        || s_ext_locale_glyphs;

    ImFontGlyphRangesBuilder cjk_builder;
    /* Chinese-common stays unconditional (covers user-typed Han in scene/asset
       names), and the i18n scan below adds every codepoint the UI actually uses
       — including each language's native name in the picker — so no language
       renders as '?'. */
    cjk_builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    /* The full Korean block (~11k Hangul) is the single biggest font cost and
       is only needed when the UI itself is Korean — gate it on the active
       locale.  Korean text still renders in other locales via the scan (which
       adds the exact Hangul used, e.g. the 한국어 picker label from ko.json). */
    if (loc_is_ko)
        cjk_builder.AddRanges(io.Fonts->GetGlyphRangesKorean());
    cjk_builder.AddRanges(cjk_extra_ranges);

    /* (Removed) The i18n PAK codepoint scan that pre-marked every glyph used
       by editor strings is obsolete under ImGui 1.92 dynamic fonts: the bgfx
       backend now sets ImGuiBackendFlags_RendererHasTextures
       (jce_imgui_renderer.cpp), under which pre-specified glyph ranges are
       IGNORED and glyphs are rasterized ON DEMAND the first time they are
       drawn — so every translatable string (and user-typed Han/Hangul/kana in
       scene/asset names, and the language picker's native names) renders
       without pre-baking, and no language can show as '?'.  Dropping the scan
       also reclaims its per-font-load decompress + UTF-8 walk of every
       i18n/*.json.  The static ranges built above/below remain only as
       harmless intent documentation and as a fallback for the legacy path if
       RendererHasTextures were ever disabled. */

    static ImVector<ImWchar> cjk_ranges_v;
    cjk_ranges_v.clear();
    cjk_builder.BuildRanges(&cjk_ranges_v);
    const ImWchar *cjk_ranges = cjk_ranges_v.Data;

    /* CJK fallback chain: try a list of commonly-installed CJK fonts so
       Chinese / Japanese text doesn't show as tofu when the preferred
       system UI Sans face is absent. */
    ImFont *cjk = load_editor_cjk_font(
        active_loc, zh_override, size_pixels, &merge_cfg, cjk_ranges);
    if (!cjk) {
        /* Linux: ask fontconfig for a CJK-covering system font (Noto CJK
           etc.).  Native, distro/location/filename independent — one font
           covers zh + ja + ko, and cjk_ranges already include Korean. */
        char fcpath[1024];
        const char *cjk_patterns[] = { ":lang=zh-cn", ":lang=ja", ":lang=zh", NULL };
        for (int i = 0; cjk_patterns[i] && !cjk; i++)
            if (linux_fc_match(cjk_patterns[i], fcpath, sizeof(fcpath)))
                cjk = load_font_with_fallback("CJK", fcpath, NULL,
                                              size_pixels, &merge_cfg, cjk_ranges);
    }
    if (!cjk) {
        const char *cjk_fallbacks[] = {
            "msyh",        /* Microsoft YaHei (Windows) */
            "simhei",      /* SimHei */
            "simsun",      /* SimSun */
            "msjh",        /* JhengHei */
            /* macOS — must be the EXACT system-font filename (probed as
             * <name>.ttc/.ttf).  Bare "STHeiti"/"Hiragino" never matched:
             * the real files are "STHeiti Medium.ttc" and the Japanese
             * "ヒラギノ…W*.ttc" (non-ASCII), and PingFang.ttc is absent on
             * some installs (e.g. Intel macOS) — which left zh/ja as tofu
             * while Korean (AppleSDGothicNeo.ttc) resolved fine.  These
             * names cover Simplified Chinese AND Japanese (Hiragino Sans GB
             * / Arial Unicode include kana). */
            "PingFang",          /* modern macOS (PingFang.ttc) */
            "Hiragino Sans GB",  /* macOS — comprehensive CJK (zh + ja kana/kanji) */
            "STHeiti Medium",    /* macOS — Chinese */
            "STHeiti Light",
            "Songti",            /* macOS — Chinese (Supplemental) */
            "Arial Unicode",     /* universal CJK (zh+ja+ko) — guaranteed render */
            "wqy-microhei",/* Linux */
            "wqy-zenhei",
            /* Linux Noto CJK ships as NotoSansCJK-Regular.ttc (one .ttc covers
             * zh+ja+ko) under /usr/share/fonts/opentype/noto; the bare
             * "NotoSansCJK" name never matched the -Regular suffix. */
            "NotoSansCJK-Regular", "NotoSerifCJK-Regular",
            "NotoSansCJK", "NotoSans",
            "DroidSansFallbackFull", "DroidSansFallback", /* fonts-droid-fallback */
            NULL };
        for (int i = 0; cjk_fallbacks[i] && !cjk; i++) {
            cjk = load_font_with_fallback(
                "CJK", NULL, cjk_fallbacks[i],
                size_pixels, &merge_cfg, cjk_ranges);
        }
    }

    /* --- Korean font (merged) --------------------------------------- */
    /* Some Chinese-first CJK fonts ship
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
        "NotoSansCJK-Regular",    /* Linux unified Noto CJK — includes Hangul */
        "NanumGothic",
        "NanumMyeongjo",
        NULL };
    /* Locale gate (512MB charter): the Korean font FILE is held resident by
     * the atlas whole (FontDataOwnedByAtlas — malgun.ttf is 13.5MB on this
     * machine), so only load it when the UI locale is actually Korean —
     * mirroring the Cyrillic pass below, which has always been locale-gated.
     * Switching the locale re-runs this function (Project Settings → Save),
     * so picking Korean loads the font then.  Hangul typed into scene/asset
     * names under a non-ko locale falls back like Cyrillic does. */
    bool ko_ok = !loc_is_ko;
    if (loc_is_ko) {   /* Linux: fontconfig Korean system font first. */
        char fcpath[1024];
        if (linux_fc_match(":lang=ko", fcpath, sizeof(fcpath))) {
            ImFont *kf = load_font_with_fallback(
                "Korean", fcpath, NULL,
                size_pixels, &merge_cfg, korean_ranges, /*quiet_on_miss=*/true);
            if (kf) ko_ok = true;
        }
    }
    for (int i = 0; !ko_ok && korean_fallbacks[i]; i++) {
        ImFont *kf = load_font_with_fallback(
            "Korean", NULL, korean_fallbacks[i],
            size_pixels, &merge_cfg, korean_ranges,
            /*quiet_on_miss=*/true);
        if (kf) break;
    }

    /* --- Cyrillic font (merged) ------------------------------------- */
    /* Ink Free (and several Latin defaults) ship NO Cyrillic glyphs, so
       ru/uk editor text renders as tofu once the locale switches. Mirror
       the Korean pass: force-merge a Cyrillic-capable system font, gated
       on a Cyrillic UI locale (Segoe UI / Arial / Tahoma on Windows;
       system sans on macOS; fontconfig :lang=ru on Linux). The merged CJK
       font frequently carries Cyrillic too, but make it explicit rather
       than rely on whichever CJK fallback won above. */
    const bool loc_is_cyrillic = (active_loc &&
        (strcmp(active_loc, "ru") == 0 || strcmp(active_loc, "uk") == 0))
        || s_ext_locale_glyphs;
    if (loc_is_cyrillic) {
        static const ImWchar cyrillic_ranges[] = {
            0x0400, 0x04FF,   /* Cyrillic */
            0x0500, 0x052F,   /* Cyrillic Supplement */
            0x2DE0, 0x2DFF,   /* Cyrillic Extended-A */
            0xA640, 0xA69F,   /* Cyrillic Extended-B */
            0,
        };
        const char *cyr_fallbacks[] = {
            "segoeui",            /* Segoe UI — Windows, full Cyrillic */
            "Arial", "tahoma", "Verdana",
            "Helvetica",          /* macOS */
            "DejaVuSans", "LiberationSans", "NotoSans",  /* Linux */
            "msyh", "NotoSansCJK-Regular",  /* CJK fonts also carry Cyrillic */
            NULL };
        bool cyr_ok = false;
        {   /* Linux: fontconfig Cyrillic system font first (native lookup). */
            char fcpath[1024];
            if (linux_fc_match(":lang=ru", fcpath, sizeof(fcpath))) {
                ImFont *cf = load_font_with_fallback(
                    "Cyrillic", fcpath, NULL,
                    size_pixels, &merge_cfg, cyrillic_ranges, /*quiet_on_miss=*/true);
                if (cf) cyr_ok = true;
            }
        }
        for (int i = 0; !cyr_ok && cyr_fallbacks[i]; i++) {
            ImFont *cf = load_font_with_fallback(
                "Cyrillic", NULL, cyr_fallbacks[i],
                size_pixels, &merge_cfg, cyrillic_ranges,
                /*quiet_on_miss=*/true);
            if (cf) break;
        }
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
    ImFont *icons = NULL;
    for (int i = 0; icon_fallbacks[i] && !icons; i++) {
        icons = load_font_with_fallback(
            "Icons", NULL, icon_fallbacks[i],
            size_pixels, &merge_cfg, icon_ranges,
            /*quiet_on_miss=*/true);
    }
    if (!cjk || !icons) {
        ImFont *stock = load_stock_font(use_pak, "fonts/JCE-CJK-Fallback.ttf",
                                      size_pixels, &merge_cfg, cjk_ranges);
        if (!cjk) cjk = stock;
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

    if (!cjk)
        LOG_WARN(LOG_TAG, "font phase=default_cjk_unavailable owner=editor_style fallback=latin_only");
    LOG_SUCCESS(LOG_TAG, "loaded fonts (%.0f px, cjk_available=%u)", size_pixels,
                 cjk ? 1u : 0u);
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
        { "segoepr.ttf",        "Segoe Print" },
        { "segoeprb.ttf",       "Segoe Print Bold" },
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

extern "C" void jce_editor_style_ensure_locale_picker_glyphs(void)
{
    if (s_ext_locale_glyphs)
        return;
    s_ext_locale_glyphs = true;
    /* One deferred rebuild with the gates lifted (same proven path as the
     * font-size slider).  Font overrides come from the config singleton so
     * the rebuild keeps the user's chosen font files. */
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    float sz = jce_editor_get_baked_font_size();
    if (sz <= 0.0f) sz = (float)cfg.font_size;
    jce_editor_request_font_reload(sz, cfg.font_en_path, cfg.font_zh_path);
    LOG_INFO(LOG_TAG, "locale-picker glyphs requested (Korean + Cyrillic "
                      "merge passes enabled for this session)");
}
