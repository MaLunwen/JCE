/*
 * jce_theme_palette.h  Theme-aware drawing palette helpers.
 *
 * Several timeline-style panels (animation editor, particle editor,
 * sequencer, animator state machine, material graph, navmesh, curve
 * editor, profiler) draw onto raw ImDrawList canvases and historically
 * used hardcoded dark RGB values. That made them unreadable under the
 * Light / SSMS themes.
 *
 * These helpers return ImU32 / ImVec4 values that pick a sensible value
 * from the current ImGui style (which is updated by jce_editor_apply_theme
 * on theme switch). Use them instead of IM_COL32(...) literals for
 * canvas backgrounds, alternating track stripes, grid lines, separators
 * and label text so the panels look correct under every theme.
 */

#ifndef JCE_THEME_PALETTE_H
#define JCE_THEME_PALETTE_H

#include <jce/tools/jce_imgui.h>

#include "jce_editor_style.h" /* JCE_THEME_DARK / LIGHT / SSMS */

#ifdef __cplusplus
extern "C++" {

namespace jce_theme {

/* Whether the current theme is a "light" theme (i.e. dark text on light
   background). Decided by sampling ImGuiCol_WindowBg luminance. */
inline bool is_light()
{
    const ImVec4 &bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    float lum = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
    return lum > 0.5f;
}

inline ImU32 col_from(ImGuiCol slot, float alpha_mul = 1.0f)
{
    ImVec4 c = ImGui::GetStyle().Colors[slot];
    c.w *= alpha_mul;
    return ImGui::ColorConvertFloat4ToU32(c);
}

inline ImU32 mix(ImU32 a, ImU32 b, float t)
{
    ImVec4 fa = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 fb = ImGui::ColorConvertU32ToFloat4(b);
    ImVec4 r((fa.x * (1 - t) + fb.x * t),
             (fa.y * (1 - t) + fb.y * t),
             (fa.z * (1 - t) + fb.z * t),
             (fa.w * (1 - t) + fb.w * t));
    return ImGui::ColorConvertFloat4ToU32(r);
}

/* Canvas background: use FrameBg so it contrasts a bit with WindowBg. */
inline ImU32 canvas_bg()      { return col_from(ImGuiCol_FrameBg); }
inline ImU32 canvas_bg_alt()  { return col_from(ImGuiCol_TableRowBgAlt); }

/* Alternating track row stripes — slight tint over canvas_bg. */
inline ImU32 track_even()
{
    ImU32 base = canvas_bg();
    ImU32 mod  = is_light() ? IM_COL32(0, 0, 0, 16)
                            : IM_COL32(255, 255, 255, 8);
    return mix(base, mod, 0.5f);
}
inline ImU32 track_odd()
{
    ImU32 base = canvas_bg();
    ImU32 mod  = is_light() ? IM_COL32(255, 255, 255, 24)
                            : IM_COL32(0, 0, 0, 16);
    return mix(base, mod, 0.5f);
}

/* Header / ruler strip. */
inline ImU32 header_bg() { return col_from(ImGuiCol_TabActive); }

/* Grid lines on the canvas. */
inline ImU32 grid_minor() { return col_from(ImGuiCol_Border, 0.45f); }
inline ImU32 grid_major() { return col_from(ImGuiCol_Border, 0.85f); }
inline ImU32 separator()  { return col_from(ImGuiCol_Separator); }

/* Text on the canvas. */
inline ImU32 text_primary()   { return col_from(ImGuiCol_Text); }
inline ImU32 text_secondary() { return col_from(ImGuiCol_TextDisabled); }

/* Selection / playhead — keep saturated, but adapt alpha for light theme. */
inline ImU32 playhead()
{
    return is_light() ? IM_COL32(220, 40, 40, 230)
                      : IM_COL32(255, 100, 100, 220);
}
inline ImU32 selection_outline()
{
    return is_light() ? IM_COL32(255, 140, 0, 240)
                      : IM_COL32(255, 220, 80, 255);
}

/* Outline pen for keyframe diamonds / nodes. */
inline ImU32 node_outline()
{
    return is_light() ? IM_COL32(50, 50, 60, 220)
                      : IM_COL32(0, 0, 0, 220);
}

} /* namespace jce_theme */

} /* extern "C++" */
#endif

#endif /* JCE_THEME_PALETTE_H */
