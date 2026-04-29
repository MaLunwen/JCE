/*
 * jce_editor_colors.h  Centralized color constants for JCE Editor UI.
 *
 * Ported from EditorColors.java — all UI colors defined here.
 * Usage:  ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_PRIMARY);
 */

#ifndef JCE_EDITOR_COLORS_H
#define JCE_EDITOR_COLORS_H

#include <jce/tools/jce_imgui.hpp>
/* Helper: build ImVec4 from 0-255 RGBA */
#define JCE_RGBA(r, g, b, a) ImVec4((r)/255.0f, (g)/255.0f, (b)/255.0f, (a)/255.0f)

/* ── Background Colors ─────────────────────────────────────────────── */

#define JCE_COLOR_BG_MAIN          JCE_RGBA( 30,  30,  40, 255)
#define JCE_COLOR_BG_PANEL         JCE_RGBA( 35,  35,  45, 255)
#define JCE_COLOR_BG_POPUP         JCE_RGBA( 40,  40,  50, 250)
#define JCE_COLOR_BG_HEADER        JCE_RGBA( 45,  45,  55, 255)
#define JCE_COLOR_BG_SELECTED      JCE_RGBA( 60,  80, 120, 255)
#define JCE_COLOR_BG_HOVERED       JCE_RGBA( 50,  60,  80, 255)

/* ── Text Colors ───────────────────────────────────────────────────── */

#define JCE_COLOR_TEXT_PRIMARY      JCE_RGBA(255, 255, 255, 255)
#define JCE_COLOR_TEXT_SECONDARY    JCE_RGBA(160, 160, 170, 255)
#define JCE_COLOR_TEXT_DISABLED     JCE_RGBA(100, 100, 110, 255)
#define JCE_COLOR_TEXT_ERROR        JCE_RGBA(255, 100, 100, 255)
#define JCE_COLOR_TEXT_WARNING      JCE_RGBA(255, 200, 100, 255)
#define JCE_COLOR_TEXT_SUCCESS      JCE_RGBA(100, 255, 150, 255)
#define JCE_COLOR_TEXT_LINK         JCE_RGBA(100, 180, 255, 255)

/* ── Accent Colors ─────────────────────────────────────────────────── */

#define JCE_COLOR_ACCENT           JCE_RGBA( 66, 150, 250, 255)
#define JCE_COLOR_ACCENT_HOVER     JCE_RGBA( 86, 170, 255, 255)
#define JCE_COLOR_ACCENT_ACTIVE    JCE_RGBA( 46, 130, 230, 255)
#define JCE_COLOR_ACCENT_SECONDARY JCE_RGBA(150, 100, 200, 255)

/* ── Scene View Colors ─────────────────────────────────────────────── */

#define JCE_COLOR_GRID_LINE        JCE_RGBA( 80,  80,  90, 255)
#define JCE_COLOR_GRID_MAJOR       JCE_RGBA(100, 100, 110, 255)
#define JCE_COLOR_AXIS_X           JCE_RGBA(255,  80,  80, 255)
#define JCE_COLOR_AXIS_Y           JCE_RGBA( 80, 255,  80, 255)
#define JCE_COLOR_AXIS_Z           JCE_RGBA( 80,  80, 255, 255)
#define JCE_COLOR_SELECTION_OUTLINE JCE_RGBA(255, 120,   0, 255)
#define JCE_COLOR_SELECTION_FILL   JCE_RGBA(255, 120,   0,  40)
#define JCE_COLOR_SELECTION_BOX    JCE_RGBA(100, 150, 255, 100)

/* Asset browser item selection (blue, matching Java reference) */
#define JCE_COLOR_ASSET_SELECTED   ImVec4(0.2f, 0.6f, 1.0f, 1.0f)
#define JCE_COLOR_ASSET_SEL_TEXT   ImVec4(0.4f, 0.8f, 1.0f, 1.0f)

/* ── Gizmo Colors ──────────────────────────────────────────────────── */

#define JCE_COLOR_GIZMO_X          JCE_RGBA(255,  50,  50, 255)
#define JCE_COLOR_GIZMO_Y          JCE_RGBA( 50, 255,  50, 255)
#define JCE_COLOR_GIZMO_Z          JCE_RGBA( 50,  50, 255, 255)
#define JCE_COLOR_GIZMO_HOVERED    JCE_RGBA(255, 255, 100, 255)
#define JCE_COLOR_GIZMO_ACTIVE     JCE_RGBA(255, 255, 255, 255)

/* ── Collider Visualization ────────────────────────────────────────── */

#define JCE_COLOR_COLLIDER_BOX     JCE_RGBA(100, 200, 255, 255)
#define JCE_COLOR_COLLIDER_SPHERE  JCE_RGBA(100, 255, 200, 255)
#define JCE_COLOR_COLLIDER_CAPSULE JCE_RGBA(200, 100, 255, 255)
#define JCE_COLOR_COLLIDER_MESH    JCE_RGBA(255, 200, 100, 255)
#define JCE_COLOR_COLLIDER_TRIGGER JCE_RGBA(100, 255, 100,  80)

/* ── Hierarchy Window ──────────────────────────────────────────────── */

#define JCE_COLOR_HIER_PARENT      JCE_RGBA(200, 200, 210, 255)
#define JCE_COLOR_HIER_LEAF        JCE_RGBA(180, 180, 190, 255)
#define JCE_COLOR_HIER_PREFAB      JCE_RGBA(100, 180, 255, 255)
#define JCE_COLOR_HIER_DISABLED    JCE_RGBA(120, 120, 130, 255)
#define JCE_COLOR_HIER_DROP_TARGET JCE_RGBA(100, 200, 100, 100)

/* ── Console Window ────────────────────────────────────────────────── */

#define JCE_COLOR_CONSOLE_INFO     JCE_RGBA(200, 200, 210, 255)
#define JCE_COLOR_CONSOLE_DEBUG    JCE_RGBA(150, 150, 160, 255)
#define JCE_COLOR_CONSOLE_WARN     JCE_RGBA(255, 200, 100, 255)
#define JCE_COLOR_CONSOLE_ERROR    JCE_RGBA(255, 100, 100, 255)

/* ── Asset Browser (matches Java reference renderFileItem colors) ──── */

#define JCE_COLOR_ASSET_FOLDER     ImVec4(0.95f, 0.75f, 0.20f, 1.0f)  /* gold   */
#define JCE_COLOR_ASSET_CODE       ImVec4(0.90f, 0.40f, 0.20f, 1.0f)  /* orange */
#define JCE_COLOR_ASSET_SCENE      ImVec4(0.60f, 0.20f, 0.80f, 1.0f)  /* purple */
#define JCE_COLOR_ASSET_DATA       ImVec4(0.20f, 0.60f, 0.20f, 1.0f)  /* green  */
#define JCE_COLOR_ASSET_IMAGE      ImVec4(0.20f, 0.50f, 0.90f, 1.0f)  /* blue   */
#define JCE_COLOR_ASSET_AUDIO      ImVec4(0.85f, 0.30f, 0.65f, 1.0f)  /* magenta */
#define JCE_COLOR_ASSET_VIDEO      ImVec4(0.70f, 0.15f, 0.30f, 1.0f)  /* crimson */
#define JCE_COLOR_ASSET_MODEL      ImVec4(0.20f, 0.65f, 0.60f, 1.0f)  /* teal   */
#define JCE_COLOR_ASSET_SHADER     ImVec4(0.80f, 0.70f, 0.20f, 1.0f)  /* mustard */
#define JCE_COLOR_ASSET_MATERIAL   ImVec4(0.40f, 0.70f, 0.25f, 1.0f)  /* olive  */
#define JCE_COLOR_ASSET_FONT       ImVec4(0.55f, 0.50f, 0.80f, 1.0f)  /* indigo */
#define JCE_COLOR_ASSET_ARCHIVE    ImVec4(0.55f, 0.40f, 0.20f, 1.0f)  /* brown  */
#define JCE_COLOR_ASSET_DOC        ImVec4(0.40f, 0.65f, 0.85f, 1.0f)  /* steel  */
#define JCE_COLOR_ASSET_ANIM       ImVec4(0.85f, 0.50f, 0.20f, 1.0f)  /* rust   */
#define JCE_COLOR_ASSET_PREFAB     ImVec4(0.20f, 0.70f, 0.50f, 1.0f)  /* jade   */
#define JCE_COLOR_ASSET_DEFAULT    ImVec4(0.55f, 0.55f, 0.55f, 1.0f)  /* gray   */

/* ── Inspector ─────────────────────────────────────────────────────── */

#define JCE_COLOR_INSP_HEADER      JCE_RGBA( 50,  55,  65, 255)
#define JCE_COLOR_INSP_LABEL       JCE_RGBA(180, 180, 190, 255)
#define JCE_COLOR_INSP_VEC_X       JCE_RGBA(200,  80,  80, 255)
#define JCE_COLOR_INSP_VEC_Y       JCE_RGBA( 80, 200,  80, 255)
#define JCE_COLOR_INSP_VEC_Z       JCE_RGBA( 80,  80, 200, 255)
#define JCE_COLOR_INSP_VEC_W       JCE_RGBA(200, 200,  80, 255)

/* ── Timeline ──────────────────────────────────────────────────────── */

#define JCE_COLOR_TL_KEYFRAME      JCE_RGBA(255, 200,  50, 255)
#define JCE_COLOR_TL_KEYFRAME_SEL  JCE_RGBA(255, 255, 100, 255)
#define JCE_COLOR_TL_PLAYHEAD      JCE_RGBA(255, 100, 100, 255)
#define JCE_COLOR_TL_TRACK_EVEN    JCE_RGBA( 40,  40,  50, 255)
#define JCE_COLOR_TL_TRACK_ODD     JCE_RGBA( 45,  45,  55, 255)

/* ── Tag Colors (macOS Finder style) ───────────────────────────────── */

#define JCE_TAG_COUNT  8
#define JCE_COLOR_TAG_NONE         JCE_RGBA(  0,   0,   0,   0)
#define JCE_COLOR_TAG_RED          JCE_RGBA(255,  59,  48, 255)
#define JCE_COLOR_TAG_ORANGE       JCE_RGBA(255, 149,   0, 255)
#define JCE_COLOR_TAG_YELLOW       JCE_RGBA(255, 204,   0, 255)
#define JCE_COLOR_TAG_GREEN        JCE_RGBA( 52, 199,  89, 255)
#define JCE_COLOR_TAG_BLUE         JCE_RGBA(  0, 122, 255, 255)
#define JCE_COLOR_TAG_PURPLE       JCE_RGBA(175,  82, 222, 255)
#define JCE_COLOR_TAG_GRAY         JCE_RGBA(142, 142, 147, 255)

/* ── ImU32 Helpers (for ImDrawList) ────────────────────────────────── */

#define JCE_COLOR32(r, g, b, a) IM_COL32((r), (g), (b), (a))

#define JCE_COL32_AXIS_X      JCE_COLOR32(255,  80,  80, 255)
#define JCE_COL32_AXIS_Y      JCE_COLOR32( 80, 255,  80, 255)
#define JCE_COL32_AXIS_Z      JCE_COLOR32( 80,  80, 255, 255)
#define JCE_COL32_GRID_LINE   JCE_COLOR32( 80,  80,  90, 255)
#define JCE_COL32_GRID_MAJOR  JCE_COLOR32(100, 100, 110, 255)
#define JCE_COL32_SELECTION   JCE_COLOR32(255, 180,  50, 255)

#endif /* JCE_EDITOR_COLORS_H */
