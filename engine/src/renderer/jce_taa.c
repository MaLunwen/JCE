/*
 * jce_taa.c  Halton jitter + projection patcher.
 *
 * Foundation layer for full TAA (history + reproject deferred to P2).
 */

#include <jce/renderer/jce_taa.h>

#include <stdlib.h>

#define HALTON_COUNT 8u  /* power-of-two for cheap mask */

float jce_taa_halton(uint32_t i, uint32_t base)
{
    if (base < 2) return 0.0f;
    float f = 1.0f;
    float r = 0.0f;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

void jce_taa_advance(JceTaaState *s, uint32_t w, uint32_t h)
{
    if (!s) return;

    s->previous_jitter[0] = s->current_jitter[0];
    s->previous_jitter[1] = s->current_jitter[1];

    /* JCE_TAA_JITTER_PHASE=N pins the Halton index instead of advancing it.
     *
     * The sequence below is deterministic in the frame index, but the frame
     * index at any wall-clock moment is not: the editor spends a variable
     * number of frames loading, so two runs of one build reach the capture
     * frame at different points in the 8-long sequence. That makes an A/B
     * screenshot comparison bimodal -- the two runs either share a phase and
     * agree exactly, or they differ by a sub-pixel shift over 3.26% of
     * pixels with a max delta of 500. Measured on the differing pixels, mean
     * local gradient is 45.67 against 9.41 on the identical ones: the
     * difference sits only on edges, which is what a sub-pixel shift does and
     * what a geometry or lighting change does not.
     *
     * A bimodal floor is worse for a gate than a loud one, because sampling
     * it once passes about two times in three -- and a comparison certified
     * that way already sent one correct change to the bin as a regression.
     * Pinning the phase removes the variable from the comparison. It costs
     * anti-aliasing quality, so it is for measurement only; tools/visual_diff
     * sets it. Anything outside 0..7 is ignored. */
    if (!s->phase_pin_read) {
        const char *pv = getenv("JCE_TAA_JITTER_PHASE");
        long pin = (pv && pv[0]) ? strtol(pv, NULL, 10) : -1;
        s->phase_pin = (pin >= 0 && pin < (long)HALTON_COUNT)
                           ? (int32_t)pin : -1;
        s->phase_pin_read = true;
    }
    if (s->phase_pin >= 0)
        s->frame_index = (uint32_t)s->phase_pin;

    /* Halton index is 1-based; wrap on a small power-of-two so the
       sequence repeats deterministically without long history. */
    uint32_t idx = (s->frame_index & (HALTON_COUNT - 1)) + 1;
    float hx = jce_taa_halton(idx, 2) - 0.5f;   /* [-0.5, +0.5) */
    float hy = jce_taa_halton(idx, 3) - 0.5f;

    /* NDC offset: 1 pixel → 2 / dimension in clip space. */
    float jx = (w > 0) ? (hx * 2.0f / (float)w) : 0.0f;
    float jy = (h > 0) ? (hy * 2.0f / (float)h) : 0.0f;
    s->current_jitter[0] = jx;
    s->current_jitter[1] = jy;
    s->frame_index++;
}

void jce_taa_apply_jitter(jce_mat4 *proj, const float jitter[2])
{
    if (!proj || !jitter) return;
    /* Column-major: proj->m[col][row].  Sub-pixel translate in clip space
       is done by adding to the w-row of x and y columns, i.e. m[2][0/1]
       (post-multiply by the perspective matrix yields the same offset
       in NDC after divide-by-w).  This matches the bgfx convention. */
    float *m = JCE_M4_PTR(*proj);
    /* Layout: m[col*4 + row]. (2,0) = m[8],  (2,1) = m[9]. */
    m[8] += jitter[0];
    m[9] += jitter[1];
}

void jce_taa_record_camera(JceTaaState *s,
                           const jce_mat4 *view,
                           const jce_mat4 *proj)
{
    if (!s || !view || !proj) return;
    s->prev_view  = *view;
    s->prev_proj  = *proj;
    s->prev_valid = true;
}
