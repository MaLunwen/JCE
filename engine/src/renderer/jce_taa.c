/*
 * jce_taa.c  Halton jitter + projection patcher.
 *
 * Foundation layer for full TAA (history + reproject deferred to P2).
 */

#include <jce/renderer/jce_taa.h>

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
