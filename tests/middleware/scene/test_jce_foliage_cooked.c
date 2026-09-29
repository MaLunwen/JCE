/*
 * test_jce_foliage_cooked.c
 *
 * Cooked foliage placement: bake the instances once, ship the list, load it at
 * runtime instead of scattering.
 *
 * The assertion that carries the feature is the FIRST one -- a cooked list and
 * a live scatter at the same seed must agree instance for instance, bit for
 * bit.  Everything else here is the container refusing to hand back a
 * plausible-but-wrong placement: wrong stride, truncation, corruption.  A
 * partially decoded forest that reports success is worse than no forest.
 */

#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TDIM   65
#define TWORLD 64.0f
#define TMAXH  40.0f
#define CAP    2048

static JceTerrain *g_t;
static const jce_vec3 g_origin = { 32.0f, 0.0f, 32.0f };

static void make_hill(void)
{
    g_t = jce_terrain_create(TDIM, TDIM, TWORLD, TWORLD, TMAXH, 64);
    TEST_ASSERT_NOT_NULL(g_t);
    float *h = (float *)jce_terrain_heights(g_t);
    for (int z = 0; z < TDIM; z++)
        for (int x = 0; x < TDIM; x++) {
            /* A GENTLE dome.  An earlier version used a sharp Gaussian and
             * every candidate was rejected by the slope test -- the scatter
             * returned 0 and the cook test was measuring nothing. */
            const float u = (float)x / (float)(TDIM - 1) - 0.5f;
            const float v = (float)z / (float)(TDIM - 1) - 0.5f;
            h[z * TDIM + x] = 0.30f * expf(-(u * u + v * v) * 1.2f);
        }
}

static JceFoliageScatterParams params(void)
{
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed          = 90210u;
    p.density       = 0.5f;
    p.area_x        = 40.0f;
    p.area_z        = 40.0f;
    p.max_slope_deg = 60.0f;
    p.scale_min     = 0.8f;
    p.scale_max     = 1.4f;
    p.want_normals  = true;
    p.height_min    = 1.0f;
    p.height_max    = 30.0f;
    return p;
}

/* ── 1. THE POINT: cooked == scattered, bit for bit ────────────────────  */

static void test_cooked_matches_live_scatter_exactly(void)
{
    make_hill();
    JceFoliageScatterParams p = params();

    JceFoliageInstance live[CAP];
    const uint32_t n = jce_foliage_scatter(&p, g_t, &g_origin, live, CAP);
    printf("[diag] scattered n=%u\n", n);
    TEST_ASSERT_TRUE(n > 20u);

    const size_t sz = jce_foliage_cook_size(n);
    uint8_t *blob = (uint8_t *)malloc(sz);
    TEST_ASSERT_NOT_NULL(blob);
    size_t written = 0;
    TEST_ASSERT_TRUE(jce_foliage_cook(live, n, p.seed, blob, sz, &written));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sz, (uint32_t)written);

    TEST_ASSERT_EQUAL_UINT32(n, jce_foliage_cooked_count(blob, sz));
    TEST_ASSERT_EQUAL_UINT32(p.seed, jce_foliage_cooked_seed(blob, sz));
    TEST_ASSERT_TRUE(jce_foliage_cooked_validate(blob, sz));

    JceFoliageInstance loaded[CAP];
    TEST_ASSERT_EQUAL_UINT32(n, jce_foliage_load_cooked(blob, sz, loaded, CAP));

    /* Bit-exact: not "within a tolerance".  A cooked asset that merely
     * approximates the scatter would drift a forest by a few centimetres per
     * tree, which is exactly the kind of difference nobody notices until a
     * collision proxy stops lining up. */
    TEST_ASSERT_EQUAL_MEMORY(live, loaded, sizeof(JceFoliageInstance) * n);

    free(blob);
    jce_terrain_free(g_t);
}

/* ── 2. Corruption is refused, not decoded ─────────────────────────────  */

static void test_corruption_refused(void)
{
    make_hill();
    JceFoliageScatterParams p = params();
    JceFoliageInstance live[CAP];
    const uint32_t n = jce_foliage_scatter(&p, g_t, &g_origin, live, CAP);

    const size_t sz = jce_foliage_cook_size(n);
    uint8_t *blob = (uint8_t *)malloc(sz);
    jce_foliage_cook(live, n, p.seed, blob, sz, NULL);

    JceFoliageInstance out[CAP];
    /* Flip a bit inside an instance, well past the header. */
    blob[64] ^= 0x10u;
    TEST_ASSERT_FALSE(jce_foliage_cooked_validate(blob, sz));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_load_cooked(blob, sz, out, CAP));
    blob[64] ^= 0x10u;
    TEST_ASSERT_EQUAL_UINT32(n, jce_foliage_load_cooked(blob, sz, out, CAP));

    /* Truncation. */
    TEST_ASSERT_EQUAL_UINT32(0u,
        jce_foliage_load_cooked(blob, sz - 4u, out, CAP));

    /* Wrong magic. */
    blob[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_load_cooked(blob, sz, out, CAP));
    blob[0] ^= 0xFFu;

    free(blob);
    jce_terrain_free(g_t);
}

/* ── 3. A stale stride is caught ───────────────────────────────────────
 *
 * JceFoliageInstance has grown once already.  Reading an old file at the new
 * stride would produce plausible garbage -- trees at nonsense positions with
 * no error -- so the stride is recorded and verified. */

static void test_stale_stride_refused(void)
{
    JceFoliageInstance one;
    memset(&one, 0, sizeof one);
    one.scale = 1.0f;

    const size_t sz = jce_foliage_cook_size(1u);
    uint8_t *blob = (uint8_t *)malloc(sz);
    jce_foliage_cook(&one, 1u, 7u, blob, sz, NULL);

    /* Pretend the file was written by a build whose instance was smaller. */
    blob[16] = (uint8_t)(sizeof(JceFoliageInstance) - 4u);
    /* Re-hash so the ONLY thing wrong is the stride -- otherwise this would
     * merely be re-testing the hash. */
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < sz; i++) {
            if (i >= 28u && i < 32u) continue;   /* the hash slot itself */
            h ^= blob[i]; h *= 16777619u;
        }
        blob[28] = (uint8_t)(h & 0xFFu);
        blob[29] = (uint8_t)((h >> 8) & 0xFFu);
        blob[30] = (uint8_t)((h >> 16) & 0xFFu);
        blob[31] = (uint8_t)((h >> 24) & 0xFFu);
    }

    JceFoliageInstance out[4];
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_load_cooked(blob, sz, out, 4u));
    free(blob);
}

/* ── 4. Empty and capacity-limited loads ───────────────────────────────  */

static void test_empty_and_capped(void)
{
    const size_t sz = jce_foliage_cook_size(0u);
    uint8_t *blob = (uint8_t *)malloc(sz);
    TEST_ASSERT_TRUE(jce_foliage_cook(NULL, 0u, 1u, blob, sz, NULL));
    TEST_ASSERT_TRUE(jce_foliage_cooked_validate(blob, sz));
    JceFoliageInstance out[4];
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_load_cooked(blob, sz, out, 4u));
    /* An empty placement is a VALID placement -- count 0 with a good hash must
     * be distinguishable from a rejected load, which is why the count is
     * queried separately. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_cooked_count(blob, sz));
    free(blob);

    make_hill();
    JceFoliageScatterParams p = params();
    JceFoliageInstance live[CAP];
    const uint32_t n = jce_foliage_scatter(&p, g_t, &g_origin, live, CAP);
    TEST_ASSERT_TRUE(n > 8u);

    const size_t bsz = jce_foliage_cook_size(n);
    uint8_t *b2 = (uint8_t *)malloc(bsz);
    jce_foliage_cook(live, n, p.seed, b2, bsz, NULL);

    /* A caller with a smaller buffer gets a prefix, not a failure. */
    JceFoliageInstance few[8];
    TEST_ASSERT_EQUAL_UINT32(8u, jce_foliage_load_cooked(b2, bsz, few, 8u));
    TEST_ASSERT_EQUAL_MEMORY(live, few, sizeof(JceFoliageInstance) * 8u);

    free(b2);
    jce_terrain_free(g_t);
}

/* ── 5. Refusing to write into too small a buffer ──────────────────────  */

static void test_short_destination_refused(void)
{
    JceFoliageInstance one;
    memset(&one, 0, sizeof one);
    uint8_t small[8];
    TEST_ASSERT_FALSE(jce_foliage_cook(&one, 1u, 1u, small, sizeof small, NULL));
    TEST_ASSERT_FALSE(jce_foliage_cook(&one, 1u, 1u, NULL, 1024u, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_cooked_count(NULL, 0u));
    JceFoliageInstance out[4];
    TEST_ASSERT_EQUAL_UINT32(0u, jce_foliage_load_cooked(NULL, 0u, out, 4u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cooked_matches_live_scatter_exactly);
    RUN_TEST(test_corruption_refused);
    RUN_TEST(test_stale_stride_refused);
    RUN_TEST(test_empty_and_capped);
    RUN_TEST(test_short_destination_refused);
    return UNITY_END();
}
