/*
 * test_jce_rprobe_source.c — ReflectionProbe.mode selects the source cubemap.
 *
 * mode and custom_hdr_path were both authored, serialised and drawn in the
 * Inspector; sr_gather_rprobe_cb took baked_cubemap_path unconditionally, so
 * the mode combo and the Custom HDR picker it gates decided nothing.  Unity's
 * Custom mode reflects `customBakedTexture`; UE's SLS_SpecifiedCubemap is the
 * same split.  It is a PATH choice, not a capture -- which is why it is an S
 * change while near_clip / far_clip / resolution still wait on live capture.
 *
 * WORTH RECORDING: the cook already pulls custom_hdr_path into the shipped
 * PAK (jce_bundle_deps.c lists "hdrPath"), so before this, authoring a custom
 * HDR silently added a file to the bundle that nothing loaded.
 *
 * The .hdr case is not tidiness.  rprobe_cache holds 8 entries, a failed load
 * marks its slot `failed` forever and nothing evicts it, so a path the loader
 * cannot open burns a slot permanently -- and after eight, sr_rprobe_cache_get
 * returns -1 for EVERY probe in the scene, including baked ones that worked.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <string.h>

/* Internal to the scene renderer; a non-static symbol in the engine library. */
const char *sr_rprobe_source_path(const JceReflectionProbeComponent *c);
bool sr_rprobe_influences(const JceReflectionProbeComponent *c,
                          jce_vec3 center, jce_vec3 p);

static const char *pick(int mode, const char *custom, const char *baked)
{
    JceReflectionProbeComponent c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    snprintf(c.custom_hdr_path, sizeof c.custom_hdr_path, "%s", custom);
    snprintf(c.baked_cubemap_path, sizeof c.baked_cubemap_path, "%s", baked);
    const char *p = sr_rprobe_source_path(&c);
    /* Copy out: the component is a local. */
    static char out[256];
    if (!p) return NULL;
    snprintf(out, sizeof out, "%s", p);
    return out;
}

#define EXPECT(mode, custom, baked, want, msg) do {                        \
        const char *got = pick((mode), (custom), (baked));                 \
        TEST_ASSERT_NOT_NULL_MESSAGE(got, msg);                            \
        TEST_ASSERT_EQUAL_STRING_MESSAGE((want), got, msg);                \
    } while (0)

static void test_baked_is_the_default_and_ignores_a_custom_path(void)
{
    /* 0 == JCE_REFLECTION_PROBE_BAKED is the parse default and the editor's
     * add-default, so this row is every scene that exists today. */
    EXPECT(JCE_REFLECTION_PROBE_BAKED, "a.ktx", "b.ktx", "b.ktx",
           "BAKED must take the baked path even when a custom one is authored");
    EXPECT(JCE_REFLECTION_PROBE_BAKED, "", "b.ktx", "b.ktx",
           "BAKED with no custom path is the ordinary case");
}

static void test_realtime_falls_back_to_the_bake_rather_than_aliasing(void)
{
    /* Live capture does not exist -- it is what near_clip / far_clip wait on.
     * A REALTIME probe therefore reflects its last bake, exactly as today. */
    EXPECT(JCE_REFLECTION_PROBE_REALTIME, "a.ktx", "b.ktx", "b.ktx",
           "REALTIME must not quietly become CUSTOM");
    EXPECT(JCE_REFLECTION_PROBE_REALTIME, "", "b.ktx", "b.ktx",
           "REALTIME reflects its last bake");
}

static void test_custom_takes_the_authored_cubemap(void)
{
    /* THE FEATURE.  This is the one row the negative control turns red. */
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "a.ktx", "b.ktx", "a.ktx",
           "CUSTOM must bind the authored cubemap -- before this the picker "
           "wrote a path that reached the PAK and nothing else");
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "a.KTX2", "b.ktx", "a.KTX2",
           "the container check is case-insensitive");
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "a.dds", "b.ktx", "a.dds",
           ".dds is a cubemap container too");
}

static void test_custom_with_no_path_falls_back(void)
{
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "", "b.ktx", "b.ktx",
           "CUSTOM with an empty path must not stop a probe that renders");
}

static void test_a_non_container_never_reaches_the_cache(void)
{
    /* An equirectangular .hdr is one lat-long image and needs a projection
     * pass that does not exist.  Handing it to jce__ktx_load_cubemap would
     * fail AND burn one of the eight probe-cache slots permanently. */
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "sky.hdr", "b.ktx", "b.ktx",
           "an equirect .hdr must fall back, not burn a cache slot");
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "sky.png", "b.ktx", "b.ktx",
           "nor any other flat image");
    EXPECT(JCE_REFLECTION_PROBE_CUSTOM, "noextension", "b.ktx", "b.ktx",
           "nor a path with no extension at all");
}

static void test_an_out_of_range_mode_keeps_the_default(void)
{
    EXPECT(7, "a.ktx", "b.ktx", "b.ktx",
           "a hand-edited mode must keep the behaviour every scene has");
}

static void test_a_probe_with_nothing_authored_is_skipped(void)
{
    TEST_ASSERT_NULL_MESSAGE(pick(JCE_REFLECTION_PROBE_BAKED, "", ""),
        "no source at all must return NULL so the gather skips the probe, "
        "which is what the old `!baked_cubemap_path[0]` guard did");
    TEST_ASSERT_NULL_MESSAGE(pick(JCE_REFLECTION_PROBE_CUSTOM, "sky.hdr", ""),
        "an unusable custom path with no bake behind it is still nothing");
}

/* ── influence volume (box_size + blend_distance) ─────────────────────
 *
 * sr_gather_rprobe_cb picked the probe nearest the camera and applied it with
 * no test that the camera was anywhere near the probe's box.  One probe baked
 * in a room was therefore the reflection source for the whole outdoor scene
 * whenever it was the nearest -- and blend_distance, the field whose entire
 * job is to say how far past the box a probe still reaches, was read by
 * nothing.
 *
 * The band is a hard boundary, not a weighted fade: fs_pbr_body.sh has all
 * sixteen sampler stages occupied, so there is nowhere to bind a second
 * environment cubemap to fade against.  These cases pin the boundary; nothing
 * here claims a fade exists.
 */

static JceReflectionProbeComponent probe(float sx, float sy, float sz,
                                         float blend)
{
    JceReflectionProbeComponent c;
    memset(&c, 0, sizeof c);
    c.box_size[0] = sx; c.box_size[1] = sy; c.box_size[2] = sz;
    c.blend_distance = blend;
    return c;
}

static const jce_vec3 k_origin = { 0.0f, 0.0f, 0.0f };

static void test_inside_the_box_influences(void)
{
    JceReflectionProbeComponent c = probe(10, 10, 10, 0.0f);
    TEST_ASSERT_TRUE(sr_rprobe_influences(&c, k_origin, jce_v3(0, 0, 0)));
    TEST_ASSERT_TRUE_MESSAGE(
        sr_rprobe_influences(&c, k_origin, jce_v3(4.9f, -4.9f, 4.9f)),
        "box_size is a FULL size, so a 10-unit box reaches 5 units out");
}

static void test_outside_the_box_does_not(void)
{
    JceReflectionProbeComponent c = probe(10, 10, 10, 0.0f);
    TEST_ASSERT_FALSE_MESSAGE(
        sr_rprobe_influences(&c, k_origin, jce_v3(5.1f, 0, 0)),
        "before this test existed, a probe 500 units away still reflected the "
        "whole scene as long as it was the nearest one");
    TEST_ASSERT_FALSE(sr_rprobe_influences(&c, k_origin, jce_v3(0, 0, 400)));
}

static void test_blend_distance_extends_the_reach(void)
{
    /* THE discriminating pair: the SAME point, the same box, and the only
     * difference is the field.  Without it the point is outside. */
    const jce_vec3 q = jce_v3(7.0f, 0.0f, 0.0f);
    JceReflectionProbeComponent none = probe(10, 10, 10, 0.0f);
    JceReflectionProbeComponent some = probe(10, 10, 10, 3.0f);
    TEST_ASSERT_FALSE_MESSAGE(sr_rprobe_influences(&none, k_origin, q),
        "5 units of half-extent does not reach 7");
    TEST_ASSERT_TRUE_MESSAGE(sr_rprobe_influences(&some, k_origin, q),
        "blend_distance 3 does: this assertion is the whole field");
    TEST_ASSERT_FALSE_MESSAGE(
        sr_rprobe_influences(&some, k_origin, jce_v3(8.1f, 0, 0)),
        "and it stops where it says it stops");
}

static void test_the_band_is_per_axis(void)
{
    /* box + blend is an EXPANDED BOX, not a sphere -- the corner at
     * (7.9, 7.9, 0) is 11.2 units from the centre and still inside. */
    JceReflectionProbeComponent c = probe(10, 10, 10, 3.0f);
    TEST_ASSERT_TRUE(sr_rprobe_influences(&c, k_origin, jce_v3(7.9f, 7.9f, 0)));
    TEST_ASSERT_FALSE(sr_rprobe_influences(&c, k_origin, jce_v3(7.9f, 8.1f, 0)));
}

static void test_the_centre_is_the_box_centre_not_the_origin(void)
{
    const jce_vec3 c100 = { 100.0f, 0.0f, 0.0f };
    JceReflectionProbeComponent c = probe(10, 10, 10, 0.0f);
    TEST_ASSERT_TRUE(sr_rprobe_influences(&c, c100, jce_v3(102, 0, 0)));
    TEST_ASSERT_FALSE(sr_rprobe_influences(&c, c100, jce_v3(0, 0, 0)));
}

static void test_a_probe_with_no_box_stays_unbounded(void)
{
    /* The compatibility valve, and the reason it is principled: a zero extent
     * is not a small volume, it is the absence of one.  A scene whose probe
     * never got a box must not silently lose the environment it was
     * providing. */
    JceReflectionProbeComponent c = probe(0, 0, 0, 0.0f);
    TEST_ASSERT_TRUE(sr_rprobe_influences(&c, k_origin, jce_v3(9999, 0, 0)));
    JceReflectionProbeComponent flat = probe(10, 0, 10, 0.0f);
    TEST_ASSERT_TRUE_MESSAGE(
        sr_rprobe_influences(&flat, k_origin, jce_v3(9999, 0, 0)),
        "one zero axis is still no volume");
}

static void test_a_negative_blend_distance_cannot_shrink_the_box(void)
{
    JceReflectionProbeComponent c = probe(10, 10, 10, -4.0f);
    TEST_ASSERT_TRUE_MESSAGE(
        sr_rprobe_influences(&c, k_origin, jce_v3(4.9f, 0, 0)),
        "a hand-edited negative must clamp to 0, not eat the box");
}

static void test_null_influences_nothing(void)
{
    TEST_ASSERT_FALSE(sr_rprobe_influences(NULL, k_origin, k_origin));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_baked_is_the_default_and_ignores_a_custom_path);
    RUN_TEST(test_realtime_falls_back_to_the_bake_rather_than_aliasing);
    RUN_TEST(test_custom_takes_the_authored_cubemap);
    RUN_TEST(test_custom_with_no_path_falls_back);
    RUN_TEST(test_a_non_container_never_reaches_the_cache);
    RUN_TEST(test_an_out_of_range_mode_keeps_the_default);
    RUN_TEST(test_a_probe_with_nothing_authored_is_skipped);
    RUN_TEST(test_inside_the_box_influences);
    RUN_TEST(test_outside_the_box_does_not);
    RUN_TEST(test_blend_distance_extends_the_reach);
    RUN_TEST(test_the_band_is_per_axis);
    RUN_TEST(test_the_centre_is_the_box_centre_not_the_origin);
    RUN_TEST(test_a_probe_with_no_box_stays_unbounded);
    RUN_TEST(test_a_negative_blend_distance_cannot_shrink_the_box);
    RUN_TEST(test_null_influences_nothing);
    return UNITY_END();
}
