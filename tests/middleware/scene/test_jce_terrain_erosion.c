/*
 * test_jce_terrain_erosion.c
 *
 * Fast Gully Erosion applied over a whole height grid, plus the ridge channel.
 *
 * What can honestly be asserted about a procedural filter is not "it looks
 * right" -- it is that it CHANGED something, changed it in the right direction,
 * stayed inside the format's range, and is reproducible.  The interesting
 * failure modes are all of that kind: a zeroed parameter struct silently
 * eroding nothing, in-place feedback biasing the result, or a value escaping
 * [0,1] and wrapping through the 16-bit cooked format.
 */

#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW    65
#define TH    65
#define WORLD 128.0f
#define MAXH  60.0f

/* A smooth hill: a real slope for gullies to run down, and no pre-existing
 * high-frequency detail that could be mistaken for the filter's output. */
static JceTerrain *make_hill(void)
{
    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    if (!t) return NULL;
    float *h = (float *)jce_terrain_heights(t);
    if (!h) { jce_terrain_free(t); return NULL; }
    for (int z = 0; z < TH; z++) {
        for (int x = 0; x < TW; x++) {
            const float u = (float)x / (float)(TW - 1) - 0.5f;
            const float v = (float)z / (float)(TH - 1) - 0.5f;
            const float d = sqrtf(u * u + v * v);
            h[z * TW + x] = 0.85f * expf(-d * d * 6.0f);
        }
    }
    return t;
}

static void snapshot(const JceTerrain *t, float *dst)
{
    memcpy(dst, jce_terrain_heights(t), sizeof(float) * TW * TH);
}

/* ── 1. It actually erodes, and a zeroed param struct still does ───────
 *
 * Every field defaults when left at 0, so a zeroed struct must be a valid
 * "erode it sensibly" request.  If 0 were taken literally as zero octaves the
 * call would succeed and change nothing -- indistinguishable from never having
 * been made. */

static void test_zeroed_params_still_erode(void)
{
    JceTerrain *t = make_hill();
    TEST_ASSERT_NOT_NULL(t);

    float before[TW * TH];
    snapshot(t, before);

    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    p.seed = 1234u;
    TEST_ASSERT_TRUE(jce_terrain_apply_erosion(t, &p, NULL));

    const float *after = jce_terrain_heights(t);
    double diff = 0.0;
    for (int i = 0; i < TW * TH; i++) diff += fabs((double)after[i] - before[i]);
    TEST_ASSERT_TRUE(diff > 1e-3);

    jce_terrain_free(t);
}

/* ── 2. Output stays inside the storage range ──────────────────────────
 *
 * Heights are stored NORMALIZED and cooked to 16-bit unorm.  A value that
 * escaped [0,1] would wrap, turning a gully floor into a spike. */

static void test_output_stays_in_range(void)
{
    JceTerrain *t = make_hill();
    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    p.seed     = 7u;
    p.strength = 40.0f;            /* deliberately violent */
    p.octaves  = 6;
    TEST_ASSERT_TRUE(jce_terrain_apply_erosion(t, &p, NULL));

    const float *h = jce_terrain_heights(t);
    for (int i = 0; i < TW * TH; i++) {
        TEST_ASSERT_FALSE(isnan(h[i]));
        TEST_ASSERT_TRUE(h[i] >= 0.0f);
        TEST_ASSERT_TRUE(h[i] <= 1.0f);
    }
    jce_terrain_free(t);
}

/* ── 3. Deterministic in the seed, and the seed matters ────────────────  */

static void test_deterministic_and_seed_sensitive(void)
{
    float a[TW * TH], b[TW * TH], c[TW * TH];

    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    p.seed = 99u;

    JceTerrain *t1 = make_hill();
    jce_terrain_apply_erosion(t1, &p, NULL);
    snapshot(t1, a);
    jce_terrain_free(t1);

    JceTerrain *t2 = make_hill();
    jce_terrain_apply_erosion(t2, &p, NULL);
    snapshot(t2, b);
    jce_terrain_free(t2);

    /* Same seed, same input: bit-identical, or a cook is not reproducible. */
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(float) * TW * TH);

    p.seed = 100u;
    JceTerrain *t3 = make_hill();
    jce_terrain_apply_erosion(t3, &p, NULL);
    snapshot(t3, c);
    jce_terrain_free(t3);

    double d = 0.0;
    for (int i = 0; i < TW * TH; i++) d += fabs((double)c[i] - a[i]);
    TEST_ASSERT_TRUE(d > 1e-4);    /* the seed is not ignored */
}

/* ── 4. The ridge channel is a real mask ───────────────────────────────
 *
 * It is meant to replace an author's hand-painted splat/density mask, so it has
 * to span both signs and correlate with the surface -- a constant would satisfy
 * "in range" and be useless. */

static void test_ridge_channel_is_signed_and_varied(void)
{
    JceTerrain *t = make_hill();
    float *ridge = (float *)malloc(sizeof(float) * TW * TH);
    TEST_ASSERT_NOT_NULL(ridge);
    for (int i = 0; i < TW * TH; i++) ridge[i] = -999.0f;   /* poison */

    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    p.seed = 5u;
    TEST_ASSERT_TRUE(jce_terrain_apply_erosion(t, &p, ridge));

    int neg = 0, pos = 0;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < TW * TH; i++) {
        TEST_ASSERT_FALSE(isnan(ridge[i]));
        TEST_ASSERT_TRUE(ridge[i] >= -1.0f && ridge[i] <= 1.0f);
        if (ridge[i] < -1e-4f) neg++;
        if (ridge[i] >  1e-4f) pos++;
        if (ridge[i] < lo) lo = ridge[i];
        if (ridge[i] > hi) hi = ridge[i];
    }
    /* Every cell was written (the poison is out of range and would fail the
     * bounds check above), and both creases and ridges exist. */
    TEST_ASSERT_TRUE(neg > 0);
    TEST_ASSERT_TRUE(pos > 0);
    TEST_ASSERT_TRUE(hi - lo > 0.05f);

    free(ridge);
    jce_terrain_free(t);
}

/* ── 5. Stronger settings erode more ───────────────────────────────────  */

static void test_strength_is_monotonic(void)
{
    float base[TW * TH];
    JceTerrain *t0 = make_hill();
    snapshot(t0, base);
    jce_terrain_free(t0);

    double prev = -1.0;
    const float strengths[3] = { 2.0f, 8.0f, 24.0f };
    for (int k = 0; k < 3; k++) {
        JceTerrain *t = make_hill();
        JceTerrainErosionParams p;
        memset(&p, 0, sizeof p);
        p.seed     = 3u;
        p.strength = strengths[k];
        jce_terrain_apply_erosion(t, &p, NULL);

        const float *h = jce_terrain_heights(t);
        double d = 0.0;
        for (int i = 0; i < TW * TH; i++) d += fabs((double)h[i] - base[i]);
        TEST_ASSERT_TRUE(d > prev);
        prev = d;
        jce_terrain_free(t);
    }
}

/* ── 6. No scan-order feedback: the filter reads the ORIGINAL field ────
 *
 * The gradient handed to the filter must come from a SNAPSHOT of the input.
 * Reading the buffer being written instead feeds already-carved neighbours
 * into later gradients, which biases the whole field along the traversal
 * direction and reads as combing.
 *
 * That mistake survives every other test here -- the result stays
 * deterministic, in range and seed-sensitive.  What distinguishes it is
 * LOCALITY: perturb one sample near the START of the scan and, with a
 * snapshot, only its immediate neighbours can change; with feedback the
 * perturbation propagates down the row into samples far away. */

static void test_no_scan_order_feedback(void)
{
    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    p.seed     = 11u;
    /* Moderate: a violent strength drives the poked sample onto the [0,1]
     * clamp in BOTH runs, erasing the very difference that is supposed to
     * propagate -- the first version of this test was silently vacuous for
     * exactly that reason. */
    p.strength = 4.0f;

    float plain[TW * TH], poked[TW * TH];

    JceTerrain *a = make_hill();
    jce_terrain_apply_erosion(a, &p, NULL);
    snapshot(a, plain);
    jce_terrain_free(a);

    /* Poke a single sample at the very start of the traversal.  UPWARD, and
     * well inside the range, so neither run can clamp it away. */
    JceTerrain *b = make_hill();
    float *hb = (float *)jce_terrain_heights(b);
    hb[1 * TW + 1] = 0.60f;
    jce_terrain_apply_erosion(b, &p, NULL);
    snapshot(b, poked);
    jce_terrain_free(b);

    /* Anything more than 2 cells away from the poke must be untouched: the
     * filter's own stencil is the 4 immediate neighbours, so a difference
     * further out can only have arrived by propagation. */
    int far_changed = 0;
    for (int z = 0; z < TH; z++) {
        for (int x = 0; x < TW; x++) {
            const int dx = x - 1, dz = z - 1;
            if (dx * dx + dz * dz <= 8) continue;    /* near the poke */
            if (fabsf(poked[z * TW + x] - plain[z * TW + x]) > 1e-6f)
                far_changed++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, far_changed);

    /* And the poke must have changed something nearby, or the test is vacuous
     * because the perturbation never mattered at all. */
    double near_diff = 0.0;
    for (int z = 0; z < 3; z++)
        for (int x = 0; x < 3; x++)
            near_diff += fabs((double)poked[z * TW + x] - plain[z * TW + x]);
    TEST_ASSERT_TRUE(near_diff > 1e-6);
}

/* ── 6. Refuses what it cannot do ──────────────────────────────────────  */

static void test_refuses_degenerate(void)
{
    JceTerrainErosionParams p;
    memset(&p, 0, sizeof p);
    TEST_ASSERT_FALSE(jce_terrain_apply_erosion(NULL, &p, NULL));

    /* A NULL params pointer is a valid "use the defaults" request, not an
     * error -- matching jce_phacelle_erode's own contract. */
    JceTerrain *t = make_hill();
    TEST_ASSERT_TRUE(jce_terrain_apply_erosion(t, NULL, NULL));
    jce_terrain_free(t);

    /* Procedural terrain has no resident grid; refusing beats reporting
     * success while eroding nothing. */
    JceTerrain *proc = jce_terrain_create_procedural(TW, TH, WORLD, WORLD,
                                                     MAXH, 8, 4, 42u, 0.05f);
    if (proc) {
        if (!jce_terrain_heights(proc))
            TEST_ASSERT_FALSE(jce_terrain_apply_erosion(proc, &p, NULL));
        jce_terrain_free(proc);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zeroed_params_still_erode);
    RUN_TEST(test_output_stays_in_range);
    RUN_TEST(test_deterministic_and_seed_sensitive);
    RUN_TEST(test_ridge_channel_is_signed_and_varied);
    RUN_TEST(test_strength_is_monotonic);
    RUN_TEST(test_no_scan_order_feedback);
    RUN_TEST(test_refuses_degenerate);
    return UNITY_END();
}
