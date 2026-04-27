/*
 * jce_widget_timeline.cpp — implementation of shared timeline drawing.
 */

#include "jce_widget_timeline.h"
#include "ui/jce_theme_palette.h"

#include <math.h>
#include <stdio.h>

void jce_widget_timeline_ruler(ImVec2 origin, float total_width,
                               float duration, float px_per_sec,
                               float minor_step, float major_step)
{
    if (duration <= 0.0f || px_per_sec <= 0.0f || minor_step <= 0.0f) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 col_minor = jce_theme::grid_minor();
    const ImU32 col_major = jce_theme::text_primary();

    for (float t = 0.0f; t <= duration + 0.001f; t += minor_step) {
        float x = origin.x + t * px_per_sec;
        if (x > origin.x + total_width) break;
        bool major = (fmodf(t + 0.001f, major_step) < minor_step * 0.5f);
        float h = major ? 16.0f : 8.0f;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + h),
                    major ? col_major : col_minor);
        if (major) {
            char label[16];
            snprintf(label, sizeof(label), "%.0fs", t);
            dl->AddText(ImVec2(x + 2.0f, origin.y), col_major, label);
        }
    }
}

void jce_widget_timeline_track_bg(ImVec2 origin, float total_width,
                                  float row_y, float row_height, int row_index)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 bg = (row_index % 2 == 0) ? jce_theme::track_even()
                                    : jce_theme::track_odd();
    dl->AddRectFilled(ImVec2(origin.x, row_y),
                      ImVec2(origin.x + total_width, row_y + row_height), bg);
}

void jce_widget_timeline_playhead(ImVec2 origin, float height,
                                  float current_time, float px_per_sec,
                                  ImU32 color, float thickness)
{
    float x = origin.x + current_time * px_per_sec;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height),
                color, thickness);
}

void jce_widget_timeline_keyframe(ImVec2 c, float r, ImU32 color)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 pts[4] = {
        ImVec2(c.x,     c.y - r),
        ImVec2(c.x + r, c.y),
        ImVec2(c.x,     c.y + r),
        ImVec2(c.x - r, c.y),
    };
    dl->AddConvexPolyFilled(pts, 4, color);
    dl->AddPolyline(pts, 4, jce_theme::node_outline(), ImDrawFlags_Closed, 1.0f);
}

bool jce_widget_timeline_scrub(ImVec2 origin, float total_width,
                               float duration, float px_per_sec,
                               float *current_time)
{
    if (!current_time || duration <= 0.0f) return false;
    if (!ImGui::IsWindowHovered()) return false;

    bool clicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left)
                 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
    if (!clicked && !dragging) return false;

    float mx = ImGui::GetMousePos().x - origin.x;
    if (mx < 0.0f) mx = 0.0f;
    if (mx > total_width) mx = total_width;
    float t = mx / px_per_sec;
    if (t < 0.0f)      t = 0.0f;
    if (t > duration)  t = duration;
    *current_time = t;
    return true;
}
