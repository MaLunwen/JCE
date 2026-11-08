/* Engine-session launch values. Typed settings, not truthy environment flags. */
#ifndef JCE_ENGINE_LAUNCH_POLICY_H
#define JCE_ENGINE_LAUNCH_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>

typedef struct JceEngineLaunchPolicy {
    bool vsync_present, vsync;
    bool fps_present;
    int fps;
} JceEngineLaunchPolicy;

/* Optional finite-run duration; preserve the existing bootstrap scalar policy. */
static inline double engine_launch_positive_seconds(const char *value)
{
    char *end = NULL;
    double seconds;
    if (!value || !value[0]) return 0.0;
    seconds = strtod(value, &end);
    if (end == value || *end != '\0' || !(seconds > 0.0) ||
        seconds > 86400.0) return 0.0;
    return seconds;
}

static inline bool engine_launch_uint(const char *text, int maximum, int *out)
{
    int value = 0;
    if (!text || !text[0]) return false;
    for (const char *p = text; *p; ++p) {
        const unsigned digit = (unsigned char)*p - (unsigned)'0';
        if (digit > 9u || value > (maximum - (int)digit) / 10 ||
            (int)digit > maximum) return false;
        value = value * 10 + (int)digit;
    }
    *out = value;
    return true;
}

static inline const char *engine_launch_parse(const char *vsync, const char *fps,
                                              JceEngineLaunchPolicy *out)
{
    JceEngineLaunchPolicy parsed = {0};
    int value;
    if (!out) return "launch policy destination";
    if (vsync) {
        if (!((vsync[0] == '0' || vsync[0] == '1') && vsync[1] == '\0'))
            return "JCE_LAUNCH_VSYNC";
        parsed.vsync_present = true; parsed.vsync = vsync[0] == '1';
    }
    if (fps) {
        if (!engine_launch_uint(fps, INT_MAX, &value)) return "JCE_LAUNCH_TARGET_FPS";
        parsed.fps_present = true; parsed.fps = value;
    }
    *out = parsed;
    return NULL;
}
#endif
