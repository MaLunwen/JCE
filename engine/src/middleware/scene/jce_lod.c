/*
 * jce_lod.c  Implementation of distance-based LOD selection.
 *
 * Layer: Scene (Layer 5).
 */

#include <jce/middleware/scene/jce_lod.h>

#include <float.h>
#include <string.h>

void jce_lod_init(JceLodGroup *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->hysteresis = 0.05f;
}

void jce_lod_setup(JceLodGroup *g,
                   JceMesh *high, float d_high,
                   JceMesh *mid,  float d_mid,
                   JceMesh *low,  float d_low)
{
    if (!g) return;
    jce_lod_init(g);

    int idx = 0;
    if (high) { g->levels[idx].mesh = high; g->levels[idx].distance = d_high; idx++; }
    if (mid)  { g->levels[idx].mesh = mid;  g->levels[idx].distance = d_mid;  idx++; }
    if (low)  { g->levels[idx].mesh = low;  g->levels[idx].distance = d_low;  idx++; }
    /* Guarantee the last level extends to infinity unless caller wants
     * an explicit cull distance (then they'd pass FLT_MAX themselves). */
    if (idx > 0) g->levels[idx - 1].distance = FLT_MAX;
    g->count = idx;
}

/* 1.0 = neutral.  See the header for what this replaced. */
static float s_lod_global_bias = 1.0f;

void jce_lod_set_global_bias(float bias)
{
    if (!(bias > 0.0f) || bias != bias)   /* rejects 0, negatives and NaN */
        return;
    if (bias < 0.01f)  bias = 0.01f;
    if (bias > 100.0f) bias = 100.0f;
    s_lod_global_bias = bias;
}

float jce_lod_get_global_bias(void)
{
    return s_lod_global_bias;
}

int jce_lod_pick(const JceLodGroup *g, float distance, int prev_level)
{
    if (!g || g->count <= 0) return -1;

    /* Dividing the distance is how a >1 bias keeps higher detail out to
     * longer ranges, which is what the slider says it does.  Applied here so
     * every caller inherits it -- the per-entity and global scene-renderer
     * paths and the runtime's -- rather than at three call sites that would
     * drift apart. */
    if (s_lod_global_bias != 1.0f)
        distance /= s_lod_global_bias;

    const float h = (g->hysteresis > 0.0f && g->hysteresis < 0.5f)
                      ? g->hysteresis : 0.05f;

    /* No prior level → nominal selection (no hysteresis). */
    if (prev_level < 0 || prev_level >= g->count) {
        for (int i = 0; i < g->count; i++) {
            if (distance <= g->levels[i].distance) return i;
        }
        return -1;  /* past last threshold → cull */
    }

    const int p = prev_level;

    /* Try to step DOWN to a lower-detail level (away from camera).
     * Only advance once per call; the next frame will keep stepping if
     * the camera is still moving away. This avoids skipping intermediate
     * levels in a single frame, which would defeat hysteresis. */
    if (p < g->count - 1) {
        const float thr      = g->levels[p].distance;
        const float thr_high = thr * (1.0f + h);
        if (distance > thr_high) return p + 1;
    } else {
        /* Already at last level; check cull. */
        const float thr      = g->levels[p].distance;
        const float thr_high = thr * (1.0f + h);
        if (thr < FLT_MAX * 0.5f && distance > thr_high) return -1;
    }

    /* Try to step UP to a higher-detail level (toward camera). */
    if (p > 0) {
        const float thr_prev = g->levels[p - 1].distance;
        const float thr_low  = thr_prev * (1.0f - h);
        if (distance < thr_low) return p - 1;
    }

    /* Inside dead-zone: keep previous level. */
    return p;
}
