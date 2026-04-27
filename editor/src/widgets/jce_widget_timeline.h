/*
 * jce_widget_timeline.h — shared timeline drawing primitives.
 *
 * Reusable building blocks for any panel that displays a horizontal
 * time-based ruler with tracks, keyframes and a playhead (Timeline,
 * Sequencer, Animation Editor, Animator State Machine timeline view…).
 *
 * All coordinates are in screen space (use ImGui::GetCursorScreenPos()
 * as origin). All functions are pure draw calls — they do NOT advance
 * the layout cursor; callers should follow up with ImGui::Dummy().
 */

#ifndef JCE_WIDGET_TIMELINE_H
#define JCE_WIDGET_TIMELINE_H

#include <jce/tools/jce_imgui.h>

#ifdef __cplusplus
extern "C++" {
#endif

/* Draw ruler with minor/major tick marks and time labels (e.g. "1s").
 * origin       : top-left in screen space
 * total_width  : width of ruler area in pixels
 * duration     : visible time range in seconds
 * px_per_sec   : zoom level
 * minor_step   : seconds between minor ticks (e.g. 0.1)
 * major_step   : seconds between major ticks + labels (e.g. 1.0) */
void jce_widget_timeline_ruler(ImVec2 origin, float total_width,
                               float duration, float px_per_sec,
                               float minor_step, float major_step);

/* Draw alternating track background row at given screen Y. */
void jce_widget_timeline_track_bg(ImVec2 origin, float total_width,
                                  float row_y, float row_height,
                                  int row_index);

/* Draw vertical playhead line from origin.y to origin.y + height. */
void jce_widget_timeline_playhead(ImVec2 origin, float height,
                                  float current_time, float px_per_sec,
                                  ImU32 color, float thickness);

/* Draw a keyframe diamond centred at screen pos. */
void jce_widget_timeline_keyframe(ImVec2 center, float radius, ImU32 color);

/* Handle click + drag on the timeline area to scrub current_time.
 * Returns true if user interacted this frame.
 * Caller is responsible for calling this only when the timeline child
 * window is hovered. */
bool jce_widget_timeline_scrub(ImVec2 origin, float total_width,
                               float duration, float px_per_sec,
                               float *current_time);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WIDGET_TIMELINE_H */
