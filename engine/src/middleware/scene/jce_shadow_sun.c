#include "jce_shadow_sun.h"

#include <math.h>

float jce_shadow_sun_quantum(float cascade_extent_m, uint32_t map_size,
                             float max_caster_height_m)
{
    if (!(cascade_extent_m > 0.0f) || map_size == 0u ||
        !(max_caster_height_m > 0.0f))
        return 0.0f;                     /* degenerate -> re-render every frame */

    const float texel = cascade_extent_m / (float)map_size;
    const float q     = texel / max_caster_height_m;   /* tan(x) ~ x here */

    /* Cap at a degree.  A short cascade over a flat scene can derive a quantum
     * of several degrees, and at that point the shadows lag the sun by an
     * amount a viewer watching a sunset would notice -- the derivation is about
     * texel snapping, and it stops describing the visible result long before
     * the number gets that large. */
    const float cap = 0.01745329f;                     /* 1 degree in radians */
    return (q > cap) ? cap : q;
}

bool jce_shadow_sun_update(JceShadowSunHold *hold, const float dir[3],
                           float quantum_rad, float out_dir[3])
{
    if (!hold || !dir || !out_dir) return false;

    const float len2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!(len2 > 1e-12f)) {
        /* A zero sun direction is meaningless; hold whatever we have rather
         * than anchoring the maps to a degenerate light. */
        if (hold->held) {
            out_dir[0] = hold->dir[0];
            out_dir[1] = hold->dir[1];
            out_dir[2] = hold->dir[2];
        } else {
            out_dir[0] = out_dir[1] = out_dir[2] = 0.0f;
        }
        return false;
    }

    const float inv = 1.0f / sqrtf(len2);
    const float nx = dir[0] * inv, ny = dir[1] * inv, nz = dir[2] * inv;

    if (hold->held && quantum_rad > 0.0f) {
        /* Compare CHORD length, not the angle.  acos() near zero is where its
         * condition number diverges, so the small differences that matter most
         * here are exactly the ones it resolves worst.  The chord subtended by
         * an angle t is 2*sin(t/2), which is smooth and monotonic across the
         * whole range we care about, and squaring it removes the sqrt. */
        const float dx = nx - hold->dir[0];
        const float dy = ny - hold->dir[1];
        const float dz = nz - hold->dir[2];
        const float chord2 = dx * dx + dy * dy + dz * dz;

        const float lim = 2.0f * sinf(0.5f * quantum_rad);
        if (chord2 <= lim * lim) {
            out_dir[0] = hold->dir[0];
            out_dir[1] = hold->dir[1];
            out_dir[2] = hold->dir[2];
            return false;
        }
    }

    /* Re-anchor on the TRUE sun.  The obvious alternative -- snapping the sun
     * onto a fixed angular grid and re-rendering whenever the bin changes --
     * has no deadband at all, so a sun sitting on a bin edge lands in a
     * different bin every frame from float noise alone: the shadows twitch by a
     * full quantum back and forth AND the cache misses every frame, which is
     * strictly worse than not quantising.  An anchor that moves with the sun
     * has no bin edges to sit on.
     *
     * (Snapping the anchor *after* this deadband check is a different matter --
     * measurably it changes almost nothing, because the deadband bounds the
     * error either way.  It is simply not worth the trig.) */
    hold->dir[0] = nx; hold->dir[1] = ny; hold->dir[2] = nz;
    hold->held   = true;
    out_dir[0] = nx; out_dir[1] = ny; out_dir[2] = nz;
    return true;
}
