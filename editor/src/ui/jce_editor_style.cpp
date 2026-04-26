/*
 * jce_editor_style.cpp  Dark editor theme and font loading.
 *
 * Matches the reference Java editor's dark theme and style.
 */

#include "jce_editor_style.h"

#include "jce_editor_colors.h"
#include "jce_imgui_bgfx.h"

#include <imgui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/pak_loader.h>
}

#include <SDL3/SDL.h>

#define LOG_TAG "editor_style"

static int s_current_theme = JCE_THEME_DARK;

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

    const char *names[] = { "Dark", "Light", "SSMS" };
    LOG_INFO(LOG_TAG, "%s theme applied", names[theme_idx]);
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
 * System font lookup uses runtime platform detection via SDL_GetPlatform()
 * — no compile-time #ifdef branches. We probe a fixed list of well-known
 * filesystem locations per platform string. Loading the file off the
 * user's machine is legal because the user holds a license to use it
 * (e.g. their Windows install ships Ink Free); we never bundle these
 * files in the editor PAK.
 */

static bool file_exists_readable(const char *path)
{
    if (!path || !*path) return false;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return false;
    SDL_CloseIO(io);
    return true;
}

/* Read a TTF/OTF font file fully into a heap buffer.
   On success returns a malloc'd buffer (caller transfers ownership to
   ImGui via FontDataOwnedByAtlas) and writes the size to *out_size. */
static void *read_font_file(const char *path, size_t *out_size)
{
    if (!path || !*path) return NULL;
    size_t n = 0;
    void *raw = SDL_LoadFile(path, &n);
    if (!raw || n == 0) {
        if (raw) SDL_free(raw);
        return NULL;
    }
    /* SDL_LoadFile returns SDL-allocated memory; ImGui will call free()
       on FontDataOwnedByAtlas, so copy into a malloc buffer. */
    void *buf = malloc(n);
    if (!buf) { SDL_free(raw); return NULL; }
    memcpy(buf, raw, n);
    SDL_free(raw);
    if (out_size) *out_size = n;
    return buf;
}

/* Build a candidate path for a system font by joining a directory with
   a filename. Empty dir or empty file -> empty result. */
static void join_path(char *out, size_t out_size,
                      const char *dir, const char *file)
{
    if (!dir || !*dir || !file || !*file) { out[0] = '\0'; return; }
    SDL_snprintf(out, out_size, "%s/%s", dir, file);
}

/* Look up a system-installed font by family. Tries platform-specific
   well-known locations and returns the first one that exists.
   `family` is one of: "InkFree", "KaiTi". Returns true if found,
   writing the absolute path to `out`. */
static bool find_system_font(const char *family, char *out, size_t out_size)
{
    out[0] = '\0';
    const char *plat = SDL_GetPlatform();   /* "Windows" / "macOS" / "Linux" / ... */

    /* Map family name to per-platform candidate filenames.
       Filenames are documented OS install names; if the user has the
       font installed it lives at one of these paths. */
    const char *win_file = NULL;
    const char *mac_file = NULL;
    const char *linux_file = NULL;
    if (SDL_strcasecmp(family, "InkFree") == 0) {
        win_file   = "inkfree.ttf";
        mac_file   = "Ink Free.ttf";   /* uncommon on macOS, harmless probe */
        linux_file = "InkFree.ttf";    /* uncommon on Linux, harmless probe */
    } else if (SDL_strcasecmp(family, "KaiTi") == 0) {
        win_file   = "simkai.ttf";
        mac_file   = "Kaiti.ttc";
        linux_file = "ukai.ttc";       /* AR PL UKai (common Linux Kaiti) */
    } else {
        return false;
    }

    char candidate[1024];

    if (SDL_strcmp(plat, "Windows") == 0) {
        const char *windir = SDL_getenv("WINDIR");
        if (!windir || !*windir) windir = SDL_getenv("SystemRoot");
        if (windir && *windir) {
            join_path(candidate, sizeof(candidate),
                      windir, "Fonts");
            char fontdir[1024];
            SDL_snprintf(fontdir, sizeof(fontdir), "%s", candidate);
            join_path(candidate, sizeof(candidate), fontdir, win_file);
            if (file_exists_readable(candidate)) {
                SDL_strlcpy(out, candidate, out_size);
                return true;
            }
        }
    } else if (SDL_strcmp(plat, "macOS") == 0) {
        const char *dirs[] = {
            "/System/Library/Fonts/Supplemental",
            "/System/Library/Fonts",
            "/Library/Fonts",
            NULL
        };
        for (int i = 0; dirs[i]; ++i) {
            join_path(candidate, sizeof(candidate), dirs[i], mac_file);
            if (file_exists_readable(candidate)) {
                SDL_strlcpy(out, candidate, out_size);
                return true;
            }
        }
    } else if (SDL_strcmp(plat, "Linux") == 0) {
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
                SDL_strlcpy(out, candidate, out_size);
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
        SDL_strlcpy(out_path, override_path, out_size);
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
   ImFont* (or NULL if neither path resolves) and logs the resolution. */
static ImFont *load_font_with_fallback(
    const char *role,                  /* "Latin" / "CJK" */
    const char *override_path,
    const char *system_family,
    float size_pixels,
    const ImFontConfig *cfg,
    const ImWchar *ranges)
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
            free(buf);
        } else {
            LOG_WARN(LOG_TAG, "%s font: cannot read %s", role, fs_path);
        }
    }

    LOG_WARN(LOG_TAG, "%s font unavailable (no override, no system match); "
                      "ImGui default will be used", role);
    return NULL;
}

bool jce_editor_load_fonts(const JcePakArchive *pak, float size_pixels,
                           const char *en_override, const char *zh_override)
{
    (void)pak; /* no longer needed; kept for API stability */
    ImGuiIO &io = ImGui::GetIO();

    /* --- Latin font -------------------------------------------------- */
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;

    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin Supplement */
        0x2000, 0x206F,   /* General Punctuation */
        0,
    };

    ImFont *font = load_font_with_fallback(
        "Latin", en_override, "InkFree",
        size_pixels, &cfg, latin_ranges);

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

    static const ImWchar cjk_ranges[] = {
        0x3000, 0x30FF,   /* CJK Symbols + Katakana */
        0x31F0, 0x31FF,   /* Katakana Phonetic Extensions */
        0xFF00, 0xFFEF,   /* Halfwidth & Fullwidth Forms */
        0x4E00, 0x9FFF,   /* CJK Unified Ideographs */
        0,
    };

    load_font_with_fallback(
        "CJK", zh_override, "KaiTi",
        size_pixels, &merge_cfg, cjk_ranges);

    io.FontDefault = font;

    /* Rebuild font atlas on bgfx side. */
    jce_imgui_bgfx_rebuild_fonts();

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
        if (SDL_strcasecmp(fname, map[i].file) == 0) return map[i].pretty;
    return fname;
}

struct font_enum_ctx {
    JceFontEntry *out;
    int           count;
    int           max;
    const char   *root_dir;     /* directory currently being scanned */
};

static SDL_EnumerationResult font_enum_cb(void *userdata,
                                          const char *dirname,
                                          const char *fname)
{
    (void)dirname;
    struct font_enum_ctx *ctx = (struct font_enum_ctx *)userdata;
    if (!fname || !*fname) return SDL_ENUM_CONTINUE;
    if (ctx->count >= ctx->max) return SDL_ENUM_SUCCESS;

    /* Filter by extension. */
    const char *dot = SDL_strrchr(fname, '.');
    if (!dot) return SDL_ENUM_CONTINUE;
    if (SDL_strcasecmp(dot, ".ttf") != 0 &&
        SDL_strcasecmp(dot, ".otf") != 0 &&
        SDL_strcasecmp(dot, ".ttc") != 0) return SDL_ENUM_CONTINUE;

    JceFontEntry *e = &ctx->out[ctx->count];
    SDL_snprintf(e->path, sizeof(e->path), "%s/%s", ctx->root_dir, fname);
    SDL_strlcpy(e->display_name, prettify_font_name(fname), sizeof(e->display_name));
    ctx->count++;
    return SDL_ENUM_CONTINUE;
}

static void scan_dir(struct font_enum_ctx *ctx, const char *dir)
{
    if (!dir || !*dir) return;
    ctx->root_dir = dir;
    /* SDL_EnumerateDirectory returns false if dir doesn't exist — fine. */
    SDL_EnumerateDirectory(dir, font_enum_cb, ctx);
}

static int font_entry_cmp(const void *a, const void *b)
{
    const JceFontEntry *ea = (const JceFontEntry *)a;
    const JceFontEntry *eb = (const JceFontEntry *)b;
    return SDL_strcasecmp(ea->display_name, eb->display_name);
}

extern "C" int jce_editor_enumerate_fonts(JceFontEntry *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;
    struct font_enum_ctx ctx = { out, 0, max_entries, NULL };

    const char *plat = SDL_GetPlatform();

    if (SDL_strcmp(plat, "Windows") == 0) {
        const char *windir = SDL_getenv("WINDIR");
        if (!windir || !*windir) windir = SDL_getenv("SystemRoot");
        if (windir && *windir) {
            char fontdir[1024];
            SDL_snprintf(fontdir, sizeof(fontdir), "%s/Fonts", windir);
            scan_dir(&ctx, fontdir);
        }
        const char *localapp = SDL_getenv("LOCALAPPDATA");
        if (localapp && *localapp) {
            char ud[1024];
            SDL_snprintf(ud, sizeof(ud),
                         "%s/Microsoft/Windows/Fonts", localapp);
            scan_dir(&ctx, ud);
        }
    } else if (SDL_strcmp(plat, "macOS") == 0) {
        scan_dir(&ctx, "/System/Library/Fonts");
        scan_dir(&ctx, "/System/Library/Fonts/Supplemental");
        scan_dir(&ctx, "/Library/Fonts");
        const char *home = SDL_getenv("HOME");
        if (home && *home) {
            char ud[1024];
            SDL_snprintf(ud, sizeof(ud), "%s/Library/Fonts", home);
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
        const char *home = SDL_getenv("HOME");
        if (home && *home) {
            char ud[1024];
            SDL_snprintf(ud, sizeof(ud), "%s/.fonts", home);
            scan_dir(&ctx, ud);
            SDL_snprintf(ud, sizeof(ud), "%s/.local/share/fonts", home);
            scan_dir(&ctx, ud);
        }
    }

    /* Deduplicate by absolute path (different scan dirs may overlap on
       Linux where /usr/share/fonts already contains truetype/ etc.). */
    if (ctx.count > 1) {
        for (int i = 0; i < ctx.count; ++i) {
            for (int j = i + 1; j < ctx.count; ) {
                if (SDL_strcasecmp(out[i].path, out[j].path) == 0) {
                    out[j] = out[ctx.count - 1];
                    ctx.count--;
                } else {
                    ++j;
                }
            }
        }
    }

    if (ctx.count > 1)
        SDL_qsort(out, (size_t)ctx.count, sizeof(JceFontEntry), font_entry_cmp);

    return ctx.count;
}
