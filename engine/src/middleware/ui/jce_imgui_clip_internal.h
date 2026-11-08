/* Private ImGui scissor admission. Current host vertex/clip coordinates are
 * drawable pixels (FramebufferScale=1); DisplayPos is removed exactly once.
 * No OS query, allocation, renderer mutation or texture servicing lives here. */
#ifndef JCE_IMGUI_CLIP_INTERNAL_H
#define JCE_IMGUI_CLIP_INTERNAL_H
#include <cmath>
#include <cfloat>
#include <cstdint>

struct JceImGuiClipRect {
    uint16_t x = 0, y = 0, width = 0, height = 0;
};
static double jce_imgui_clip_relative(float value, float origin)
{
    const double relative = double(value) - double(origin);
    /* Match the old float subtraction when representable, including rounding
     * at an integer edge. Widen only its exceptional overflow range. */
    return relative >= -double(FLT_MAX) && relative <= double(FLT_MAX)
        ? double(static_cast<float>(relative)) : relative;
}
static bool jce_imgui_clip_intersect(float min_x, float min_y, float max_x, float max_y,
    float display_x, float display_y, uint32_t framebuffer_width,
    uint32_t framebuffer_height, JceImGuiClipRect *out)
{
    if (!out) return false;
    *out = {};
    if (!framebuffer_width || !framebuffer_height ||
        framebuffer_width > UINT16_MAX || framebuffer_height > UINT16_MAX ||
        !std::isfinite(min_x) || !std::isfinite(min_y) ||
        !std::isfinite(max_x) || !std::isfinite(max_y) ||
        !std::isfinite(display_x) || !std::isfinite(display_y)) return false;

    /* Widen before subtracting: finite float endpoints can overflow float
     * subtraction, but cannot overflow these double intermediates. */
    const double left = jce_imgui_clip_relative(min_x, display_x);
    const double top = jce_imgui_clip_relative(min_y, display_y);
    const double right = jce_imgui_clip_relative(max_x, display_x);
    const double bottom = jce_imgui_clip_relative(max_y, display_y);
    const double limit_x = double(framebuffer_width);
    const double limit_y = double(framebuffer_height);
    const double clipped_left = left < 0.0 ? 0.0 : left;
    const double clipped_top = top < 0.0 ? 0.0 : top;
    const double clipped_right = right > limit_x ? limit_x : right;
    const double clipped_bottom = bottom > limit_y ? limit_y : bottom;
    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) return false;

    /* Preserve the existing valid-pixel rounding: truncate the positive origin,
     * then truncate max-origin. All conversions are now finite and in range. */
    if (clipped_left >= double(UINT16_MAX) || clipped_top >= double(UINT16_MAX) ||
        clipped_right > double(UINT16_MAX) || clipped_bottom > double(UINT16_MAX)) return false;
    const uint16_t x = static_cast<uint16_t>(clipped_left);
    const uint16_t y = static_cast<uint16_t>(clipped_top);
    const double width = clipped_right - double(x);
    const double height = clipped_bottom - double(y);
    if (width < 1.0 || height < 1.0 || width > double(UINT16_MAX) || height > double(UINT16_MAX)) return false;
    out->x = x; out->y = y;
    out->width = static_cast<uint16_t>(width);
    out->height = static_cast<uint16_t>(height);
    return true;
}
#endif
