/*
 * jce_postfx_apply.c  Build per-frame post-FX pipeline from stack.
 *
 * Ping-pongs RT slots 0 and 1 between consecutive enabled passes —
 * each pass reads the previous output slot and writes the alternate.
 * The final output slot is whatever the last pass wrote to.
 */

#include <jce/renderer/jce_postfx_apply.h>

#include <math.h>
#include <string.h>

static void push_pass(JcePostFxPipeline *p, JcePostFxPassKind k,
                       const float a[4], const float b[4],
                       uint8_t in_slot, uint8_t out_slot)
{
    if (p->pass_count >= JCE_POSTFX_MAX_PASSES) return;
    JcePostFxPassDesc *d = &p->passes[p->pass_count++];
    d->kind = k;
    d->input_slot  = in_slot;
    d->output_slot = out_slot;
    memcpy(d->params0, a, sizeof(d->params0));
    memcpy(d->params1, b, sizeof(d->params1));
}

void jce_postfx_apply_build(const JcePostFxStackComponent *s,
                              JcePostFxPipeline *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->exposure_gain = 1.0f;
    if (!s) return;

    /* Exposure baked into gain (passed through as a uniform; not a
     * pass on its own — applied by tonemap). */
    out->exposure_gain = powf(2.0f, s->color.enabled ? s->color.exposure_ev
                                                      : 0.0f);

    uint8_t in_slot  = 0;
    uint8_t out_slot = 1;
    bool any = false;

    if (s->bloom.enabled) {
        float a[4] = { s->bloom.threshold, s->bloom.intensity,
                       s->bloom.scatter,   0 };
        float b[4] = { s->bloom.tint[0], s->bloom.tint[1], s->bloom.tint[2], 0 };
        push_pass(out, JCE_POSTFX_PASS_BLOOM, a, b, in_slot, out_slot);
        in_slot ^= 1; out_slot ^= 1;
        any = true;
    }
    if (s->ca.enabled) {
        float a[4] = { s->ca.intensity, 0, 0, 0 };
        float b[4] = { 0 };
        push_pass(out, JCE_POSTFX_PASS_CA, a, b, in_slot, out_slot);
        in_slot ^= 1; out_slot ^= 1;
        any = true;
    }
    if (s->vignette.enabled) {
        float a[4] = { s->vignette.intensity, s->vignette.smoothness, 0, 0 };
        float b[4] = { s->vignette.color[0], s->vignette.color[1],
                       s->vignette.color[2], 0 };
        push_pass(out, JCE_POSTFX_PASS_VIGNETTE, a, b, in_slot, out_slot);
        in_slot ^= 1; out_slot ^= 1;
        any = true;
    }
    if (s->grain.enabled) {
        float a[4] = { s->grain.intensity, s->grain.response,
                       (float)s->grain.type, 0 };
        float b[4] = { 0 };
        push_pass(out, JCE_POSTFX_PASS_GRAIN, a, b, in_slot, out_slot);
        in_slot ^= 1; out_slot ^= 1;
        any = true;
    }
    if (s->color.enabled) {
        float a[4] = { s->color.contrast,
                       s->color.saturation,
                       s->color.hue_shift_deg, 0 };
        float b[4] = { s->color.color_filter[0],
                       s->color.color_filter[1],
                       s->color.color_filter[2], 0 };
        push_pass(out, JCE_POSTFX_PASS_COLORADJ, a, b, in_slot, out_slot);
        in_slot ^= 1; out_slot ^= 1;
        any = true;
    }
    (void)any;
}

const char *jce_postfx_pass_kind_name(JcePostFxPassKind k)
{
    switch (k) {
    case JCE_POSTFX_PASS_BLOOM:    return "Bloom";
    case JCE_POSTFX_PASS_CA:       return "Chromatic Aberration";
    case JCE_POSTFX_PASS_VIGNETTE: return "Vignette";
    case JCE_POSTFX_PASS_GRAIN:    return "Film Grain";
    case JCE_POSTFX_PASS_COLORADJ: return "Color Adjust";
    default:                       return "(none)";
    }
}
