/*
 * jce_vcam_confiner.c  Clamp a vcam position into an AABB.
 *
 * Damping_radius lerps the position toward the centre of the
 * shrunk-AABB face when within radius — gives Cinemachine's
 * "pull-back" feel rather than a hard snap.
 */

#include <jce/middleware/scene/jce_vcam_confiner.h>

#include <math.h>
#include <string.h>

void jce_vcam_confiner3d_from_center(JceVcamConfiner3D *out,
                                       const float center[3],
                                       const float half[3], float damp)
{
    if (!out || !center || !half) return;
    for (int i = 0; i < 3; ++i) {
        out->min[i] = center[i] - half[i];
        out->max[i] = center[i] + half[i];
    }
    out->damping_radius = damp;
    out->confine_y = true;
    out->active = true;
}

static float clamp_axis(float v, float lo, float hi,
                          float damp, bool *modified)
{
    if (v < lo) { *modified = true; return lo; }
    if (v > hi) { *modified = true; return hi; }
    if (damp <= 0.0f) return v;
    /* Pull inside soft-zone with quadratic ease. */
    float over_lo = v - (lo + damp);
    float over_hi = (hi - damp) - v;
    if (over_lo < 0.0f) {
        float t = -over_lo / damp;
        if (t > 1.0f) t = 1.0f;
        float pull = t * t * damp;
        v += pull;
        *modified = true;
    } else if (over_hi < 0.0f) {
        float t = -over_hi / damp;
        if (t > 1.0f) t = 1.0f;
        float pull = t * t * damp;
        v -= pull;
        *modified = true;
    }
    return v;
}

bool jce_vcam_confiner3d_clamp(const JceVcamConfiner3D *c, float pos[3])
{
    if (!c || !c->active || !pos) return false;
    bool modified = false;
    pos[0] = clamp_axis(pos[0], c->min[0], c->max[0],
                         c->damping_radius, &modified);
    if (c->confine_y)
        pos[1] = clamp_axis(pos[1], c->min[1], c->max[1],
                             c->damping_radius, &modified);
    pos[2] = clamp_axis(pos[2], c->min[2], c->max[2],
                         c->damping_radius, &modified);
    return modified;
}
