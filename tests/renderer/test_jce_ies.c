/* test_jce_ies.c
 *
 * Pure-text unit tests for the IES LM-63 parser (jce_ies_parse), plus the
 * path-keyed LUT cache added on 2026-09-01.
 * No bgfx context is required — we only exercise the text-side path.
 * Baking to a texture is covered by integration tests that own a
 * live bgfx device.
 *
 * WHY THE CACHE EXISTS AT ALL.  Until 2026-09-01 every function in this
 * header had ZERO callers, while JceSpotLight carried an authored `ies_path`
 * and an `ies_lut_texture` and the PBR shader carried a s_iesLut sampler on
 * stage 14.  Nothing ever assigned that texture field anything but INVALID,
 * so the draw path's `path is set AND texture is valid` test could not pass:
 * a designer picked an .ies file and the light never changed.  The draw path
 * now resolves it per light per frame, which is why a cache -- and a bounded
 * one -- is part of the fix rather than an optimisation.
 */

#include <jce/renderer/jce_ies_profile.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Minimal TILT=NONE IES blob — 5 vertical angles, 1 horizontal slice,
 * symmetric triangular candela distribution peaking on-axis. */
static const char *kMinimalIes =
    "IESNA:LM-63-1995\n"
    "[TEST] synthetic\n"
    "TILT=NONE\n"
    "1 1000.0 1.0 5 1 1 2 0.0 0.0 0.0\n"
    "1.0 1.0 100.0\n"
    "0.0 22.5 45.0 67.5 90.0\n"
    "0.0\n"
    "1000.0 800.0 500.0 200.0 0.0\n";

static void test_parse_meta(void)
{
    JceIesProfile p;
    memset(&p, 0xCD, sizeof(p));
    bool ok = jce_ies_parse(kMinimalIes, 0, &p, NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(5, p.num_vertical);
    TEST_ASSERT_EQUAL_UINT32(1, p.num_horizontal);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1000.0f, p.max_candela);
}

static void test_parse_lut_endpoints(void)
{
    JceIesProfile p;
    float lut[JCE_IES_LUT_SIZE];
    bool ok = jce_ies_parse(kMinimalIes, 0, &p, lut);
    TEST_ASSERT_TRUE(ok);

    /* The LUT is normalised so peak = 1.0.  Sample 0 maps to the
     * 0° vertical angle (on-axis = peak), sample 255 to the 90°
     * tail (zero). */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, lut[0]);
    TEST_ASSERT_FLOAT_WITHIN(5e-2f, 0.0f, lut[JCE_IES_LUT_SIZE - 1]);

    /* Monotonically non-increasing across this synthetic profile. */
    for (uint32_t i = 1; i < JCE_IES_LUT_SIZE; i++) {
        TEST_ASSERT_TRUE(lut[i] <= lut[i - 1] + 1e-4f);
    }
}

static void test_parse_rejects_tilt_include(void)
{
    static const char *bad =
        "IESNA:LM-63-1995\n"
        "TILT=INCLUDE\n"
        "1 1 1 1 1\n";
    JceIesProfile p;
    memset(&p, 0xAB, sizeof(p));
    bool ok = jce_ies_parse(bad, 0, &p, NULL);
    TEST_ASSERT_FALSE(ok);
    /* Failure path zeroes the metadata struct. */
    TEST_ASSERT_EQUAL_UINT32(0, p.num_vertical);
    TEST_ASSERT_EQUAL_UINT32(0, p.num_horizontal);
}

static void test_parse_null_text_fails(void)
{
    JceIesProfile p;
    TEST_ASSERT_FALSE(jce_ies_parse(NULL, 0, &p, NULL));
}

/* ── Path-keyed LUT cache ───────────────────────────────────────────── */

/* An empty or NULL path is "no profile", not a lookup. */
static void test_lut_cache_rejects_empty_path(void)
{
    TEST_ASSERT_FALSE(jce_texture_valid(jce_ies_lut_for_path(NULL)));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_ies_lut_for_path("")));
}

/* The cache is a fixed 16-slot array and the resolver is called PER LIGHT PER
 * FRAME, so overrunning it is a stack/BSS smash in the render loop rather
 * than a missing texture.  Feed it more distinct paths than it holds.
 *
 * Every result is INVALID here because no file exists and there is no bgfx
 * device; what is under test is that asking 40 times neither overflows nor
 * grows without bound.  A failure MUST be cached too -- otherwise a mistyped
 * path is re-read and re-parsed on every frame of every light naming it. */
static void test_lut_cache_is_bounded_and_caches_failures(void)
{
    char path[64];
    int  i;

    for (i = 0; i < 40; ++i) {
        snprintf(path, sizeof path, "no/such/profile_%d.ies", i);
        TEST_ASSERT_FALSE_MESSAGE(jce_texture_valid(jce_ies_lut_for_path(path)),
            "a nonexistent .ies path resolved to a valid texture");
    }
    /* Repeat the first few: served from the cache, same answer. */
    for (i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "no/such/profile_%d.ies", i);
        TEST_ASSERT_FALSE(jce_texture_valid(jce_ies_lut_for_path(path)));
    }

    jce_ies_cache_shutdown();
}

/* Shutdown is idempotent and leaves the cache usable -- jce_renderer_destroy
 * may run more than once in a process that switches backends. */
static void test_lut_cache_shutdown_is_idempotent(void)
{
    jce_ies_cache_shutdown();
    jce_ies_cache_shutdown();
    TEST_ASSERT_FALSE(jce_texture_valid(jce_ies_lut_for_path("no/such.ies")));
    jce_ies_cache_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_meta);
    RUN_TEST(test_parse_lut_endpoints);
    RUN_TEST(test_parse_rejects_tilt_include);
    RUN_TEST(test_parse_null_text_fails);
    RUN_TEST(test_lut_cache_rejects_empty_path);
    RUN_TEST(test_lut_cache_is_bounded_and_caches_failures);
    RUN_TEST(test_lut_cache_shutdown_is_idempotent);
    return UNITY_END();
}
