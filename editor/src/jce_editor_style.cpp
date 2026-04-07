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
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
}

/* ── Light theme ──────────────────────────────────────────────────── */

static void apply_light_theme(void)
{
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
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);
}

/* ── SSMS theme (SQL Server Management Studio style) ──────────────── */

static void apply_ssms_theme(void)
{
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
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
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

bool jce_editor_load_fonts(const PakArchive *pak, float size_pixels)
{
    /* --- Latin font (en.ttf) ---------------------------------------- */
    const PakAsset *en_asset = pak_find(pak, "fonts/en.ttf");
    if (!en_asset) {
        LOG_WARN(LOG_TAG, "fonts/en.ttf not found in PAK, using default font");
        return false;
    }

    void *en_buf = malloc((size_t)en_asset->original_size);
    if (!en_buf) return false;

    size_t en_n = pak_decompress(en_asset, en_buf, (size_t)en_asset->original_size);
    if (en_n == 0) {
        LOG_ERROR(LOG_TAG, "failed to decompress fonts/en.ttf");
        free(en_buf);
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;   /* ImGui will free the buffer. */
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;

    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin Supplement */
        0x2000, 0x206F,   /* General Punctuation */
        0,
    };

    ImFont *font = io.Fonts->AddFontFromMemoryTTF(
        en_buf, (int)en_n, size_pixels, &cfg, latin_ranges);

    if (!font) {
        LOG_WARN(LOG_TAG, "failed to add fonts/en.ttf, using default font");
        return false;
    }

    /* --- CJK font (zh-CN.ttf) — merged into the Latin font --------- */
    const PakAsset *zh_asset = pak_find(pak, "fonts/zh-CN.ttf");
    if (zh_asset) {
        void *zh_buf = malloc((size_t)zh_asset->original_size);
        if (zh_buf) {
            size_t zh_n = pak_decompress(zh_asset, zh_buf,
                                         (size_t)zh_asset->original_size);
            if (zh_n > 0) {
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

                if (io.Fonts->AddFontFromMemoryTTF(
                        zh_buf, (int)zh_n, size_pixels,
                        &merge_cfg, cjk_ranges)) {
                    LOG_INFO(LOG_TAG, "merged fonts/zh-CN.ttf for CJK glyphs");
                } else {
                    LOG_WARN(LOG_TAG, "failed to merge fonts/zh-CN.ttf");
                }
            } else {
                LOG_WARN(LOG_TAG, "failed to decompress fonts/zh-CN.ttf");
                free(zh_buf);
            }
        }
    } else {
        LOG_WARN(LOG_TAG, "fonts/zh-CN.ttf not found in PAK, CJK glyphs unavailable");
    }

    io.FontDefault = font;

    /* Rebuild font atlas on bgfx side. */
    jce_imgui_bgfx_rebuild_fonts();

    LOG_SUCCESS(LOG_TAG, "loaded fonts (%.0f px, Latin + CJK)", size_pixels);
    return true;
}
