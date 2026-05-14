/*
 * jce_postfx_stack.c  PostFX stack defaults + profile blending.
 */

#include <jce/middleware/scene/jce_postfx_stack.h>

#include <string.h>

JcePostFxStackComponent jce_postfx_stack_default(void)
{
    JcePostFxStackComponent s;
    memset(&s, 0, sizeof(s));

    s.bloom.threshold  = 1.0f;
    s.bloom.intensity  = 1.0f;
    s.bloom.scatter    = 0.7f;
    s.bloom.tint[0]    = 1.0f;
    s.bloom.tint[1]    = 1.0f;
    s.bloom.tint[2]    = 1.0f;

    s.ca.intensity     = 0.0f;

    s.vignette.intensity  = 0.0f;
    s.vignette.smoothness = 0.5f;
    s.vignette.color[0]   = 0.0f;
    s.vignette.color[1]   = 0.0f;
    s.vignette.color[2]   = 0.0f;

    s.grain.type      = JCE_FILM_GRAIN_THIN1;
    s.grain.intensity = 0.0f;
    s.grain.response  = 0.8f;

    s.color.exposure_ev   = 0.0f;
    s.color.contrast      = 0.0f;
    s.color.saturation    = 0.0f;
    s.color.hue_shift_deg = 0.0f;
    s.color.color_filter[0] = 1.0f;
    s.color.color_filter[1] = 1.0f;
    s.color.color_filter[2] = 1.0f;

    s.priority     = 0;
    s.blend_radius = 0.0f;
    return s;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

JcePostFxStackComponent jce_postfx_stack_blend(
    JcePostFxStackComponent a, JcePostFxStackComponent b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    JcePostFxStackComponent r;

    /* Boolean enables follow the stronger weight. */
    bool pick_b = t > 0.5f;

    r.bloom.enabled    = pick_b ? b.bloom.enabled    : a.bloom.enabled;
    r.bloom.threshold  = lerp(a.bloom.threshold,  b.bloom.threshold,  t);
    r.bloom.intensity  = lerp(a.bloom.intensity,  b.bloom.intensity,  t);
    r.bloom.scatter    = lerp(a.bloom.scatter,    b.bloom.scatter,    t);
    for (int i = 0; i < 3; ++i)
        r.bloom.tint[i] = lerp(a.bloom.tint[i], b.bloom.tint[i], t);

    r.ca.enabled       = pick_b ? b.ca.enabled : a.ca.enabled;
    r.ca.intensity     = lerp(a.ca.intensity,   b.ca.intensity,   t);

    r.vignette.enabled    = pick_b ? b.vignette.enabled : a.vignette.enabled;
    r.vignette.intensity  = lerp(a.vignette.intensity,  b.vignette.intensity,  t);
    r.vignette.smoothness = lerp(a.vignette.smoothness, b.vignette.smoothness, t);
    for (int i = 0; i < 3; ++i)
        r.vignette.color[i] = lerp(a.vignette.color[i], b.vignette.color[i], t);

    r.grain.enabled   = pick_b ? b.grain.enabled : a.grain.enabled;
    r.grain.type      = pick_b ? b.grain.type    : a.grain.type;
    r.grain.intensity = lerp(a.grain.intensity, b.grain.intensity, t);
    r.grain.response  = lerp(a.grain.response,  b.grain.response,  t);

    r.color.enabled      = pick_b ? b.color.enabled : a.color.enabled;
    r.color.exposure_ev  = lerp(a.color.exposure_ev,  b.color.exposure_ev,  t);
    r.color.contrast     = lerp(a.color.contrast,     b.color.contrast,     t);
    r.color.saturation   = lerp(a.color.saturation,   b.color.saturation,   t);
    r.color.hue_shift_deg= lerp(a.color.hue_shift_deg,b.color.hue_shift_deg,t);
    for (int i = 0; i < 3; ++i)
        r.color.color_filter[i] = lerp(a.color.color_filter[i],
                                       b.color.color_filter[i], t);

    /* Discrete metadata pulled from whichever side dominates. */
    r.priority     = pick_b ? b.priority     : a.priority;
    r.blend_radius = lerp(a.blend_radius, b.blend_radius, t);
    return r;
}
