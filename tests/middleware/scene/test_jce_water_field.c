/*
 * test_jce_water_field.c
 *
 * The field exists to make one specific bug unrepresentable: the renderer and
 * the physics solver simulating the same water on two unreconciled clocks.  So
 * these tests are mostly about AGREEMENT and IDEMPOTENCE, not about wave shape
 * -- the shape is already covered by test_jce_water and test_jce_water_fft.
 */

#include "jce_water_field_internal.h"
#include "jce_water_nearshore_config.h"
#include "jce_sr_water_mesh.h"
#include "jce_water_shore_atlas.h"
#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_water_fft.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static JceWaterWave g_waves[3];

static void waves_init(void)
{
    g_waves[0].amplitude = 0.60f; g_waves[0].wavelength = 24.0f;
    g_waves[0].speed = 3.0f; g_waves[0].dir_x = 1.0f; g_waves[0].dir_z = 0.0f;
    g_waves[0].steepness = 0.5f;

    g_waves[1].amplitude = 0.25f; g_waves[1].wavelength =  9.0f;
    g_waves[1].speed = 2.0f; g_waves[1].dir_x = 0.4f; g_waves[1].dir_z = 0.9f;
    g_waves[1].steepness = 0.4f;

    /* Deliberately tiny: the band limit must be able to exclude this one. */
    g_waves[2].amplitude = 0.05f; g_waves[2].wavelength =  1.2f;
    g_waves[2].speed = 1.0f; g_waves[2].dir_x = -0.7f; g_waves[2].dir_z = 0.7f;
    g_waves[2].steepness = 0.3f;
}

static JceWaterFieldDesc desc_gerstner(void)
{
    JceWaterFieldDesc d;
    memset(&d, 0, sizeof(d));
    d.model       = JCE_WATER_FIELD_GERSTNER;
    d.base_height = 2.0f;
    d.size_x = 200.0f; d.size_z = 200.0f;
    d.center_x = 0.0f; d.center_z = 0.0f;
    d.waves = g_waves; d.wave_count = 3;
    return d;
}

static JceWaterFieldDesc desc_fft(void)
{
    JceWaterFieldDesc d;
    memset(&d, 0, sizeof(d));
    d.model       = JCE_WATER_FIELD_FFT;
    d.base_height = 2.0f;
    d.size_x = 200.0f; d.size_z = 200.0f;
    d.fft_resolution = 32;
    d.fft_patch_size = 64.0f;
    d.fft_wind_speed = 9.0f;
    d.fft_wind_dir_x = 1.0f;
    d.fft_wind_dir_z = 0.0f;
    d.fft_amplitude  = 8e-4f;
    d.fft_seed       = 1337u;
    return d;
}

/* ── 1. A field is queryable the instant it exists ─────────────────────
 *
 * "Created but not yet evolved" would be a second way for a consumer to read
 * zeros that look exactly like a flat calm sea. */

static void test_queryable_immediately(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, jce_water_field_time(f));
    TEST_ASSERT_TRUE(jce_water_field_revision(f) > 0u);
    TEST_ASSERT_NOT_NULL(jce_water_field_fft(f));

    JceWaterSample s;
    TEST_ASSERT_TRUE(jce_water_field_sample(f, 3.0f, -4.0f, 0.0f, &s));
    TEST_ASSERT_FALSE(isnan(s.position.y));
    /* The normal must be unit length, or every lighting term downstream is
     * quietly scaled. */
    const float len = sqrtf(s.normal.x * s.normal.x + s.normal.y * s.normal.y +
                            s.normal.z * s.normal.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, len);
    jce_water_field_destroy(f);
}

/* ── 2. THE POINT: one clock, and setting it is idempotent ─────────────
 *
 * This is the property that replaces the old two-accumulator arrangement.  A
 * second consumer setting the same time in the same tick must cost nothing and
 * must not perturb the surface. */

static void test_clock_is_idempotent(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *f = jce_water_field_create(&d);

    jce_water_field_set_time(f, 4.25);
    const uint64_t rev = jce_water_field_revision(f);
    JceWaterSample a;
    jce_water_field_sample(f, 11.0f, 7.0f, 0.0f, &a);

    jce_water_field_set_time(f, 4.25);          /* the second consumer */
    TEST_ASSERT_EQUAL_UINT64(rev, jce_water_field_revision(f));

    JceWaterSample b;
    jce_water_field_sample(f, 11.0f, 7.0f, 0.0f, &b);
    TEST_ASSERT_EQUAL_FLOAT(a.position.y, b.position.y);

    /* A different time DOES advance the revision -- otherwise a consumer
     * caching on the revision would freeze. */
    jce_water_field_set_time(f, 4.5);
    TEST_ASSERT_TRUE(jce_water_field_revision(f) > rev);
    jce_water_field_destroy(f);
}

/* ── 3. Two fields on the same desc and clock agree bit-exactly ────────
 *
 * This is what buys the engine its structural advantage: the renderer's
 * uploaded texture and the physics query come from the same evaluated state,
 * with no readback, no latency and no bake.  If this ever fails, the whole
 * argument for keeping the FFT on the CPU collapses. */

static void test_same_desc_same_clock_is_bit_exact(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *a = jce_water_field_create(&d);
    JceWaterField *b = jce_water_field_create(&d);

    jce_water_field_set_time(a, 7.125);
    jce_water_field_set_time(b, 7.125);

    const int n = jce_water_fft_resolution(jce_water_field_fft(a));
    const float *ha = jce_water_fft_height_data(jce_water_field_fft(a));
    const float *hb = jce_water_fft_height_data(jce_water_field_fft(b));
    TEST_ASSERT_EQUAL_MEMORY(ha, hb, sizeof(float) * (size_t)(n * n));

    for (int i = 0; i < 8; i++) {
        const float x = (float)i * 3.7f - 12.0f;
        const float z = (float)i * -2.3f + 5.0f;
        JceWaterSample sa, sb;
        jce_water_field_sample(a, x, z, 0.0f, &sa);
        jce_water_field_sample(b, x, z, 0.0f, &sb);
        TEST_ASSERT_EQUAL_FLOAT(sa.position.y, sb.position.y);
    }
    jce_water_field_destroy(a);
    jce_water_field_destroy(b);
}

/* ── 4. Outside the body is refused, not invented ──────────────────────
 *
 * Inventing a surface off the edge is how a boat gets shoved upward by an
 * ocean it is nowhere near. */

static void test_render_fft_rim_reuses_the_buoyancy_surface(void)
{
    JceWaterFieldDesc d = desc_fft();
    d.size_x = 8.0f;
    d.size_z = 40.0f;
    d.cascade_fraction = 0.27f;
    JceWaterField *bounded = jce_water_field_create(&d);
    d.size_x = 0.0f;
    JceWaterField *unbounded = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(bounded);
    TEST_ASSERT_NOT_NULL(unbounded);
    TEST_ASSERT_NOT_NULL(jce_water_field_fft2(bounded));

    jce_water_field_set_time(bounded, 3.25);
    jce_water_field_set_time(unbounded, 3.25);
    const uint64_t revision = jce_water_field_revision(bounded);
    JceWaterSample expected, rendered;
    /* An ocean ring can extend past the narrow dimension of its rectangle. */
    TEST_ASSERT_FALSE(jce_water_field_sample(bounded, 14.0f, -3.0f, 0.0f, &rendered));
    TEST_ASSERT_TRUE(jce_water_field_sample(unbounded, 14.0f, -3.0f, 0.0f, &expected));
    TEST_ASSERT_TRUE(jce_water_field_sample_render(bounded, 14.0f, -3.0f, &rendered));
    TEST_ASSERT_EQUAL_FLOAT(14.0f, rendered.position.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.position.y, rendered.position.y);
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, rendered.position.z);
    TEST_ASSERT_EQUAL_FLOAT(expected.normal.x, rendered.normal.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.normal.y, rendered.normal.y);
    TEST_ASSERT_EQUAL_FLOAT(expected.normal.z, rendered.normal.z);
    TEST_ASSERT_EQUAL_FLOAT(expected.jacobian, rendered.jacobian);
    TEST_ASSERT_EQUAL_UINT64(revision, jce_water_field_revision(bounded));
    TEST_ASSERT_FALSE(jce_water_field_sample_render(bounded, NAN, -3.0f, &rendered));

    jce_water_field_destroy(unbounded);
    jce_water_field_destroy(bounded);
}

static void test_outside_the_body_is_refused(void)
{
    JceWaterFieldDesc d = desc_gerstner();
    JceWaterField *f = jce_water_field_create(&d);

    JceWaterSample s;
    TEST_ASSERT_TRUE (jce_water_field_sample(f,   50.0f,  50.0f, 0.0f, &s));
    TEST_ASSERT_FALSE(jce_water_field_sample(f, 5000.0f,   0.0f, 0.0f, &s));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.position.y);   /* zeroed, not stale */

    /* The convenience accessor falls back to the still plane rather than to 0,
     * so a caller that ignores the bool still gets a sane number. */
    TEST_ASSERT_EQUAL_FLOAT(2.0f,
        jce_water_field_surface_y(f, 5000.0f, 0.0f, 0.0f));

    /* A zero extent means "unbounded": an ocean authored without a plane size
     * must not refuse every query. */
    d.size_x = 0.0f; d.size_z = 0.0f;
    JceWaterField *ocean = jce_water_field_create(&d);
    TEST_ASSERT_TRUE(jce_water_field_sample(ocean, 5000.0f, 0.0f, 0.0f, &s));

    jce_water_field_destroy(ocean);
    jce_water_field_destroy(f);
}

/* ── 5. The band limit actually excludes short waves ───────────────────
 *
 * Feeding 20 cm ripples into a 40 m hull's rigid-body solver is both wasted
 * work and a stability hazard, so the exclusion must be real, not decorative. */

static void test_band_limit_excludes_short_waves(void)
{
    JceWaterFieldDesc d = desc_gerstner();
    JceWaterField *f = jce_water_field_create(&d);
    jce_water_field_set_time(f, 1.0);

    /* Full spectrum vs. a 40 m hull, which must drop the 1.2 m wave. */
    const float full = jce_water_field_surface_y(f, 6.0f, -3.0f,  0.0f);
    const float hull = jce_water_field_surface_y(f, 6.0f, -3.0f, 40.0f);
    TEST_ASSERT_TRUE(fabsf(full - hull) > 1e-4f);

    /* A limit below every wavelength must change nothing at all. */
    const float none = jce_water_field_surface_y(f, 6.0f, -3.0f, 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(full, none);
    jce_water_field_destroy(f);
}

/* ── 6. Sync: cheap params in place, spectrum params rebuild ───────────
 *
 * Rebuilding the ocean every time a designer nudges the plane's position would
 * make the water flicker under the cursor. */

static void test_sync_rebuilds_only_for_spectrum_changes(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *f = jce_water_field_create(&d);
    jce_water_field_set_time(f, 3.0);
    const uint64_t rev = jce_water_field_revision(f);

    d.base_height = 9.0f;            /* cheap: no spectrum involvement */
    d.center_x    = 40.0f;
    TEST_ASSERT_TRUE(jce_water_field_sync(f, &d));
    TEST_ASSERT_EQUAL_UINT64(rev, jce_water_field_revision(f));
    TEST_ASSERT_EQUAL_FLOAT(9.0f, jce_water_field_base_height(f));

    d.fft_wind_speed = 14.0f;        /* spectrum: must rebuild */
    TEST_ASSERT_TRUE(jce_water_field_sync(f, &d));
    TEST_ASSERT_TRUE(jce_water_field_revision(f) > rev);
    /* The clock survives a rebuild -- otherwise the sea would jump back to
     * t=0 whenever the wind changed. */
    TEST_ASSERT_EQUAL_DOUBLE(3.0, jce_water_field_time(f));
    jce_water_field_destroy(f);
}

/* ── 7. Switching model swaps the ocean, not just its phase ────────────  */

static void test_model_switch_rebuilds(void)
{
    JceWaterFieldDesc d = desc_gerstner();
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NULL(jce_water_field_fft(f));
    TEST_ASSERT_EQUAL_INT(JCE_WATER_FIELD_GERSTNER, jce_water_field_model(f));

    JceWaterFieldDesc fd = desc_fft();
    TEST_ASSERT_TRUE(jce_water_field_sync(f, &fd));
    TEST_ASSERT_EQUAL_INT(JCE_WATER_FIELD_FFT, jce_water_field_model(f));
    TEST_ASSERT_NOT_NULL(jce_water_field_fft(f));

    jce_water_field_destroy(f);
}

/* ── 8. Jacobian reports folds, which is what a whitecap IS ────────────  */

static void test_jacobian_detects_folding(void)
{
    JceWaterFieldDesc d = desc_gerstner();
    JceWaterField *calm = jce_water_field_create(&d);
    JceWaterSample s;
    jce_water_field_sample(calm, 1.0f, 1.0f, 0.0f, &s);
    TEST_ASSERT_TRUE(s.jacobian > 0.0f);      /* a well-formed wave */
    jce_water_field_destroy(calm);

    /* Crank steepness past the self-intersection threshold. */
    JceWaterWave steep[1];
    steep[0] = g_waves[0];
    steep[0].steepness = 1.0f;
    steep[0].amplitude = 6.0f;
    steep[0].wavelength = 8.0f;
    d.waves = steep; d.wave_count = 1;

    JceWaterField *breaking = jce_water_field_create(&d);
    jce_water_field_sample(breaking, 1.0f, 1.0f, 0.0f, &s);
    TEST_ASSERT_TRUE(s.jacobian < 0.0f);
    jce_water_field_destroy(breaking);
}

/* ── 9. NULL safety ────────────────────────────────────────────────────  */

static void test_null_safety(void)
{
    TEST_ASSERT_NULL(jce_water_field_create(NULL));
    jce_water_field_destroy(NULL);                  /* must not crash */
    jce_water_field_set_time(NULL, 1.0);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, jce_water_field_time(NULL));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_water_field_revision(NULL));
    TEST_ASSERT_NULL(jce_water_field_fft(NULL));
    TEST_ASSERT_FALSE(jce_water_field_sync(NULL, NULL));

    JceWaterSample s;
    TEST_ASSERT_FALSE(jce_water_field_sample(NULL, 0.0f, 0.0f, 0.0f, &s));
    TEST_ASSERT_FALSE(jce_water_field_sample(NULL, 0.0f, 0.0f, 0.0f, NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_field_surface_y(NULL, 0, 0, 0));
}


/* ── 10. The set: exactly one driver may advance the clock ─────────────
 *
 * This is the mechanism that replaces "please only call this from one place".
 * A comment would have been obeyed for about a week. */

static void test_only_one_driver_advances(void)
{
    JceWaterFieldSet *set = jce_water_field_set_create();
    TEST_ASSERT_NOT_NULL(set);

    int sim = 0, renderer = 0;   /* two distinct driver identities */

    TEST_ASSERT_TRUE (jce_water_field_set_advance(set, &sim, 0.5));
    TEST_ASSERT_EQUAL_DOUBLE(0.5, jce_water_field_set_get_time(set));

    /* The renderer offering to advance must be a NO-OP, not an addition. */
    TEST_ASSERT_FALSE(jce_water_field_set_advance(set, &renderer, 0.25));
    TEST_ASSERT_EQUAL_DOUBLE(0.5, jce_water_field_set_get_time(set));

    TEST_ASSERT_TRUE (jce_water_field_set_advance(set, &sim, 0.5));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, jce_water_field_set_get_time(set));

    /* Releasing hands the claim to whoever advances next -- this is what makes
     * leaving Play give the editor preview its clock back instead of freezing
     * the water forever. */
    jce_water_field_set_release(set, &sim);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &renderer, 0.25));
    TEST_ASSERT_EQUAL_DOUBLE(1.25, jce_water_field_set_get_time(set));

    /* Releasing a claim you do not hold must do nothing. */
    jce_water_field_set_release(set, &sim);
    TEST_ASSERT_FALSE(jce_water_field_set_advance(set, &sim, 1.0));
    TEST_ASSERT_EQUAL_DOUBLE(1.25, jce_water_field_set_get_time(set));

    jce_water_field_set_destroy(set);
}

/* ── 11. Fields in a set ride the set's clock ──────────────────────────
 *
 * Including one acquired LATE: a water body created mid-session must join the
 * ocean already in progress, not start its own at t=0. */

static void test_fields_ride_the_set_clock(void)
{
    JceWaterFieldSet *set = jce_water_field_set_create();
    int sim = 0;

    JceWaterFieldDesc d = desc_gerstner();
    JceWaterField *a = jce_water_field_set_acquire(set, 1u, &d);
    TEST_ASSERT_NOT_NULL(a);

    jce_water_field_set_advance(set, &sim, 2.0);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, jce_water_field_time(a));

    JceWaterField *late = jce_water_field_set_acquire(set, 2u, &d);
    TEST_ASSERT_NOT_NULL(late);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, jce_water_field_time(late));

    /* Re-acquiring the same key returns the SAME field, not a new one --
     * otherwise every frame would rebuild the ocean. */
    TEST_ASSERT_EQUAL_PTR(a, jce_water_field_set_acquire(set, 1u, &d));
    TEST_ASSERT_EQUAL_PTR(a, jce_water_field_set_find(set, 1u));
    TEST_ASSERT_NULL(jce_water_field_set_find(set, 99u));

    jce_water_field_set_destroy(set);
}

/* ── 12. Sweep forgets bodies that stopped being acquired ──────────────  */

static void test_sweep_evicts_untouched(void)
{
    JceWaterFieldSet *set = jce_water_field_set_create();
    JceWaterFieldDesc d = desc_gerstner();

    jce_water_field_set_acquire(set, 1u, &d);
    jce_water_field_set_acquire(set, 2u, &d);
    jce_water_field_set_sweep(set);          /* both touched this round */
    TEST_ASSERT_NOT_NULL(jce_water_field_set_find(set, 1u));
    TEST_ASSERT_NOT_NULL(jce_water_field_set_find(set, 2u));

    jce_water_field_set_acquire(set, 1u, &d);   /* only 1 this round */
    jce_water_field_set_sweep(set);
    TEST_ASSERT_NOT_NULL(jce_water_field_set_find(set, 1u));
    TEST_ASSERT_NULL(jce_water_field_set_find(set, 2u));

    jce_water_field_set_destroy(set);
}

static void test_set_null_safety(void)
{
    jce_water_field_set_destroy(NULL);
    jce_water_field_set_sweep(NULL);
    jce_water_field_set_release(NULL, NULL);
    TEST_ASSERT_FALSE(jce_water_field_set_advance(NULL, NULL, 1.0));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, jce_water_field_set_get_time(NULL));
    TEST_ASSERT_NULL(jce_water_field_set_acquire(NULL, 0u, NULL));
    TEST_ASSERT_NULL(jce_water_field_set_find(NULL, 0u));
}

/* ── 13. The FFT path really uses the spectral normal and fold ─────────
 *
 * Every earlier assertion here survives the auxiliary fields being absent: a
 * zero gradient still yields a unit (0,1,0) normal and a default jacobian of 1
 * is still "not folded".  So if the field forgot to REQUEST slopes and foam,
 * nothing would have complained -- the water would simply have rendered as a
 * mirror with no foam, and the tests would have stayed green.
 *
 * These assertions fail on the defaults, by construction. */

static void test_fft_field_reports_real_normals_and_folds(void)
{
    JceWaterFieldDesc d = desc_fft();
    d.fft_amplitude = 2e-2f;         /* choppy enough to fold somewhere */
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);
    jce_water_field_set_time(f, 5.0);

    /* Discriminate on EXACT zero, not on a magnitude threshold.  With slopes
     * disabled the gradient is identically 0 and the normal is exactly
     * (0,1,0); a Phillips sea at these settings has an rms slope around 1e-2,
     * so "visibly tilted" is the wrong test but "not identically flat" is
     * decisive. */
    float max_nx = 0.0f, max_nz = 0.0f;
    int jac_varies = 0;
    float min_j = 1e9f, max_j = -1e9f;
    for (int i = 0; i < 400; i++) {
        const float x = (float)(i % 20) * 3.1f - 30.0f;
        const float z = (float)(i / 20) * 3.7f - 30.0f;
        JceWaterSample s;
        if (!jce_water_field_sample(f, x, z, 0.0f, &s)) continue;

        if (fabsf(s.normal.x) > max_nx) max_nx = fabsf(s.normal.x);
        if (fabsf(s.normal.z) > max_nz) max_nz = fabsf(s.normal.z);
        if (s.jacobian < min_j) min_j = s.jacobian;
        if (s.jacobian > max_j) max_j = s.jacobian;

        /* Whatever the values, they must stay physical. */
        const float len = sqrtf(s.normal.x * s.normal.x +
                                s.normal.y * s.normal.y +
                                s.normal.z * s.normal.z);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, len);
        TEST_ASSERT_TRUE(s.normal.y > 0.0f);   /* never inverted */
    }
    /* Exactly zero on both axes is the "slopes were never enabled" signature. */
    TEST_ASSERT_TRUE(max_nx > 1e-6f);
    TEST_ASSERT_TRUE(max_nz > 1e-6f);

    /* The fold determinant must actually vary across the surface; a constant 1
     * is the "foam was never enabled" signature. */
    jac_varies = (max_j - min_j) > 1e-4f;
    TEST_ASSERT_TRUE(jac_varies);

    jce_water_field_destroy(f);
}

/* ── 14. Fetch reaches the spectrum, and 0 still means Phillips ────────
 *
 * The JONSWAP path was implemented and unit-tested with nothing in the engine
 * setting fft_fetch, so it was unreachable code.  These pin both halves: a
 * non-zero fetch must change the sea, and zero must leave it byte-identical to
 * before the parameter existed -- otherwise every scene's water would shift
 * the moment the field was added. */

static void test_fetch_selects_the_spectrum(void)
{
    JceWaterFieldDesc d = desc_fft();

    JceWaterField *phillips = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(phillips);
    jce_water_field_set_time(phillips, 3.0);

    d.fft_fetch = 150000.0f;                 /* open ocean */
    JceWaterField *jonswap = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(jonswap);
    jce_water_field_set_time(jonswap, 3.0);

    const int n = jce_water_fft_resolution(jce_water_field_fft(phillips));
    const float *a = jce_water_fft_height_data(jce_water_field_fft(phillips));
    const float *b = jce_water_fft_height_data(jce_water_field_fft(jonswap));

    double diff = 0.0;
    for (int i = 0; i < n * n; i++) diff += fabs((double)a[i] - b[i]);
    TEST_ASSERT_TRUE(diff > 1e-4);           /* the fetch actually arrived */

    jce_water_field_destroy(jonswap);

    /* Zero fetch must reproduce Phillips EXACTLY -- not approximately. */
    d.fft_fetch = 0.0f;
    JceWaterField *again = jce_water_field_create(&d);
    jce_water_field_set_time(again, 3.0);
    const float *c = jce_water_fft_height_data(jce_water_field_fft(again));
    TEST_ASSERT_EQUAL_MEMORY(a, c, sizeof(float) * (size_t)(n * n));

    jce_water_field_destroy(again);
    jce_water_field_destroy(phillips);
}

/* ── 15. Cascade periods must not realign within sight ─────────────────
 *
 * One FFT patch tiles, and the eye locks onto the REPEAT rather than the
 * detail -- more resolution does not help.  Two patches summed repeat only
 * where both repeat at once, so the sizes have to be non-commensurate.
 *
 * This is a property no screenshot test would catch either, because the
 * repetition is only visible when the camera can see several patches at once;
 * a close-up of a correct ocean and a badly-cascaded one look identical. */

static void test_commensurate_periods_repeat_immediately(void)
{
    /* The failure case: 64 divides 128 exactly, so the pair realigns at 128 m
     * and the second cascade buys nothing at all. */
    const float bad = jce_water_cascade_repeat_distance(128.0f, 64.0f, 20000.0f);
    TEST_ASSERT_TRUE(bad <= 130.0f);

    /* Halves, thirds and quarters are all traps for the same reason. */
    TEST_ASSERT_TRUE(jce_water_cascade_repeat_distance(120.0f, 40.0f, 20000.0f) <= 130.0f);
    TEST_ASSERT_TRUE(jce_water_cascade_repeat_distance(100.0f, 25.0f, 20000.0f) <= 110.0f);
}

static void test_picked_secondary_pushes_the_repeat_far_away(void)
{
    /* The fractions that MATTER are the round ones a person picks by hand.
     * At 0.5 or 0.25 the raw target divides the primary exactly, so the pair
     * realigns at the primary itself and the second cascade buys NOTHING.
     *
     * An earlier version of this test used 0.41, where the raw target already
     * happens to be non-commensurate -- so it passed even when the search was
     * mutated away entirely.  Measurement across 30 primary/fraction pairs
     * showed the search materially helps in 28 of them, and the two it does
     * not are precisely the ones the old test picked. */
    const float primary = 128.0f;
    const float fracs[3] = { 0.5f, 0.25f, 0.41f };

    for (int i = 0; i < 3; i++) {
        const float second = jce_water_cascade_pick_secondary(primary, fracs[i]);
        TEST_ASSERT_TRUE(second > 4.0f);
        TEST_ASSERT_TRUE(second < primary);

        const float picked =
            jce_water_cascade_repeat_distance(primary, second, 20000.0f);
        const float raw =
            jce_water_cascade_repeat_distance(primary, primary * fracs[i],
                                              20000.0f);

        /* Beyond any plausible view distance for a water body... */
        TEST_ASSERT_TRUE(picked > 2000.0f);
        /* ...and never worse than simply taking the fraction, which is the
         * only thing that justifies searching at all. */
        TEST_ASSERT_TRUE(picked >= raw);
    }

    /* On a round fraction the improvement must be dramatic, not marginal:
     * primary*0.5 realigns at the primary, so anything less than a large
     * multiple means the search did not run. */
    const float half_pick = jce_water_cascade_pick_secondary(primary, 0.5f);
    const float half_d =
        jce_water_cascade_repeat_distance(primary, half_pick, 20000.0f);
    const float half_raw =
        jce_water_cascade_repeat_distance(primary, primary * 0.5f, 20000.0f);
    TEST_ASSERT_TRUE(half_raw <= primary + 1.0f);   /* the trap, confirmed */
    TEST_ASSERT_TRUE(half_d > half_raw * 10.0f);
}

static void test_cascade_helpers_are_robust(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_cascade_repeat_distance(0.0f, 10.0f, 100.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_cascade_repeat_distance(10.0f, -1.0f, 100.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_cascade_pick_secondary(0.0f, 0.4f));

    /* Identical periods realign immediately -- the degenerate case must report
     * that honestly rather than running the search to its cap. */
    TEST_ASSERT_TRUE(jce_water_cascade_repeat_distance(80.0f, 80.0f, 20000.0f) <= 81.0f);

    /* A zero fraction falls back to a sane default rather than returning 0. */
    const float f = jce_water_cascade_pick_secondary(128.0f, 0.0f);
    TEST_ASSERT_TRUE(f > 4.0f && f < 128.0f);
}

/* ── 16. The second cascade is actually summed ─────────────────────────
 *
 * Choosing a good period is worthless if nothing evaluates it.  These pin the
 * three things that make the cascade real rather than nominal: it exists, it
 * changes the surface, and it rides the SAME clock as the primary. */

static void test_second_cascade_changes_the_surface(void)
{
    JceWaterFieldDesc d = desc_fft();
    d.fft_amplitude = 2e-2f;

    JceWaterField *single = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(single);
    TEST_ASSERT_NULL(jce_water_field_fft2(single));   /* 0 = one cascade */

    d.cascade_fraction = 0.41f;
    JceWaterField *dual = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(dual);
    TEST_ASSERT_NOT_NULL(jce_water_field_fft2(dual));

    /* The chosen period must be non-commensurate with the primary, not simply
     * primary*fraction -- that is the whole point of picking it. */
    const float p1 = jce_water_fft_patch_size(jce_water_field_fft(dual));
    const float p2 = jce_water_fft_patch_size(jce_water_field_fft2(dual));
    TEST_ASSERT_TRUE(p2 > 0.0f && p2 < p1);
    TEST_ASSERT_TRUE(jce_water_cascade_repeat_distance(p1, p2, 20000.0f) > 2000.0f);

    jce_water_field_set_time(single, 4.0);
    jce_water_field_set_time(dual,   4.0);

    /* Summing a second cascade must move the surface.  Identical heights would
     * mean the cascade was built and then ignored. */
    double diff = 0.0;
    int compared = 0;
    for (int i = 0; i < 40; i++) {
        const float x = (float)i * 2.3f - 40.0f;
        const float z = (float)i * -1.7f + 20.0f;
        JceWaterSample a, b;
        if (!jce_water_field_sample(single, x, z, 0.0f, &a)) continue;
        if (!jce_water_field_sample(dual,   x, z, 0.0f, &b)) continue;
        diff += fabs((double)a.position.y - b.position.y);
        compared++;
    }
    TEST_ASSERT_TRUE(compared > 20);
    TEST_ASSERT_TRUE(diff > 1e-4);

    jce_water_field_destroy(single);
    jce_water_field_destroy(dual);
}

/* Both cascades must ride ONE clock.  A second cascade with its own clock
 * would be the two-clock defect again, one level down. */
static void test_both_cascades_share_the_clock(void)
{
    JceWaterFieldDesc d = desc_fft();
    d.fft_amplitude    = 2e-2f;
    d.cascade_fraction = 0.41f;
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);

    const JceWaterFft *c2 = jce_water_field_fft2(f);
    TEST_ASSERT_NOT_NULL(c2);
    const int n = jce_water_fft_resolution(c2);
    const float *h = jce_water_fft_height_data(c2);

    double e0 = 0.0;
    for (int i = 0; i < n * n; i++) e0 += fabs((double)h[i]);

    jce_water_field_set_time(f, 12.0);
    double e1 = 0.0;
    for (int i = 0; i < n * n; i++) {
        TEST_ASSERT_FALSE(isnan(h[i]));
        e1 += fabs((double)h[i]);
    }
    /* Advancing the FIELD clock must have evolved the SECOND cascade too. */
    TEST_ASSERT_TRUE(fabs(e1 - e0) > 1e-6);

    jce_water_field_destroy(f);
}

/* cascade_fraction 0 must be byte-identical to before the feature existed. */
static void test_disabled_cascade_is_bit_identical(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *a = jce_water_field_create(&d);
    d.cascade_fraction = 0.0f;
    JceWaterField *b = jce_water_field_create(&d);

    jce_water_field_set_time(a, 6.0);
    jce_water_field_set_time(b, 6.0);

    const int n = jce_water_fft_resolution(jce_water_field_fft(a));
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(jce_water_field_fft(a)),
                             jce_water_fft_height_data(jce_water_field_fft(b)),
                             sizeof(float) * (size_t)(n * n));
    TEST_ASSERT_NULL(jce_water_field_fft2(b));

    jce_water_field_destroy(a);
    jce_water_field_destroy(b);
}


/* ── Submersion: the query that replaced a member which could never work ──
 *
 * JceWaterSample.depth was documented as signed submersion and written as a
 * literal 0.0f, because sample() has no Y to measure against.  Nothing filled
 * it, so every reader got "exactly at the surface" from something that looked
 * like data.  These are the properties that member could never have had. */

static void test_submersion_sign_and_magnitude(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);

    JceWaterSample s;
    TEST_ASSERT_TRUE(jce_water_field_sample(f, 2.0f, 5.0f, 0.0f, &s));
    const float surf = s.position.y;

    float dep = -12345.0f;
    /* Exactly at the surface. */
    TEST_ASSERT_TRUE(jce_water_field_submersion(f, 2.0f, surf, 5.0f, 0.0f, &dep));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dep);

    /* Below is POSITIVE and equals the distance.  A sign flip here inverts
     * every underwater test at once, and the result -- fog above the water and
     * clear air below it -- is a look someone could mistake for a style. */
    TEST_ASSERT_TRUE(jce_water_field_submersion(f, 2.0f, surf - 3.0f, 5.0f, 0.0f, &dep));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 3.0f, dep);

    /* Above is NEGATIVE. */
    TEST_ASSERT_TRUE(jce_water_field_submersion(f, 2.0f, surf + 1.5f, 5.0f, 0.0f, &dep));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -1.5f, dep);

    jce_water_field_destroy(f);
}

static void test_submersion_uses_the_displaced_surface(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);
    jce_water_field_set_time(f, 7.5);

    /* Find the point in a scan where the wave displaces the surface FURTHEST
     * from the still-water plane -- crest or trough, either discriminates.
     * Taking the extreme rather than the first sample over a fixed threshold
     * means the test does not depend on the fixture's amplitude happening to
     * exceed a number I picked. */
    float best_x = 0.0f, best_z = 0.0f, best_surf = d.base_height, best = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const float x = (float)(i % 20) * 2.3f;
        const float z = (float)(i / 20) * 2.7f;
        JceWaterSample s;
        if (!jce_water_field_sample(f, x, z, 0.0f, &s)) continue;
        const float lift = s.position.y - d.base_height;
        if (fabsf(lift) > fabsf(best)) {
            best = lift; best_x = x; best_z = z; best_surf = s.position.y;
        }
    }
    /* A surface that never leaves its own plane is not a wave; if this fires,
     * the failure is upstream and this test has nothing to measure. */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(best) > 1e-3f,
                             "surface is flat - nothing to discriminate");

    /* A point strictly BETWEEN the still-water plane and the displaced
     * surface.  This is exactly where the two possible implementations
     * disagree: measured against base_height it lands on one side, measured
     * against the real surface it lands on the other. */
    const float y = d.base_height + best * 0.5f;

    float dep = 0.0f;
    TEST_ASSERT_TRUE(jce_water_field_submersion(f, best_x, y, best_z, 0.0f, &dep));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, best_surf - y, dep);

    /* And it must contradict the naive plane test.  Under a crest the point is
     * submerged while sitting above base_height; in a trough it is dry while
     * sitting below.  Either way the camera would surface inside a wave, which
     * reads as the wave being broken rather than the test. */
    const bool plane_says_wet = (y < d.base_height);
    const bool truth_says_wet = (dep > 0.0f);
    TEST_ASSERT_TRUE(plane_says_wet != truth_says_wet);

    jce_water_field_destroy(f);
}

static void test_outside_the_body_is_not_depth_zero(void)
{
    JceWaterFieldDesc d = desc_fft();
    d.size_x = 20.0f;
    d.size_z = 20.0f;
    JceWaterField *f = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(f);

    /* Far outside a 20x20 body.  The refusal is asserted UNCONDITIONALLY --
     * an `if (!ok)` guard around the assertion would make this test incapable
     * of failing in exactly the case it exists to catch.
     *
     * "Not over the water" is not "at the surface": a caller that read a
     * success-with-zero here would start rendering underwater the moment it
     * walked off the edge of a pond. */
    float dep = -999.0f;
    TEST_ASSERT_FALSE(jce_water_field_submersion(f, 5000.0f, 0.0f, 5000.0f,
                                                 0.0f, &dep));
    TEST_ASSERT_EQUAL_FLOAT(-999.0f, dep);   /* left untouched */

    /* A NULL out pointer is refused rather than dereferenced. */
    TEST_ASSERT_FALSE(jce_water_field_submersion(f, 0.0f, 0.0f, 0.0f, 0.0f, NULL));
    jce_water_field_destroy(f);
}

static void test_versioned_spectrum_shared_owner(void)
{
    JceWaterFieldDesc d = desc_fft();
    JceWaterFftSpectrumDesc config = {0};
    config.struct_size = sizeof(config);
    config.version = JCE_WATER_FFT_SPECTRUM_VERSION;
    config.model = JCE_WATER_FFT_SPECTRUM_PM;
    config.depth = 20.0f;
    config.directional_blend = 1.0f;
    JceWaterFieldSet *set = jce_water_field_set_create();
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &d, 3.0));
    JceWaterField *f = jce_water_field_set_acquire_spectrum(set, 7u, &d, &config);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, jce_water_field_time(f));
    const uint64_t revision = jce_water_field_revision(f);
    TEST_ASSERT_EQUAL_PTR(f, jce_water_field_set_acquire_spectrum(set, 7u, &d, &config));
    TEST_ASSERT_EQUAL_UINT64(revision, jce_water_field_revision(f));

    /* Body wind/seed override the redundant FFT descriptor inputs. */
    JceWaterFft *reference = jce_water_fft_create(d.fft_resolution, d.fft_patch_size,
        d.fft_wind_speed, d.fft_wind_dir_x, d.fft_wind_dir_z,
        d.fft_amplitude, d.fft_seed);
    TEST_ASSERT_NOT_NULL(reference);
    config.wind_speed = d.fft_wind_speed;
    config.seed = d.fft_seed;
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(reference, &config));
    jce_water_fft_evolve(reference, 3.0f);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(reference),
        jce_water_fft_height_data(jce_water_field_fft(f)), 32 * 32 * sizeof(float));
    jce_water_fft_destroy(reference);

    jce_water_field_set_sweep(set);
    config.model = 999u;
    TEST_ASSERT_NULL(jce_water_field_set_acquire_spectrum(set, 7u, &d, &config));
    jce_water_field_set_sweep(set);
    TEST_ASSERT_EQUAL_PTR(f, jce_water_field_set_find(set, 7u));
    TEST_ASSERT_EQUAL_UINT64(revision, jce_water_field_revision(f));
    config.model = JCE_WATER_FFT_SPECTRUM_TMA;
    config.fetch = 100000.0f;
    TEST_ASSERT_EQUAL_PTR(f, jce_water_field_set_acquire_spectrum(set, 7u, &d, &config));
    TEST_ASSERT_TRUE(jce_water_field_revision(f) > revision);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, jce_water_field_time(f));

    /* Removing the extension restores the exact old Phillips surface. */
    TEST_ASSERT_EQUAL_PTR(f, jce_water_field_set_acquire_spectrum(set, 7u, &d, NULL));
    JceWaterField *legacy = jce_water_field_create(&d);
    TEST_ASSERT_NOT_NULL(legacy);
    jce_water_field_set_time(legacy, 3.0);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(jce_water_field_fft(legacy)),
        jce_water_fft_height_data(jce_water_field_fft(f)), 32 * 32 * sizeof(float));
    jce_water_field_destroy(legacy);
    jce_water_field_set_destroy(set);
}

static void assert_motion_phase(const JceWaterField *field,
                                JceWaterField *reference, double phase)
{
    TEST_ASSERT_EQUAL_DOUBLE(phase, jce_water_field_phase(field));
    jce_water_field_set_time(reference, phase);
    const JceWaterFft *actual[2] = {jce_water_field_fft(field), jce_water_field_fft2(field)};
    const JceWaterFft *expected[2] = {jce_water_field_fft(reference), jce_water_field_fft2(reference)};
    for (int i = 0; i < 2; ++i) {
        TEST_ASSERT_NOT_NULL(actual[i]);
        TEST_ASSERT_NOT_NULL(expected[i]);
        const int n = jce_water_fft_resolution(expected[i]);
        TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(expected[i]),
                                 jce_water_fft_height_data(actual[i]),
                                 (size_t)n * (size_t)n * sizeof(float));
    }
}

static void test_motion_rate_preserves_phase_and_real_clock(void)
{
    JceWaterFieldDesc desc = desc_fft();
    desc.cascade_fraction = 0.41f;
    JceWaterFieldSet *set = jce_water_field_set_create();
    JceWaterField *reference = jce_water_field_create(&desc);
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_NOT_NULL(reference);
    const int driver = 0;
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.75));

    JceWaterFieldMotion motion = {1.0f, 1.0f, 0.0f};
    JceWaterField *field = jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion);
    JceWaterField *legacy = jce_water_field_set_acquire_spectrum(set, 2, &desc, NULL);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_NOT_NULL(legacy);
    assert_motion_phase(field, reference, 0.75);
    assert_motion_phase(legacy, reference, 0.75);

    motion.phase_rate = 2.0f;
    const uint64_t first_revision = jce_water_field_revision(field);
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    TEST_ASSERT_EQUAL_UINT64(first_revision, jce_water_field_revision(field));
    assert_motion_phase(field, reference, 0.75);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.25));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, jce_water_field_time(field));
    assert_motion_phase(field, reference, 1.25);
    assert_motion_phase(legacy, reference, 1.0);

    JceWaterField *late = jce_water_field_set_acquire_motion(set, 3, &desc, NULL, &motion);
    TEST_ASSERT_NOT_NULL(late);
    assert_motion_phase(late, reference, 2.0);

    motion.phase_rate = 0.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    const uint64_t frozen_revision = jce_water_field_revision(field);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.5));
    TEST_ASSERT_EQUAL_DOUBLE(1.5, jce_water_field_time(field));
    TEST_ASSERT_EQUAL_UINT64(frozen_revision, jce_water_field_revision(field));
    assert_motion_phase(field, reference, 1.25);

    motion.phase_rate = 1.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    TEST_ASSERT_EQUAL_UINT64(frozen_revision, jce_water_field_revision(field));
    assert_motion_phase(field, reference, 1.25);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, -0.5));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, jce_water_field_time(field));
    assert_motion_phase(field, reference, 0.75);
    assert_motion_phase(legacy, reference, 1.0);

    jce_water_field_destroy(reference);
    jce_water_field_set_destroy(set);
}

static void assert_fft_displacement_equal(const JceWaterFft *expected, const JceWaterFft *actual)
{
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_NOT_NULL(actual);
    const int n = jce_water_fft_resolution(expected);
    TEST_ASSERT_EQUAL_INT(n, jce_water_fft_resolution(actual));
    const size_t bytes = (size_t)n * (size_t)n * sizeof(float);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(expected), jce_water_fft_height_data(actual), bytes);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_disp_x_data(expected), jce_water_fft_disp_x_data(actual), bytes);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_disp_z_data(expected), jce_water_fft_disp_z_data(actual), bytes);
}

static void assert_caustic_fraction_rejected(JceWaterField *field, float fraction)
{
    uint64_t revision = 42u;
    TEST_ASSERT_NULL(jce_water_field_caustic_fft(field, fraction, &revision));
    TEST_ASSERT_EQUAL_UINT64(0u, revision);
}

static void test_caustic_fft_is_lazy_and_shares_the_geometry_phase(void)
{
    JceWaterFieldDesc desc = desc_fft();
    desc.cascade_fraction = 0.41f;
    const float fraction = 0.125f;
    JceWaterFieldSet *set = jce_water_field_set_create();
    JceWaterField *reference = jce_water_field_create(&desc);
    TEST_ASSERT_NOT_NULL(set);
    TEST_ASSERT_NOT_NULL(reference);
    JceWaterFieldMotion motion = {1.0f, 1.0f, 0.0f};
    JceWaterField *field = jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion);
    TEST_ASSERT_NOT_NULL(field);

    const int driver = 0;
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.5));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.25));
    jce_water_field_set_time(reference, 0.75);
    const uint64_t geometry_revision = jce_water_field_revision(field);
    assert_caustic_fraction_rejected(field, 0.0f);
    uint64_t revision = 0u;
    const JceWaterFft *third = jce_water_field_caustic_fft(field, fraction, &revision);
    TEST_ASSERT_NOT_NULL(third);
    TEST_ASSERT_TRUE(third != jce_water_field_fft(field) && third != jce_water_field_fft2(field));
    TEST_ASSERT_EQUAL_UINT64(1u, revision); /* The earlier geometry ticks did not evaluate this source. */
    TEST_ASSERT_EQUAL_FLOAT(fraction * jce_water_fft_patch_size(jce_water_field_fft(field)),
                            jce_water_fft_patch_size(third));
    TEST_ASSERT_TRUE(jce_water_fft_patch_size(third) < jce_water_fft_patch_size(jce_water_field_fft2(field)));
    TEST_ASSERT_EQUAL_UINT64(geometry_revision, jce_water_field_revision(field));
    assert_fft_displacement_equal(jce_water_field_fft(reference), jce_water_field_fft(field));
    assert_fft_displacement_equal(jce_water_field_fft2(reference), jce_water_field_fft2(field));

    float initial_height[32 * 32];
    TEST_ASSERT_EQUAL_INT(32, jce_water_fft_resolution(third));
    memcpy(initial_height, jce_water_fft_height_data(third), sizeof(initial_height));
    assert_caustic_fraction_rejected(field, -0.125f);
    assert_caustic_fraction_rejected(field, NAN);
    assert_caustic_fraction_rejected(field, 0.75f); /* Larger period than the second cascade. */
    uint64_t repeated = 0u;
    TEST_ASSERT_EQUAL_PTR(third, jce_water_field_caustic_fft(field, fraction, &repeated));
    TEST_ASSERT_EQUAL_UINT64(revision, repeated);

    motion.phase_rate = 2.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    third = jce_water_field_caustic_fft(field, fraction, &repeated);
    TEST_ASSERT_NOT_NULL(third);
    TEST_ASSERT_EQUAL_UINT64(revision, repeated);
    TEST_ASSERT_EQUAL_MEMORY(initial_height, jce_water_fft_height_data(third), sizeof(initial_height));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.25));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, jce_water_field_time(field));
    uint64_t advanced = 0u;
    third = jce_water_field_caustic_fft(field, fraction, &advanced);
    TEST_ASSERT_NOT_NULL(third);
    TEST_ASSERT_TRUE(advanced > revision);
    TEST_ASSERT_TRUE(memcmp(initial_height, jce_water_fft_height_data(third), sizeof(initial_height)) != 0);
    jce_water_field_set_time(reference, 1.25);
    const JceWaterFft *reference_third = jce_water_field_caustic_fft(reference, fraction, NULL);
    assert_fft_displacement_equal(reference_third, third);

    motion.phase_rate = 0.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    const uint64_t frozen_geometry = jce_water_field_revision(field);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.5));
    TEST_ASSERT_EQUAL_DOUBLE(1.5, jce_water_field_time(field));
    third = jce_water_field_caustic_fft(field, fraction, &repeated);
    TEST_ASSERT_EQUAL_UINT64(advanced, repeated);
    TEST_ASSERT_EQUAL_UINT64(frozen_geometry, jce_water_field_revision(field));
    assert_fft_displacement_equal(reference_third, third);

    motion.phase_rate = 1.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_motion(set, 1, &desc, NULL, &motion));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, -0.5));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, jce_water_field_time(field));
    uint64_t rewound = 0u;
    third = jce_water_field_caustic_fft(field, fraction, &rewound);
    TEST_ASSERT_NOT_NULL(third);
    TEST_ASSERT_TRUE(rewound > advanced);
    TEST_ASSERT_EQUAL_MEMORY(initial_height, jce_water_fft_height_data(third), sizeof(initial_height));
    jce_water_field_set_time(reference, 0.75);
    reference_third = jce_water_field_caustic_fft(reference, fraction, NULL);
    assert_fft_displacement_equal(reference_third, third);

    const uint64_t final_geometry = jce_water_field_revision(field);
    uint64_t released = rewound;
    TEST_ASSERT_NULL(jce_water_field_caustic_fft(field, 0.0f, &released));
    TEST_ASSERT_EQUAL_UINT64(0u, released);
    TEST_ASSERT_EQUAL_UINT64(final_geometry, jce_water_field_revision(field));
    assert_fft_displacement_equal(jce_water_field_fft(reference), jce_water_field_fft(field));
    assert_fft_displacement_equal(jce_water_field_fft2(reference), jce_water_field_fft2(field));
    uint64_t rebuilt = 0u;
    third = jce_water_field_caustic_fft(field, fraction, &rebuilt);
    TEST_ASSERT_TRUE(rebuilt > rewound);
    assert_fft_displacement_equal(reference_third, third);
    TEST_ASSERT_EQUAL_UINT64(final_geometry, jce_water_field_revision(field));
    assert_fft_displacement_equal(jce_water_field_fft(reference), jce_water_field_fft(field));
    assert_fft_displacement_equal(jce_water_field_fft2(reference), jce_water_field_fft2(field));

    jce_water_field_destroy(reference);
    jce_water_field_set_destroy(set);
}

/* Model the shader's normalized, linear and repeating texture lookup. */
static float sample_packed_grid(const JceWaterFft *fft, const float *grid, float x, float z)
{
    const int n = jce_water_fft_resolution(fft);
    const float texels_per_metre = (float)n / jce_water_fft_patch_size(fft);
    const float gx = x * texels_per_metre - 0.5f;
    const float gz = z * texels_per_metre - 0.5f;
    const int ix = (int)floorf(gx), iz = (int)floorf(gz);
    const int x0 = (ix % n + n) % n, z0 = (iz % n + n) % n;
    const int x1 = (x0 + 1) % n, z1 = (z0 + 1) % n;
    const float u = gx - (float)ix, v = gz - (float)iz;
    const float near = grid[z0*n+x0] + u * (grid[z0*n+x1] - grid[z0*n+x0]);
    const float far = grid[z1*n+x0] + u * (grid[z1*n+x1] - grid[z1*n+x0]);
    return near + v * (far - near);
}

static void test_dual_motion_caps_packed_chop_and_solves_the_combined_surface(void)
{
    JceWaterFieldDesc desc = desc_fft();
    desc.fft_amplitude = 2e-2f;
    desc.cascade_fraction = 0.41f;
    JceWaterFieldSet *set = jce_water_field_set_create();
    TEST_ASSERT_NOT_NULL(set);
    JceWaterField *legacy = jce_water_field_set_acquire(set, 1, &desc);
    TEST_ASSERT_NOT_NULL(legacy);
    const JceWaterFft *original[2] = {jce_water_field_fft(legacy), jce_water_field_fft2(legacy)};
    float budget = 0.01f;
    for (int level = 0; level < 2; ++level) {
        TEST_ASSERT_NOT_NULL(original[level]);
        const float *dx = jce_water_fft_disp_x_data(original[level]);
        const float *dz = jce_water_fft_disp_z_data(original[level]);
        const int n = jce_water_fft_resolution(original[level]);
        float peak = 0.0f;
        for (int i = 0; i < n*n; ++i) peak = fmaxf(peak, hypotf(dx[i], dz[i]));
        TEST_ASSERT_TRUE(peak > 0.0f);
        budget = fminf(budget, peak * 0.5f);
    }

    const JceWaterFieldMotion motion = {1.0f, 1.0f, 2.0f * budget};
    JceWaterField *field = jce_water_field_set_acquire_motion(set, 2, &desc, NULL, &motion);
    TEST_ASSERT_NOT_NULL(field);
    const JceWaterFft *levels[2] = {jce_water_field_fft(field), jce_water_field_fft2(field)};
    for (int level = 0; level < 2; ++level) {
        const JceWaterFft *fft = levels[level];
        TEST_ASSERT_NOT_NULL(fft);
        const int n = jce_water_fft_resolution(fft);
        TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(original[level]),
                                 jce_water_fft_height_data(fft), (size_t)n * (size_t)n * sizeof(float));
        const float *dx = jce_water_fft_disp_x_data(fft);
        const float *dz = jce_water_fft_disp_z_data(fft);
        const float *jacobian = jce_water_fft_foam_data(fft);
        float peak = 0.0f;
        for (int i = 0; i < n*n; ++i) {
            peak = fmaxf(peak, hypotf(dx[i], dz[i]));
            const int x = i % n, z = i / n;
            const int xm = z*n+(x+n-1)%n, xp = z*n+(x+1)%n;
            const int zm = ((z+n-1)%n)*n+x, zp = ((z+1)%n)*n+x;
            const float jxx = 1.0f + 0.5f*(dx[xp]-dx[xm]);
            const float jzz = 1.0f + 0.5f*(dz[zp]-dz[zm]);
            const float cross = 0.25f*(dx[zp]-dx[zm])*(dz[xp]-dz[xm]);
            TEST_ASSERT_FLOAT_WITHIN(1e-6f, jxx*jzz-cross, jacobian[i]);
        }
        TEST_ASSERT_FLOAT_WITHIN(budget * 1e-5f, budget, peak);
    }

    for (int i = 0; i < 12; ++i) {
        const float x = -8.2f + 1.37f * (float)i, z = 3.1f - 0.97f * (float)i;
        float dx = 0.0f, dz = 0.0f, h = 0.0f, jacobian = INFINITY;
        for (int level = 0; level < 2; ++level) {
            const JceWaterFft *fft = levels[level];
            dx += sample_packed_grid(fft, jce_water_fft_disp_x_data(fft), x, z);
            dz += sample_packed_grid(fft, jce_water_fft_disp_z_data(fft), x, z);
            h += sample_packed_grid(fft, jce_water_fft_height_data(fft), x, z);
            jacobian = fminf(jacobian, sample_packed_grid(fft, jce_water_fft_foam_data(fft), x, z));
        }
        TEST_ASSERT_TRUE(hypotf(dx, dz) <= motion.max_horizontal_displacement * 1.00001f);
        JceWaterSample sample;
        TEST_ASSERT_TRUE(jce_water_field_sample_render(field, x + dx, z + dz, &sample));
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, desc.base_height + h, sample.position.y);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, jacobian, sample.jacobian);
    }
    jce_water_field_set_destroy(set);
}


static void test_nearshore_cell_budget_and_wide_clock(void)
{
    TEST_ASSERT_EQUAL_INT(32, jce_water_nearshore_grid_side(16.0f));
    TEST_ASSERT_EQUAL_INT(32, jce_water_nearshore_grid_side(28.0f));
    TEST_ASSERT_EQUAL_INT(64, jce_water_nearshore_grid_side(56.0f));
    JceWaterFieldSet *set = jce_water_field_set_create();
    TEST_ASSERT_NOT_NULL(set);
    const JceWaterFieldDesc desc = desc_fft();
    JceWaterShoreBinding binding = {0};
    binding.terrain = 2;
    binding.terrain_revision = 1;
    JceWaterShoreStateDesc grid = {
        .side = 32, .cell_size = 0.875f, .still_y = 0.0f,
        .max_incoming_height = 1.5f, .max_incoming_speed = 3.0f,
        .water = {0.25f, 0.02f, 0.8f, 0.03f, 8.0f, 0.15f, 8.0f},
        .foam = {1.5f, 0.7f, 0.35f, 3.0f, 0.5f},
    };
    float bed[4096];
    for (int i = 0; i < 4096; ++i) bed[i] = -1.0f;
    for (uint64_t key = 1; key <= 5; ++key) {
        TEST_ASSERT_NOT_NULL(jce_water_field_set_acquire(set, key, &desc));
        jce_water_field_set_configure_shore(set, key, &binding, &grid, bed);
        if (key <= 4) TEST_ASSERT_NOT_NULL(jce_water_field_set_shore(set, key));
        else TEST_ASSERT_NULL(jce_water_field_set_shore(set, key));
    }
    for (uint64_t key = 2; key <= 4; ++key)
        jce_water_field_set_configure_shore(set, key, NULL, NULL, NULL);
    grid.side = 64;
    jce_water_field_set_configure_shore(set, 1, &binding, &grid, bed);
    const JceWaterShoreState *wide = jce_water_field_set_shore(set, 1);
    TEST_ASSERT_NOT_NULL(wide);
    JceWaterShoreSnapshot before, after;
    TEST_ASSERT_TRUE(jce_water_shore_state_snapshot(wide, &before));
    grid.side = 32;
    jce_water_field_set_configure_shore(set, 2, &binding, &grid, bed);
    TEST_ASSERT_NULL(jce_water_field_set_shore(set, 2));
    TEST_ASSERT_EQUAL_PTR(wide, jce_water_field_set_shore(set, 1));
    int driver = 0;
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.05));
    TEST_ASSERT_EQUAL_PTR(wide, jce_water_field_set_shore(set, 1));
    TEST_ASSERT_TRUE(jce_water_shore_state_snapshot(wide, &after));
    TEST_ASSERT_EQUAL_INT(64, after.side);
    TEST_ASSERT_TRUE(after.revision > before.revision);
    jce_water_field_set_destroy(set);
}

static void test_ocean_near_mesh_has_no_radial_spacing_cliff(void)
{
    const float extents[] = {0.5f, 100.0f, 10000.0f, 1000000.0f};
    for (int j = 0; j < 4; ++j) {
        const float outer = extents[j];
        const float inner = fminf(0.55f, outer * 0.1f);
        const float edge = fminf(48.0f, outer * 0.25f);
        const float growth = sr_water_detail_growth(inner, edge, outer);
        float previous = 0.0f;
        for (int ring = 0; ring < SR_WATER_DETAIL_RINGS; ++ring) {
            const float radius = sr_water_detail_radius(ring, inner, edge, outer, growth);
            TEST_ASSERT_TRUE(isfinite(radius) && radius > previous);
            previous = radius;
        }
        TEST_ASSERT_FLOAT_WITHIN(outer * 1e-6f, outer, previous);
        const float pre = edge - sr_water_detail_radius(62, inner, edge, outer, growth);
        const float post = sr_water_detail_radius(64, inner, edge, outer, growth) - edge;
        TEST_ASSERT_TRUE(post > 0.0f && post / pre < 1.4f);
    }
}

/* Exercise the real topology factory with the renderer's requested budget.
 * Radius-only tests cannot detect admission returning no ocean mesh. */
static void test_ocean_detail_topology_factory_admits_renderer_budget(void)
{
    const int rings = SR_WATER_DETAIL_RINGS;
    const int segments = SR_WATER_DETAIL_SEGMENTS;
    static JceWaterRingVertex vertices[1 + SR_WATER_DETAIL_RINGS * SR_WATER_DETAIL_SEGMENTS];
    static uint32_t indices[(2 * SR_WATER_DETAIL_RINGS - 1) * SR_WATER_DETAIL_SEGMENTS * 3];
    uint32_t nv = 0, ni = 0;
    TEST_ASSERT_TRUE(jce_water_ring_mesh_size(rings, segments, &nv, &ni));
    TEST_ASSERT_EQUAL_UINT32(sizeof(vertices) / sizeof(vertices[0]), nv);
    TEST_ASSERT_EQUAL_UINT32(sizeof(indices) / sizeof(indices[0]), ni);
    TEST_ASSERT_TRUE(nv <= 65535u);
    TEST_ASSERT_TRUE(jce_water_ring_build(rings, segments, 0.55f, 10000.0f,
                                         vertices, indices));
    for (uint32_t i = 0; i < nv; ++i) {
        TEST_ASSERT_TRUE(isfinite(vertices[i].x) && isfinite(vertices[i].z));
        TEST_ASSERT_TRUE(isfinite(vertices[i].radius));
    }
    for (uint32_t i = 0; i < ni; ++i)
        TEST_ASSERT_TRUE(indices[i] < nv);
    for (uint32_t i = 0; i < ni; i += 3) {
        const JceWaterRingVertex a = vertices[indices[i]];
        const JceWaterRingVertex b = vertices[indices[i + 1]];
        const JceWaterRingVertex c = vertices[indices[i + 2]];
        const float up = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        TEST_ASSERT_TRUE(isfinite(up) && up > 0.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10000.0f, vertices[nv - 1].radius);
    TEST_ASSERT_TRUE(jce_water_ring_mesh_size(64, 512, &nv, &ni));
    TEST_ASSERT_EQUAL_UINT32(32769u, nv);
    TEST_ASSERT_FALSE(jce_water_ring_mesh_size(129, segments, &nv, &ni));
    TEST_ASSERT_EQUAL_UINT32(0u, nv);
    TEST_ASSERT_EQUAL_UINT32(0u, ni);
    TEST_ASSERT_FALSE(jce_water_ring_mesh_size(rings, 513, &nv, &ni));
    TEST_ASSERT_FALSE(jce_water_ring_mesh_size(128, 512, &nv, &ni));
    TEST_ASSERT_EQUAL_UINT32(0u, nv);
    TEST_ASSERT_EQUAL_UINT32(0u, ni);
}

static float atlas_test_half(uint16_t h)
{
    const int e = (h >> 10) & 31;
    const float v = e ? ldexpf((float)((h & 1023) + 1024), e - 25)
                      : ldexpf((float)(h & 1023), -24);
    return (h & 32768) ? -v : v;
}

static void test_nearshore_atlas_extends_wet_surface_without_dry_bed_ramps(void)
{
    JceWaterShoreCell cells[16] = {0};
    float bed[16], foam[16] = {0};
    uint16_t pixels[6 * 12 * 4];
    for (int z = 0; z < 4; ++z) {
        for (int x = 0; x < 4; ++x) {
            const int i = z * 4 + x;
            bed[i] = x < 2 ? -0.75f : 20.0f + (float)z;
            cells[i].depth = x < 2 ? 1.0f : 0.0f;
            cells[i].momentum_x = x < 2 ? 1.25f : 0.0f;
            foam[i] = x < 2 ? 0.125f : 0.0f;
        }
    }
    const JceWaterShoreSnapshot snapshot = {4, 1, 0, 0, cells, bed, foam, 7, 9};
    jce_water_shore_pack_atlas(pixels, 6, 12, &snapshot, NULL, 0);
    for (int z = 0; z < 6; ++z) {
        for (int x = 1; x <= 3; ++x) {
            const size_t i = ((size_t)z * 6 + x) * 4;
            TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, atlas_test_half(pixels[i]));
        }
        const size_t wet = ((size_t)z * 6 + 2) * 4;
        const size_t dry = ((size_t)z * 6 + 3) * 4;
        const float blended = (atlas_test_half(pixels[wet]) + atlas_test_half(pixels[dry])) * 0.5f;
        /* Neither an elevated lake nor a negative still plane changes R. */
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 23.25f, 23.0f + blended);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, -6.75f, -7.0f + blended);
        TEST_ASSERT_EQUAL_UINT16(0, pixels[dry + 1]);
        TEST_ASSERT_EQUAL_UINT16(0, pixels[dry + 2]);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, atlas_test_half(pixels[wet + 1]));
        const size_t flow = ((size_t)(z + 6) * 6 + 2) * 4;
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.25f, atlas_test_half(pixels[flow]));
        TEST_ASSERT_EQUAL_UINT16(0, pixels[flow + 4]);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 20.0f, bed[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, cells[2].depth);
}

int main(void)
{
    waves_init();
    UNITY_BEGIN();
    RUN_TEST(test_nearshore_atlas_extends_wet_surface_without_dry_bed_ramps);
    RUN_TEST(test_ocean_detail_topology_factory_admits_renderer_budget);
    RUN_TEST(test_nearshore_cell_budget_and_wide_clock);
    RUN_TEST(test_ocean_near_mesh_has_no_radial_spacing_cliff);
    RUN_TEST(test_versioned_spectrum_shared_owner);
    RUN_TEST(test_motion_rate_preserves_phase_and_real_clock);
    RUN_TEST(test_caustic_fft_is_lazy_and_shares_the_geometry_phase);
    RUN_TEST(test_dual_motion_caps_packed_chop_and_solves_the_combined_surface);
    RUN_TEST(test_submersion_sign_and_magnitude);
    RUN_TEST(test_submersion_uses_the_displaced_surface);
    RUN_TEST(test_outside_the_body_is_not_depth_zero);
    RUN_TEST(test_queryable_immediately);
    RUN_TEST(test_clock_is_idempotent);
    RUN_TEST(test_same_desc_same_clock_is_bit_exact);
    RUN_TEST(test_outside_the_body_is_refused);
    RUN_TEST(test_render_fft_rim_reuses_the_buoyancy_surface);
    RUN_TEST(test_band_limit_excludes_short_waves);
    RUN_TEST(test_sync_rebuilds_only_for_spectrum_changes);
    RUN_TEST(test_model_switch_rebuilds);
    RUN_TEST(test_jacobian_detects_folding);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_only_one_driver_advances);
    RUN_TEST(test_fields_ride_the_set_clock);
    RUN_TEST(test_sweep_evicts_untouched);
    RUN_TEST(test_set_null_safety);
    RUN_TEST(test_fft_field_reports_real_normals_and_folds);
    RUN_TEST(test_fetch_selects_the_spectrum);
    RUN_TEST(test_commensurate_periods_repeat_immediately);
    RUN_TEST(test_picked_secondary_pushes_the_repeat_far_away);
    RUN_TEST(test_cascade_helpers_are_robust);
    RUN_TEST(test_second_cascade_changes_the_surface);
    RUN_TEST(test_both_cascades_share_the_clock);
    RUN_TEST(test_disabled_cascade_is_bit_identical);
    return UNITY_END();
}
