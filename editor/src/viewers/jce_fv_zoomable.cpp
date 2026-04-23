/*
 * jce_fv_zoomable.cpp  Shared zoomable-image canvas helper.
 *
 * Used by image and video viewers to provide identical pan/zoom UX:
 *   - Mouse wheel : zoom centered on cursor
 *   - Left drag   : pan
 *   - Middle drag : pan (alternate)
 *   - Double LMB  : toggle Fit ↔ 1:1
 *   - Drawn inside an ImGui child window; uses an InvisibleButton to
 *     receive the input regardless of the texture itself being clickable.
 */

#include "jce_fv_common.h"

static const float ZOOM_MIN = 0.05f;
static const float ZOOM_MAX = 16.0f;

static float clamp_zoom(float z)
{
    if (z < ZOOM_MIN) return ZOOM_MIN;
    if (z > ZOOM_MAX) return ZOOM_MAX;
    return z;
}

void fv_zoomable_fit(const FvZoomable *p, ImVec2 avail)
{
    if (!p || !p->zoom || p->content_w <= 0 || p->content_h <= 0) return;
    float sx = avail.x / (float)p->content_w;
    float sy = avail.y / (float)p->content_h;
    float fit = (sx < sy) ? sx : sy;
    *p->zoom = clamp_zoom(fit);
    if (p->pan_x) *p->pan_x = 0.0f;
    if (p->pan_y) *p->pan_y = 0.0f;
}

void fv_zoomable_one_to_one(const FvZoomable *p)
{
    if (!p || !p->zoom) return;
    *p->zoom = 1.0f;
    if (p->pan_x) *p->pan_x = 0.0f;
    if (p->pan_y) *p->pan_y = 0.0f;
}

void fv_render_zoomable(const FvZoomable *p)
{
    if (!p || !p->zoom || !p->pan_x || !p->pan_y) return;
    if (p->content_w <= 0 || p->content_h <= 0) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 16.0f) avail.x = 16.0f;
    if (avail.y < 16.0f) avail.y = 16.0f;

    /* Auto-fit on first render (zoom unset). */
    if (*p->zoom <= 0.0f) {
        fv_zoomable_fit(p, avail);
    }

    float zoom = clamp_zoom(*p->zoom);
    *p->zoom = zoom;

    float disp_w = (float)p->content_w * zoom;
    float disp_h = (float)p->content_h * zoom;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    uint32_t matte = p->matte_color ? p->matte_color : IM_COL32(20, 20, 24, 255);
    dl->AddRectFilled(origin,
                      ImVec2(origin.x + avail.x, origin.y + avail.y),
                      matte);

    /* Center, then apply pan. */
    float cx = (avail.x - disp_w) * 0.5f + *p->pan_x;
    float cy = (avail.y - disp_h) * 0.5f + *p->pan_y;
    ImVec2 img_pos = ImVec2(origin.x + cx, origin.y + cy);

    if (jce_texture_valid(p->tex)) {
        dl->PushClipRect(origin,
                         ImVec2(origin.x + avail.x, origin.y + avail.y),
                         true);
        dl->AddImage((ImTextureID)(uintptr_t)p->tex.idx,
                     img_pos,
                     ImVec2(img_pos.x + disp_w, img_pos.y + disp_h));
        dl->PopClipRect();
    }

    /* Input layer — invisible button covers the whole canvas. */
    ImGui::InvisibleButton("##fv_zoomable", avail,
                           ImGuiButtonFlags_MouseButtonLeft
                           | ImGuiButtonFlags_MouseButtonMiddle
                           | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    /* Wheel zoom centered on cursor. */
    if (hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float old_zoom = zoom;
            float factor = (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
            for (int i = 1; i < (int)fabsf(wheel); ++i) factor *= factor;
            float new_zoom = clamp_zoom(old_zoom * factor);

            /* Keep the pixel under the cursor stationary. */
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float mx = mouse.x - origin.x - avail.x * 0.5f;
            float my = mouse.y - origin.y - avail.y * 0.5f;
            float scale = new_zoom / old_zoom;
            *p->pan_x = (*p->pan_x) * scale + mx * (1.0f - scale);
            *p->pan_y = (*p->pan_y) * scale + my * (1.0f - scale);
            *p->zoom = new_zoom;
        }
    }

    /* Drag pan (left or middle). */
    if (active) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            *p->pan_x += d.x;
            *p->pan_y += d.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            *p->pan_x += d.x;
            *p->pan_y += d.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    /* Double-click toggles Fit ↔ 1:1. */
    if (p->allow_double_click_toggle && hovered
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        float fit_sx = avail.x / (float)p->content_w;
        float fit_sy = avail.y / (float)p->content_h;
        float fit = (fit_sx < fit_sy) ? fit_sx : fit_sy;
        if (fabsf(zoom - 1.0f) < 0.001f) {
            /* Currently 1:1 → fit. */
            *p->zoom = clamp_zoom(fit);
            *p->pan_x = 0.0f;
            *p->pan_y = 0.0f;
        } else {
            fv_zoomable_one_to_one(p);
        }
    }
}
