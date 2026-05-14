/*
 * jce_taa_jitter.c  Halton + sub-pixel jitter math.
 *
 * Pure functions; no engine state.  Halton sequence is the
 * iterative van der Corput construction — fast, no allocations.
 */

#include <jce/renderer/jce_taa_jitter.h>

float jce_taa_halton(uint32_t i, uint32_t b)
{
    if (b < 2u) b = 2u;
    float r = 0.0f;
    float f = 1.0f;
    while (i > 0u) {
        f /= (float)b;
        r += f * (float)(i % b);
        i /= b;
    }
    return r;
}

void jce_taa_jitter_offset(uint32_t frame_index, uint32_t seq_len,
                            uint32_t w, uint32_t h,
                            float *out_x, float *out_y)
{
    if (!out_x || !out_y) return;
    if (seq_len == 0u) seq_len = 8u;
    if (w == 0u) w = 1u;
    if (h == 0u) h = 1u;

    /* Halton(2,3) shifted to [-0.5, 0.5] sub-pixels. */
    uint32_t idx = (frame_index % seq_len) + 1u;  /* skip i=0 to avoid (0,0) */
    float px = jce_taa_halton(idx, 2u) - 0.5f;    /* [-0.5, 0.5] pixels */
    float py = jce_taa_halton(idx, 3u) - 0.5f;

    /* Convert to NDC: a 1-pixel shift in screen space = 2.0/dim in NDC
     * (NDC spans [-1, 1]).  So a 0.5-pixel max ⇒ 1.0/dim NDC max. */
    *out_x = px * (2.0f / (float)w);
    *out_y = py * (2.0f / (float)h);
}

/* Column-major projection matrix layout (matches `jce_mat4`):
 *   m[0][0] m[1][0] m[2][0] m[3][0]
 *   m[0][1] m[1][1] m[2][1] m[3][1]
 *   m[0][2] m[1][2] m[2][2] m[3][2]
 *   m[0][3] m[1][3] m[2][3] m[3][3]
 *
 * Standard perspective stores translation in column 2 (z-row); the
 * jitter shifts the x/y translation entries to produce the desired
 * NDC offset after divide.  This matches the form used by Unity's
 * Camera.SetGateFitJitter.
 */
void jce_taa_jitter_projection(jce_mat4 *p, float dx, float dy)
{
    if (!p) return;
    /* Bake into row 0/1, column 2 (the "tx/ty added to clip.xy after
     * the divide" entries that ALL perspective matrices carry as 0
     * by default).  This avoids touching the focal-length elements. */
    p->raw[2][0] += dx;
    p->raw[2][1] += dy;
}

void jce_taa_unjitter_projection(jce_mat4 *p, float dx, float dy)
{
    if (!p) return;
    p->raw[2][0] -= dx;
    p->raw[2][1] -= dy;
}
