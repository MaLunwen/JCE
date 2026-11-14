/* Private bounded light response for the authored foliage toon ramp.
 * Input colors are linear RGB. Time-of-day may carry its changing energy
 * in RGB with scalar intensity one, so neither factor can be ignored.
 * The ramp still owns its daytime shape; this is not a second Lambert term.
 * Zero incident light has no surface emission. Fog is applied separately. */
#ifndef JCE_SR_FOLIAGE_LIGHTING_H
#define JCE_SR_FOLIAGE_LIGHTING_H

#include <math.h>

static inline float sr_foliage_incident_channel(float color, float intensity)
{
    if (!isfinite(color) || !isfinite(intensity) ||
        color <= 0.0f || intensity <= 0.0f)
        return 0.0f;
    /* Double avoids overflow for finite authored float values. */
    const double energy = (double)color * (double)intensity;
    return energy >= 1.0 ? 1.0f : (float)energy;
}

static inline void sr_foliage_lighting_multiplier(const float authored[3],
    const float direct[3], float direct_intensity,
    const float ambient[3], float ambient_intensity, float output[3])
{
    for (unsigned i = 0; i < 3u; ++i) {
        const float energy = sr_foliage_incident_channel(direct[i], direct_intensity) +
            sr_foliage_incident_channel(ambient[i], ambient_intensity);
        /* Full daylight preserves the existing authored palette exactly.
         * At night the same palette follows actual light energy and hue. */
        output[i] = authored[i] * (energy >= 1.0f ? 1.0f : energy);
    }
}

#endif
