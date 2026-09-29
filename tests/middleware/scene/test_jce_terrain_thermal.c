/*
 * test_jce_terrain_thermal.c
 *
 * Thermal / talus erosion: material above the angle of repose slides downhill.
 *
 * This one has a HARD invariant the gully filter does not -- it MOVES material
 * rather than inventing it, so total height is conserved.  That makes it
 * genuinely testable rather than merely plausible-checkable, and conservation
 * is the first thing asserted here: a pass that quietly adds or destroys mass
 * is wrong however good it looks.
 */

#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW    33
#define TH    33
#define WORLD 32.0f      /* cell size exactly 1 m */
#define MAXH  20.0f

static double total_height(const JceTerrain *t)
{
    const float *h = jce_terrain_heights(t);
    double s = 0.0;
    for (int i = 0; i < TW * TH; i++) s += h[i];
    return s;
}

/* A single tall spire on flat ground: the steepest possible thing, and the
 * clearest demonstration of talus settling. */
static JceTerrain *make_spire(void)
{
    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    if (!t) return NULL;
    float *h = (float *)jce_terrain_heights(t);
    for (int i = 0; i < TW * TH; i++) h[i] = 0.0f;
    h[(TH / 2) * TW + (TW / 2)] = 0.9f;
    return t;
}

/* ── 1. Material is conserved ──────────────────────────────────────────  */

static void test_mass_is_conserved(void)
{
    JceTerrain *t = make_spire();
    TEST_ASSERT_NOT_NULL(t);
    const double before = total_height(t);

    JceTerrainThermalParams p;
    memset(&p, 0, sizeof p);
    p.iterations = 40;
    TEST_ASSERT_TRUE(jce_terrain_apply_thermal(t, &p));

    const double after = total_height(t);
    /* Float accumulation over 33*33 cells and 40 passes; anything larger than
     * this is real mass appearing or vanishing, not rounding. */
    TEST_ASSERT_TRUE(fabs(after - before) < 1e-3);

    jce_terrain_free(t);
}

/* ── 2. The spire actually settles, and spreads ────────────────────────  */

static void test_spire_settles_and_spreads(void)
{
    JceTerrain *t = make_spire();
    const int    c = (TH / 2) * TW + (TW / 2);
    const float  peak_before = jce_terrain_heights(t)[c];

    JceTerrainThermalParams p;
    memset(&p, 0, sizeof p);
    p.iterations = 40;
    jce_terrain_apply_thermal(t, &p);

    const float *h = jce_terrain_heights(t);
    /* The peak came down... */
    TEST_ASSERT_TRUE(h[c] < peak_before);
    /* ...and the material went somewhere adjacent rather than evaporating. */
    TEST_ASSERT_TRUE(h[c + 1] > 1e-4f);
    TEST_ASSERT_TRUE(h[c - 1] > 1e-4f);
    TEST_ASSERT_TRUE(h[c + TW] > 1e-4f);

    jce_terrain_free(t);
}

/* ── 3. No slope steeper than the repose angle survives ────────────────
 *
 * This is the definition of the algorithm, so it is the assertion that matters
 * most.  With enough iterations every remaining neighbour difference must sit
 * at or below the talus threshold. */

static void test_no_slope_exceeds_repose(void)
{
    JceTerrain *t = make_spire();
    JceTerrainThermalParams p;
    memset(&p, 0, sizeof p);
    p.talus_angle_deg = 35.0f;
    p.iterations      = 200;
    p.strength        = 0.5f;
    jce_terrain_apply_thermal(t, &p);

    const float *h = jce_terrain_heights(t);
    const float cell = WORLD / (float)(TW - 1);            /* 1 m */
    const float talus = tanf(35.0f * 3.14159265358979f / 180.0f) * cell / MAXH;

    float worst = 0.0f;
    for (int z = 0; z < TH; z++) {
        for (int x = 0; x < TW - 1; x++) {
            const float d = fabsf(h[z * TW + x] - h[z * TW + x + 1]);
            if (d > worst) worst = d;
        }
    }
    /* A little over the threshold is expected -- the pass moves a FRACTION of
     * the excess each time and converges asymptotically -- but it must be
     * close, not merely finite. */
    TEST_ASSERT_TRUE(worst <= talus * 1.15f);

    jce_terrain_free(t);
}

/* ── 4. A shallower repose angle settles further ───────────────────────
 *
 * Wet sand slumps flatter than dry rock.  If the angle were ignored, every
 * setting would produce the same pile. */

static void test_shallower_angle_settles_flatter(void)
{
    float peak[2];
    const float angles[2] = { 20.0f, 60.0f };
    for (int i = 0; i < 2; i++) {
        JceTerrain *t = make_spire();
        JceTerrainThermalParams p;
        memset(&p, 0, sizeof p);
        p.talus_angle_deg = angles[i];
        p.iterations      = 120;
        jce_terrain_apply_thermal(t, &p);
        peak[i] = jce_terrain_heights(t)[(TH / 2) * TW + (TW / 2)];
        jce_terrain_free(t);
    }
    /* 20 degrees must leave a flatter, lower pile than 60. */
    TEST_ASSERT_TRUE(peak[0] < peak[1]);
}

/* ── 5. Already-stable terrain is left alone ───────────────────────────
 *
 * Flat ground has no slope to relax, so the pass must be a no-op.  A version
 * that drifted here would erode a plain into a bowl over repeated cooks. */

static void test_flat_terrain_is_untouched(void)
{
    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    float *h = (float *)jce_terrain_heights(t);
    for (int i = 0; i < TW * TH; i++) h[i] = 0.4f;

    JceTerrainThermalParams p;
    memset(&p, 0, sizeof p);
    p.iterations = 30;
    jce_terrain_apply_thermal(t, &p);

    for (int i = 0; i < TW * TH; i++)
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, h[i]);

    jce_terrain_free(t);
}

/* ── 6. Converged output is stable under further passes ────────────────
 *
 * An unstable kernel oscillates: run it again and the surface keeps changing.
 * Running to convergence and then running MORE must barely move anything. */

static void test_converged_result_is_stable(void)
{
    JceTerrain *t = make_spire();
    JceTerrainThermalParams p;
    memset(&p, 0, sizeof p);
    p.iterations = 200;
    jce_terrain_apply_thermal(t, &p);

    float settled[TW * TH];
    memcpy(settled, jce_terrain_heights(t), sizeof settled);

    p.iterations = 50;
    jce_terrain_apply_thermal(t, &p);

    const float *h = jce_terrain_heights(t);
    double drift = 0.0;
    for (int i = 0; i < TW * TH; i++) drift += fabs((double)h[i] - settled[i]);
    TEST_ASSERT_TRUE(drift < 1e-3);

    jce_terrain_free(t);
}

/* ── 7. Defaults and refusal ───────────────────────────────────────────  */

static void test_defaults_and_refusal(void)
{
    TEST_ASSERT_FALSE(jce_terrain_apply_thermal(NULL, NULL));

    /* A zeroed struct -- and a NULL pointer -- must both mean "settle it with
     * sensible defaults", not "do nothing". */
    JceTerrain *a = make_spire();
    const float peak = jce_terrain_heights(a)[(TH / 2) * TW + (TW / 2)];
    JceTerrainThermalParams zero;
    memset(&zero, 0, sizeof zero);
    TEST_ASSERT_TRUE(jce_terrain_apply_thermal(a, &zero));
    TEST_ASSERT_TRUE(jce_terrain_heights(a)[(TH / 2) * TW + (TW / 2)] < peak);
    jce_terrain_free(a);

    JceTerrain *b = make_spire();
    TEST_ASSERT_TRUE(jce_terrain_apply_thermal(b, NULL));
    TEST_ASSERT_TRUE(jce_terrain_heights(b)[(TH / 2) * TW + (TW / 2)] < peak);
    jce_terrain_free(b);

    JceTerrain *proc = jce_terrain_create_procedural(TW, TH, WORLD, WORLD,
                                                     MAXH, 8, 4, 1u, 0.05f);
    if (proc) {
        if (!jce_terrain_heights(proc))
            TEST_ASSERT_FALSE(jce_terrain_apply_thermal(proc, NULL));
        jce_terrain_free(proc);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mass_is_conserved);
    RUN_TEST(test_spire_settles_and_spreads);
    RUN_TEST(test_no_slope_exceeds_repose);
    RUN_TEST(test_shallower_angle_settles_flatter);
    RUN_TEST(test_flat_terrain_is_untouched);
    RUN_TEST(test_converged_result_is_stable);
    RUN_TEST(test_defaults_and_refusal);
    return UNITY_END();
}
