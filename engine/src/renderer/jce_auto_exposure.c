/*
 * jce_auto_exposure.c — the adaptation law.  Arithmetic on one number.
 *
 * See the header for why this is separate from the measurement.  Nothing here
 * touches a texture, a frame, or the GPU, which is what lets the properties
 * that matter be asserted rather than looked at.
 */

#include <jce/renderer/jce_auto_exposure.h>

#include <jce/os/core/jce_math.h>   /* jce_half_to_float */

#include <math.h>

/* The floor a measured luminance is clamped to before log2.
 *
 * NOT an epsilon chosen to "avoid a warning": 1e-5 cd/m^2 is far below
 * anything a rendered frame contains and comfortably above float denormals,
 * so the clamp only ever engages on a frame that is genuinely black -- the
 * first frame, a fade, a fully occluded shot -- and on those it produces
 * min_ev rather than -inf. */
#define AE_LUM_FLOOR 1.0e-5f

JCE_API JceAutoExposureDesc jce_auto_exposure_desc_default(void)
{
    JceAutoExposureDesc d;
    /* A 16-stop window.  Wide enough for a night interior to a noon exterior,
     * narrow enough that a single blown-out specular cannot drag the whole
     * frame dark for the second it takes to adapt back. */
    d.min_ev        = -8.0f;
    d.max_ev        =  8.0f;
    /* Dark -> bright is roughly three times faster than the reverse, which is
     * the asymmetry the eye has and the one the compared engines model. */
    d.speed_up      =  3.0f;
    d.speed_down    =  1.0f;
    d.exposure_bias =  0.0f;
    d.key           =  0.18f;
    return d;
}

/* An order of magnitude above AE_LUM_FLOOR: high enough that an all-zero
 * read-back can never clear it, low enough that no lit frame can fail it
 * (this is about eleven stops below middle grey). */
#define AE_USABLE_MIN (AE_LUM_FLOOR * 10.0f)

JCE_API bool jce_auto_exposure_measurement_is_usable(float avg_luminance)
{
    return isfinite(avg_luminance) && avg_luminance > AE_USABLE_MIN;
}

JCE_API float jce_auto_exposure_target_ev(float avg_luminance,
                                          const JceAutoExposureDesc *desc)
{
    const JceAutoExposureDesc d = desc ? *desc : jce_auto_exposure_desc_default();

    /* isfinite FIRST.  A NaN compares false against every bound, so a plain
     * `if (l < floor)` lets it straight through and the NaN reaches log2, the
     * exposure, and then every pixel.  A readback that arrived short, or a
     * target that was never rendered, produces exactly this. */
    float l = avg_luminance;
    if (!isfinite(l) || l < AE_LUM_FLOOR) l = AE_LUM_FLOOR;

    const float key = (d.key > 0.0f) ? d.key : 0.18f;

    /* The exposure that maps the scene's average onto the key.  In EV:
     *   multiplier = key / L   =>   ev = log2(key / L)
     * Brighter scene -> smaller multiplier -> lower EV, which is the sign
     * convention every case below depends on. */
    float ev = log2f(key / l) + d.exposure_bias;

    /* Clamp, and tolerate a caller who passed the bounds the wrong way round
     * rather than returning something between two numbers that do not bracket
     * anything. */
    float lo = d.min_ev, hi = d.max_ev;
    if (lo > hi) { const float t = lo; lo = hi; hi = t; }
    if (ev < lo) ev = lo;
    if (ev > hi) ev = hi;
    return ev;
}

JCE_API float jce_auto_exposure_step(float current_ev, float target_ev,
                                     float dt,
                                     const JceAutoExposureDesc *desc)
{
    const JceAutoExposureDesc d = desc ? *desc : jce_auto_exposure_desc_default();

    if (!isfinite(current_ev)) return target_ev;   /* recover, do not spread */
    if (!isfinite(target_ev))  return current_ev;
    if (!isfinite(dt) || dt <= 0.0f) return current_ev;

    const float delta = target_ev - current_ev;
    if (delta == 0.0f) return current_ev;

    /* The scene got BRIGHTER when the target EV is LOWER (see the sign
     * convention above), and that is the fast direction. */
    const float speed = (delta < 0.0f) ? d.speed_up : d.speed_down;
    if (!(speed > 0.0f)) return current_ev;        /* 0 or negative = frozen */

    const float move = speed * dt;
    if (move >= fabsf(delta)) return target_ev;    /* land ON it, never past */

    return current_ev + ((delta < 0.0f) ? -move : move);
}

JCE_API float jce_auto_exposure_log_average_rgba16f(const uint16_t *rgba16f,
                                                    uint32_t texel_count)
{
    if (!rgba16f || texel_count == 0u) return 0.0f;

    /* Accumulate in double.  A 64x64 target is 4096 logs of values that can
     * each be 1e-5; in float the running sum loses the small ones once it has
     * grown, and the answer drifts with the ORDER of the pixels. */
    double sum = 0.0;
    for (uint32_t i = 0; i < texel_count; ++i) {
        const uint16_t *h = rgba16f + (size_t)i * 4u;
        const float r = jce_half_to_float(h[0]);
        const float g = jce_half_to_float(h[1]);
        const float b = jce_half_to_float(h[2]);
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        /* isfinite FIRST, for the same reason as in target_ev: a NaN compares
         * false against every bound, so a bare `l < floor` lets it into log
         * and one bad texel then poisons the whole frame's exposure. */
        if (!isfinite(l) || l < AE_LUM_FLOOR) l = AE_LUM_FLOOR;
        sum += (double)logf(l);
    }
    return expf((float)(sum / (double)texel_count));
}

JCE_API float jce_auto_exposure_multiplier(float ev)
{
    if (!isfinite(ev)) return 1.0f;
    return exp2f(ev);
}
