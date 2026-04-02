/*
 * jce_editor_style.cpp  Dark editor theme and font loading.
 *
 * Matches the reference Java editor's dark theme and style.
 */

#include "jce_editor_style.h"
#include "jce_editor_colors.h"
#include "jce_imgui_bgfx.h"

#include <imgui.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_log.h>
}

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
    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.67f, 0.67f, 0.67f, 0.39f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
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
    c[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.35f, 0.38f, 0.42f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);

}

/* ── Light theme ──────────────────────────────────────────────────── */

static void apply_light_theme(void)
{
    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.70f, 0.70f, 0.70f, 0.65f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.78f, 0.78f, 0.78f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.70f, 0.70f, 0.70f, 0.67f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.90f, 0.90f, 0.90f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.16f, 0.47f, 0.87f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.20f, 0.50f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.16f, 0.47f, 0.87f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.72f, 0.72f, 0.72f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.78f, 0.78f, 0.78f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.72f, 0.72f, 0.72f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.70f, 0.70f, 0.70f, 0.50f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.16f, 0.47f, 0.87f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.16f, 0.47f, 0.87f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.16f, 0.47f, 0.87f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.16f, 0.47f, 0.87f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.16f, 0.47f, 0.87f, 0.95f);
    c[ImGuiCol_Tab]                   = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.72f, 0.72f, 0.72f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.88f, 0.88f, 0.88f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.65f, 0.65f, 0.70f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.75f, 0.75f, 0.78f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.16f, 0.47f, 0.87f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.16f, 0.47f, 0.87f, 1.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.16f, 0.47f, 0.87f, 1.00f);
}

/* ── Blue theme ───────────────────────────────────────────────────── */

static void apply_blue_theme(void)
{
    ImVec4 *c = ImGui::GetStyle().Colors;

    c[ImGuiCol_Text]                  = ImVec4(0.90f, 0.93f, 1.00f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.45f, 0.50f, 0.58f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.11f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.25f, 0.30f, 0.42f, 0.50f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.19f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.30f, 0.35f, 0.48f, 0.67f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.12f, 0.17f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.06f, 0.09f, 0.53f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.30f, 0.35f, 0.48f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.44f, 0.58f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.28f, 0.50f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.18f, 0.22f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.25f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.30f, 0.38f, 0.55f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.16f, 0.19f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.22f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.30f, 0.35f, 0.48f, 0.67f);
    c[ImGuiCol_Separator]             = ImVec4(0.25f, 0.30f, 0.42f, 0.50f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.20f, 0.40f, 0.80f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.20f, 0.40f, 0.80f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.30f, 0.55f, 1.00f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.30f, 0.55f, 1.00f, 0.67f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.30f, 0.55f, 1.00f, 0.95f);
    c[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.12f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.22f, 0.28f, 0.42f, 1.00f);
    c[ImGuiCol_TabSelected]           = ImVec4(0.16f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_TabDimmed]             = ImVec4(0.10f, 0.12f, 0.17f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.12f, 0.15f, 0.22f, 1.00f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.14f, 0.17f, 0.24f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.22f, 0.26f, 0.36f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.90f, 0.93f, 1.00f, 0.03f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.30f, 0.55f, 1.00f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);
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
    case JCE_THEME_BLUE:  apply_blue_theme();  break;
    default:              apply_dark_theme();   theme_idx = JCE_THEME_DARK; break;
    }
    s_current_theme = theme_idx;

    const char *names[] = { "Dark", "Light", "Blue" };
    LOG_INFO(LOG_TAG, "%s theme applied", names[theme_idx]);
}

int jce_editor_get_theme(void)
{
    return s_current_theme;
}

/* ── Font loading ──────────────────────────────────────────────────── */

bool jce_editor_load_fonts(const PakArchive *pak, float size_pixels)
{
    const PakAsset *asset = pak_find(pak, "fonts/JCE.ttf");
    if (!asset) {
        LOG_WARN(LOG_TAG, "JCE.ttf not found in PAK, using default font");
        return false;
    }

    void *buf = malloc((size_t)asset->original_size);
    if (!buf) return false;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "failed to decompress JCE.ttf");
        free(buf);
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;   /* ImGui will free the buffer. */
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;

    /* Full glyph ranges for CJK + Latin (matches reference). */
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin Supplement */
        0x2000, 0x206F,   /* General Punctuation */
        0x3000, 0x30FF,   /* CJK Symbols + Katakana */
        0x31F0, 0x31FF,   /* Katakana Phonetic Extensions */
        0xFF00, 0xFFEF,   /* Halfwidth & Fullwidth Forms */
        0x4E00, 0x9FFF,   /* CJK Unified Ideographs */
        0,
    };

    ImFont *font = io.Fonts->AddFontFromMemoryTTF(
        buf, (int)n, size_pixels, &cfg, ranges);

    if (!font) {
        LOG_WARN(LOG_TAG, "failed to add JCE.ttf, using default font");
        return false;
    }

    io.FontDefault = font;

    /* Rebuild font atlas on bgfx side. */
    jce_imgui_bgfx_rebuild_fonts();

    LOG_SUCCESS(LOG_TAG, "loaded JCE.ttf (%.0f px, CJK+Latin)", size_pixels);
    return true;
}
