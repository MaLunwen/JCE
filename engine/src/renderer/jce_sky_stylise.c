#include "renderer/jce_sky_stylise.h"

#include <math.h>
#include <string.h>

/* Rec.709 luminance, the same weights the tonemapper uses.  Using a different
 * set here would make the grade disagree with exposure about which parts of the
 * sky are bright, and the disagreement shows up as the horizon posterising at a
 * different threshold than the zenith. */
static float sky_luma(const float rgb[3])
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

JceSkyStylise jce_sky_stylise_identity(void)
{
    JceSkyStylise s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 3; ++i) {
        s.shadow_tint[i]    = 1.0f;
        s.mid_tint[i]       = 1.0f;
        s.highlight_tint[i] = 1.0f;
    }
    s.bands        = 0u;
    s.rim_strength = 0.0f;
    s.saturation   = 1.0f;
    return s;
}

bool jce_sky_stylise_is_identity(const JceSkyStylise *s)
{
    if (!s) return true;
    for (int i = 0; i < 3; ++i) {
        if (s->shadow_tint[i]    != 1.0f) return false;
        if (s->mid_tint[i]       != 1.0f) return false;
        if (s->highlight_tint[i] != 1.0f) return false;
    }
    /* One band is a single flat colour -- not a stylisation, a bug that looks
     * like one.  Treated as off, so it cannot ship by accident. */
    if (s->bands > 1u)            return false;
    if (s->rim_strength != 0.0f)  return false;
    if (s->saturation   != 1.0f)  return false;
    return true;
}

void jce_sky_stylise_apply(const JceSkyStylise *s, const float in_rgb[3],
                           float rim, float out_rgb[3])
{
    if (!in_rgb || !out_rgb) return;

    /* The identity path COPIES.
     *
     * Note what is NOT the reason: the graded arithmetic below happens to be
     * exact at identity, because every branch is either skipped or multiplies
     * by one.  The reason is the FINAL CLAMP -- it drops negatives and
     * non-finite values to zero, which is right for a graded result and wrong
     * for a pass-through.  Without the copy, "identity" would silently rewrite
     * any out-of-range radiance the scattering core produced, and the sky would
     * change depending on whether a disabled feature was compiled in. */
    /* NULL is checked HERE, on its own, and not left to the identity fast path
     * below.  That path returns true for NULL, so it happens to cover this --
     * but a guard that exists as a side effect of an optimisation disappears
     * the moment the optimisation is reordered or removed, and what it was
     * guarding is a null dereference. */
    if (!s || jce_sky_stylise_is_identity(s)) {
        out_rgb[0] = in_rgb[0];
        out_rgb[1] = in_rgb[1];
        out_rgb[2] = in_rgb[2];
        return;
    }

    float c[3] = { in_rgb[0], in_rgb[1], in_rgb[2] };
    const float l = sky_luma(c);

    /* ── Palette remap ──────────────────────────────────────────────────
     * Three-point: shadow to mid over the lower half of the range, mid to
     * highlight over the upper.  Radiance is unbounded, so the position is
     * taken from a tone-mapped proxy l/(1+l) rather than from l directly --
     * otherwise a bright sun disc pins every sky pixel to the highlight tint
     * and the palette collapses to one colour. */
    {
        const float t = l / (1.0f + l);
        float tint[3];
        if (t < 0.5f) {
            const float u = t * 2.0f;
            for (int i = 0; i < 3; ++i)
                tint[i] = s->shadow_tint[i] + (s->mid_tint[i] - s->shadow_tint[i]) * u;
        } else {
            const float u = (t - 0.5f) * 2.0f;
            for (int i = 0; i < 3; ++i)
                tint[i] = s->mid_tint[i] + (s->highlight_tint[i] - s->mid_tint[i]) * u;
        }
        for (int i = 0; i < 3; ++i) c[i] *= tint[i];
    }

    /* ── Posterisation ──────────────────────────────────────────────────
     * Quantise LUMINANCE and rescale the colour, rather than quantising each
     * channel.  Per-channel banding shifts hue at every step, so a smooth blue
     * gradient bands into blue-then-cyan-then-green -- which looks like a
     * colour bug, not like posterisation. */
    if (s->bands > 1u) {
        const float lum = sky_luma(c);
        if (lum > 1e-6f) {
            const float t = lum / (1.0f + lum);
            const float n = (float)s->bands;
            const float q = floorf(t * n) / n + (0.5f / n);
            const float target = q / (1.0f - ((q < 0.999f) ? q : 0.999f));
            const float k = target / lum;
            for (int i = 0; i < 3; ++i) c[i] *= k;
        }
    }

    /* ── Rim emphasis ───────────────────────────────────────────────────
     * What makes stylised clouds read as SHAPES rather than as fog. */
    if (s->rim_strength != 0.0f && rim > 0.0f) {
        const float r = (rim < 1.0f) ? rim : 1.0f;
        for (int i = 0; i < 3; ++i) c[i] *= 1.0f + s->rim_strength * r;
    }

    /* ── Saturation ─────────────────────────────────────────────────────
     * Around the CURRENT luminance, not the original: the tint and the bands
     * have already moved it, and pulling back toward a stale luminance would
     * partly undo them. */
    if (s->saturation != 1.0f) {
        const float lc = sky_luma(c);
        for (int i = 0; i < 3; ++i)
            c[i] = lc + (c[i] - lc) * s->saturation;
    }

    for (int i = 0; i < 3; ++i)
        out_rgb[i] = (c[i] > 0.0f && isfinite(c[i])) ? c[i] : 0.0f;
}
