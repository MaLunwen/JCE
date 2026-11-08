#ifndef JCE_EDITOR_VIEWPORT_WORK_H
#define JCE_EDITOR_VIEWPORT_WORK_H
#include "jce_editor_viewport_trace.h"
#include <jce/tools/jce_imgui.hpp>
#include <jce/os/platform/jce_window.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

/* NewFrame consumes focus events; EndFrame clears AppFocusLost again. Query
 * current native flags for each visible UI frame, including startup with no
 * delivered focus event. ImGui owns queue deduplication; unknown flags leave
 * its current observations and already queued events unchanged. */
inline void jce_editor_viewport_sync_window_focus(const JceWindow *window)
{
    uint32_t flags = 0;
    if (!window || !jce_window_get_flags(window, &flags)) return;
    const bool focused = (flags & JCE_WINDOW_INPUT_FOCUS) != 0 &&
        (flags & (JCE_WINDOW_HIDDEN | JCE_WINDOW_MINIMIZED)) == 0;
    ImGui::GetIO().AddFocusEvent(focused);
}

/* The engine already skips minimized host frames. These are the actual ImGui
 * viewport visibility and focus observations, not OS occlusion/presentation. */
enum class JceEditorFrameLogViewportPolicy { Capture, Natural, Invalid };

inline JceEditorFrameLogViewportPolicy jce_editor_frame_log_viewport_policy(const char *value)
{
    if (!value || !value[0] || std::strcmp(value, "capture") == 0)
        return JceEditorFrameLogViewportPolicy::Capture;
    if (std::strcmp(value, "natural") == 0)
        return JceEditorFrameLogViewportPolicy::Natural;
    return JceEditorFrameLogViewportPolicy::Invalid;
}

inline bool jce_editor_viewport_capture_requested(bool game_capture)
{
    const char *frame = std::getenv("JCE_KPI_FRAME_LOG");
    const char *draw = std::getenv("JCE_KPI_DRAW_LOG");
    /* Only recognized natural opts each logger out of its legacy override.
     * Invalid selectors retain capture-default and cannot certify natural.
     * Active Game capture keeps its independent precedence. */
    const bool frame_capture = frame && frame[0] &&
        jce_editor_frame_log_viewport_policy(std::getenv("JCE_KPI_FRAME_LOG_VIEWPORT")) !=
            JceEditorFrameLogViewportPolicy::Natural;
    const bool draw_capture = draw && draw[0] &&
        jce_editor_frame_log_viewport_policy(std::getenv("JCE_KPI_DRAW_LOG_VIEWPORT")) !=
            JceEditorFrameLogViewportPolicy::Natural;
    return game_capture || frame_capture || draw_capture;
}
inline bool jce_editor_viewport_wants_render(ImVec2 start, ImVec2 size,
                                             bool explicit_capture)
{
    if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(size.x) || !std::isfinite(size.y) ||
        size.x <= 0.0f || size.y <= 0.0f) return false;
    /* Explicit captures still render a positive, clipped/unfocused viewport;
     * invalid/empty layout cannot manufacture a rendered capture. */
    if (explicit_capture) return true;
    return !ImGui::GetIO().AppFocusLost &&
        ImGui::IsRectVisible(start, ImVec2(start.x + size.x, start.y + size.y));
}

/* Keep the original admission owner authoritative; this overload only explains
 * its result. Invalid layout precedes focus/clip, explicit capture precedes
 * valid-layout focus/clip. The three-argument CPU controls remain unchanged. */
inline bool jce_editor_viewport_wants_render(ImVec2 start, ImVec2 size,
                                            bool explicit_capture,
                                            const char *static_trace_name)
{
    const bool admitted = jce_editor_viewport_wants_render(start, size, explicit_capture);
    if (!jce_trace_enabled()) return admitted;
    JceEditorViewportPolicyReason reason;
    if (admitted) reason = explicit_capture ? JceEditorViewportPolicyReason::Capture
                                           : JceEditorViewportPolicyReason::Admitted;
    else if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
             !std::isfinite(size.x) || !std::isfinite(size.y) ||
             size.x <= 0.0f || size.y <= 0.0f)
        reason = JceEditorViewportPolicyReason::InvalidLayout;
    else reason = ImGui::GetIO().AppFocusLost ? JceEditorViewportPolicyReason::FocusLost
                                            : JceEditorViewportPolicyReason::Clipped;
    jce_editor_viewport_policy_trace(static_trace_name, reason);
    return admitted;
}
#endif
