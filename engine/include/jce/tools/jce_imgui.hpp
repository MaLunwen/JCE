/*
 * jce/tools/jce_imgui.h  Tools-layer Dear ImGui access.
 *
 * Single canonical entry point for ImGui in JCE editor / debug-overlay
 * code. Direct  #include <imgui.h>  is forbidden in editor/ — always
 * route through this header.
 *
 * Why a wrapper:
 *   * Centralises ImGui pre-defines (cglm interop, custom allocator,
 *     UTF-8 string helpers) so they stay consistent across TUs.
 *   * Lets us swap or vendor-uplift Dear ImGui without touching 30+
 *     editor files.
 *   * Provides a future hook for pluggable backends (bgfx today, others
 *     tomorrow) and JCE-flavoured helpers (theme, asset thumbnails).
 *
 * Usage:
 *   #include <jce/tools/jce_imgui.h>
 *   ImGui::Begin("My Window");
 *   ...
 *
 * For internal ImGui APIs (drag-drop internals, dockspace internals)
 * use:
 *   #include <jce/tools/jce_imgui_internal.h>
 *
 * Layer placement: belongs to the Tools plane (alongside DCC importers
 * and asset cookers), NOT the runtime engine. Game code (caged_kingdom)
 * must NOT include this.
 */

#ifndef JCE_TOOLS_JCE_IMGUI_H
#define JCE_TOOLS_JCE_IMGUI_H

#include <jce/os/core/jce_defs.h>

/* ── Pre-defines (must come BEFORE imgui.h) ─────────────────────── */

/* Interop convenience: let ImVec2/ImVec4 implicitly construct from cglm
 * vec2/vec4. Disabled for now to keep ImGui ABI stable across TUs. */
/* #define IM_VEC2_CLASS_EXTRA ... */

/* NOTE: do NOT define IMGUI_USE_WCHAR32 here — the vendored imgui
 * library is built with the default 16-bit ImWchar; mismatching the
 * size in headers vs the .lib breaks ABI (AddFontFromMemoryTTF etc). */

/* ── Vendored Dear ImGui ────────────────────────────────────────── */
#include <imgui.h>

/* ── JCE convenience helpers ────────────────────────────────────── */

#ifdef __cplusplus
namespace jce_imgui {

/* Push the JCE editor default style colors (theme + accent).
 * Matches jce_editor_style.cpp. Pop with PopStyleColors(). */
inline void push_jce_theme() { /* implemented in editor/src/ui/jce_editor_style.cpp */ }

/* Returns true if any ImGui window is currently capturing keyboard
 * input (text edit, name field, etc.). Wraps the GetIO() check. */
inline bool wants_keyboard() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

/* Same for mouse. */
inline bool wants_mouse() {
    return ImGui::GetIO().WantCaptureMouse;
}

}  /* namespace jce_imgui */
#endif  /* __cplusplus */

#endif  /* JCE_TOOLS_JCE_IMGUI_H */
