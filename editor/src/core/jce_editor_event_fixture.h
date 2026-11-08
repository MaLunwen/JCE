#ifndef JCE_EDITOR_EVENT_FIXTURE_H
#define JCE_EDITOR_EVENT_FIXTURE_H

#include <jce/os/platform/jce_window_event.h>
#include <cmath>
#include <cfloat>
#include <climits>

inline bool jce_editor_fixture_integer(double value, double minimum, double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum &&
           std::floor(value) == value;
}

inline bool jce_editor_fixture_frame(double value, unsigned *out)
{
    if (!out || !jce_editor_fixture_integer(value, 0, UINT_MAX)) return false;
    *out = static_cast<unsigned>(value);
    return true;
}

/* Decode explicit QA input through the ordinary public event payload. The
 * caller owns Play-frame scheduling and viewport visibility; this sets neither. */
inline bool jce_editor_fixture_event(char kind, double x, double y,
    double a, double b, float vx, float vy, float width, float height, JceEvent *out)
{
    if (!out) return false;
    JceEvent event = {};
    if (kind == 'K') {
        if (!jce_editor_fixture_integer(a, 1, JCE_KEY_COUNT - 1) ||
            !jce_editor_fixture_integer(b, 0, 1)) return false;
        event.type = b == 1 ? JCE_EVENT_KEY_DOWN : JCE_EVENT_KEY_UP;
        event.key.scancode = static_cast<JceKey>(a);
        event.key.mod = JCE_KMOD_NONE;
        event.key.repeat = false;
    } else if (kind == 'W') {
        if (!std::isfinite(a) || std::fabs(a) > FLT_MAX) return false;
        event.type = JCE_EVENT_MOUSE_WHEEL;
        event.wheel.y = static_cast<float>(a);
    } else if (kind == 'M' || kind == 'B') {
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(vx) || !std::isfinite(vy) ||
            !std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0) return false;
        const double px = vx + x * width, py = vy + y * height;
        if (!std::isfinite(px) || !std::isfinite(py) ||
            std::fabs(px) > FLT_MAX || std::fabs(py) > FLT_MAX) return false;
        if (kind == 'M') {
            const double dx = a * width, dy = b * height;
            if (!std::isfinite(dx) || !std::isfinite(dy) ||
                std::fabs(dx) > FLT_MAX || std::fabs(dy) > FLT_MAX) return false;
            event.type = JCE_EVENT_MOUSE_MOTION;
            event.motion.x = static_cast<float>(px);
            event.motion.y = static_cast<float>(py);
            event.motion.xrel = static_cast<float>(dx);
            event.motion.yrel = static_cast<float>(dy);
        } else {
            if (!jce_editor_fixture_integer(a, JCE_MOUSE_BUTTON_LEFT, JCE_MOUSE_BUTTON_X2) ||
                !jce_editor_fixture_integer(b, 0, 1)) return false;
            event.type = b == 1 ? JCE_EVENT_MOUSE_BUTTON_DOWN : JCE_EVENT_MOUSE_BUTTON_UP;
            event.button.x = static_cast<float>(px);
            event.button.y = static_cast<float>(py);
            event.button.button = static_cast<uint8_t>(a);
        }
    } else return false;
    *out = event;
    return true;
}

#endif
