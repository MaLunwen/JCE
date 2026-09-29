/*
 * test_jce_sr_debug_targets.c — the render-target inspector's table.
 *
 * WHY THIS FILE EXISTS.  An inspector is the one tool where the failure it is
 * built to find and its own worst failure are the same picture.  A target
 * listed while stale does not show nothing -- it shows last frame's pixels,
 * or an uninitialised allocation full of plausible-looking noise -- so
 * "something appeared, and it looked like data" is exactly what a broken
 * inspector and a working one both produce.
 *
 * Every assertion here is therefore about ABSENCE: which targets must NOT be
 * listed for a given set of validity bits.  A test that only checked the
 * happy path would pass on a table that ignores every flag.
 *
 * The table is reachable headlessly because jce_sr_debug_targets_build takes
 * a plain state struct rather than a JceSceneRenderer (which owns bgfx
 * resources and cannot be constructed in a unit test).  The copy from the
 * renderer into that struct is the one place a new target could be forgotten,
 * which is what the golden name list below is for.
 */

#include "middleware/scene/jce_sr_debug_targets.h"

#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define CAP JCE_SR_DEBUG_TARGET_MAX

/* Everything on, every handle a real (non-UINT16_MAX) index. */
static void all_on(JceSrDebugTargetState *st)
{
    memset(st, 0, sizeof *st);
    st->ssao_valid           = true;
    st->depth_prepass_frame  = true;
    st->ssao_has_velocity    = true;
    st->velocity_valid_frame = true;
    st->ssao_w = 1920; st->ssao_h = 1080;
    st->depth_tex = 10; st->normal_tex = 11;
    st->albedo_tex = 12; st->velocity_tex = 13;

    st->shadow_valid       = true;
    st->shadow_use_csm     = false;
    st->shadow_map_size    = 2048;
    st->dyn_csm_atlas_size = 4096;
    st->shadow_tex = 20; st->dyn_csm_atlas_tex = 21;
    st->local_atlas_tex = 22; st->cloud_shadow_tex = 23;

    st->brdf_lut = 30;
    st->sky_transmittance_tex = 31;
    st->sky_multiscatter_tex  = 32;
}

static bool has(const JceRenderTargetInfo *rows, int n, const char *name)
{
    for (int i = 0; i < n; ++i)
        if (rows[i].name && strcmp(rows[i].name, name) == 0) return true;
    return false;
}

/* THE GOLDEN LIST.  Hand-authored on purpose: generating it from the table
 * would only prove the table agrees with itself.
 *
 * Its real job is the copy in state_from_renderer.  A target added to the
 * table whose handle is never plumbed out of the renderer stays at
 * UINT16_MAX and is silently dropped -- it would simply never appear in the
 * editor, with nothing failing.  This list is what notices. */
static void test_the_full_table_is_exactly_this(void)
{
    static const char *const GOLDEN[] = {
        "gbuffer.depth", "gbuffer.normal", "gbuffer.albedo",
        "gbuffer.velocity",
        "shadow.map", "shadow.local_atlas", "shadow.cloud",
        "lut.brdf", "lut.sky_transmittance", "lut.sky_multiscatter",
    };
    const int want = (int)(sizeof GOLDEN / sizeof GOLDEN[0]);
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];
    int n, i;

    all_on(&st);
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    printf("  all-valid state lists %d target(s)\n", n);

    TEST_ASSERT_EQUAL_INT_MESSAGE(want, n,
        "the table emitted a different number of targets than this list "
        "names -- either a row was added without being added here, or a row "
        "was added to the table without its handle being copied out of the "
        "renderer, in which case it can never appear in the editor and "
        "nothing else would say so");
    for (i = 0; i < want; ++i) {
        char msg[160];
        snprintf(msg, sizeof msg, "'%s' is missing from the all-valid list",
                 GOLDEN[i]);
        TEST_ASSERT_TRUE_MESSAGE(has(rows, n, GOLDEN[i]), msg);
    }

    /* Names are how a viewer remembers a selection across frames, so a
     * duplicate would silently move the user's choice. */
    for (i = 0; i < n; ++i) {
        TEST_ASSERT_NOT_NULL(rows[i].name);
        TEST_ASSERT_TRUE_MESSAGE(rows[i].name[0] != '\0', "an empty name");
        TEST_ASSERT_NOT_NULL_MESSAGE(rows[i].note,
            "a target with no note -- most of these are not pictures, and a "
            "viewer showing one with no explanation has shown a flat grey "
            "rectangle and said nothing");
        for (int j = i + 1; j < n; ++j)
            TEST_ASSERT_FALSE_MESSAGE(
                strcmp(rows[i].name, rows[j].name) == 0,
                "two targets share a name; a viewer that remembers the "
                "selection by name would jump between them");
    }
}

/* THE ASSERTION THIS FILE IS FOR.
 *
 * ssao_valid says the render target EXISTS, and it is deliberately kept
 * across frames -- so after SSAO, SSR and TAA are all switched off
 * mid-session the handle stays valid forever.  A table that tested the
 * handle, or only ssao_valid, would list a depth buffer frozen at the last
 * frame the pre-pass ran.  That is a real picture of a real scene that is
 * not this frame's scene, and nothing about it looks wrong. */
static void test_an_allocated_gbuffer_that_was_not_written_is_not_listed(void)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];
    int n;

    all_on(&st);
    st.depth_prepass_frame = false;   /* allocated, not written this frame */
    n = jce_sr_debug_targets_build(&st, rows, CAP);

    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "gbuffer.depth"),
        "an allocated-but-unwritten depth target was listed; the inspector "
        "would show pixels frozen at the last frame the pre-pass ran, which "
        "is a real picture of a scene that has since moved");
    TEST_ASSERT_FALSE(has(rows, n, "gbuffer.normal"));
    TEST_ASSERT_FALSE(has(rows, n, "gbuffer.albedo"));
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "gbuffer.velocity"),
        "velocity survived the depth pre-pass being skipped -- it is written "
        "by that same pass");

    /* The control: everything unrelated is still there, so the assertions
     * above are about the flag and not about the build returning nothing. */
    TEST_ASSERT_TRUE_MESSAGE(has(rows, n, "shadow.map"),
        "clearing depth_prepass_frame removed the shadow map too, so the "
        "four assertions above prove nothing about that flag");
    TEST_ASSERT_TRUE(has(rows, n, "lut.brdf"));
}

/* Velocity's own extra condition: the attachment exists only when TAA asked
 * for it.  Its handle is valid in both cases, which is what makes testing
 * the handle wrong. */
static void test_velocity_needs_taa_not_just_a_valid_handle(void)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];
    int n;

    all_on(&st);
    st.ssao_has_velocity = false;     /* handle still 13, FBO has no RT */
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "gbuffer.velocity"),
        "velocity was listed from its handle alone; with TAA off that "
        "texture holds whatever it held when TAA was last on");
    TEST_ASSERT_TRUE(has(rows, n, "gbuffer.depth"));

    all_on(&st);
    st.velocity_valid_frame = false;  /* RT exists, this frame wrote nothing */
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "gbuffer.velocity"),
        "velocity was listed on a frame that did not write motion");
}

/* The two shadow targets are alternatives.  Listing both would put a stale
 * one beside a live one with nothing in the UI to tell them apart. */
static void test_the_two_shadow_targets_are_mutually_exclusive(void)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];
    int n;

    all_on(&st);
    st.shadow_use_csm = false;
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_TRUE (has(rows, n, "shadow.map"));
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "shadow.csm_dynamic_atlas"),
        "the cascade atlas was listed while the frame used the single map");

    all_on(&st);
    st.shadow_use_csm = true;
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_TRUE (has(rows, n, "shadow.csm_dynamic_atlas"));
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "shadow.map"),
        "the single shadow map was listed while the frame used cascades");

    all_on(&st);
    st.shadow_valid = false;
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_FALSE(has(rows, n, "shadow.map"));
    TEST_ASSERT_FALSE(has(rows, n, "shadow.csm_dynamic_atlas"));
    TEST_ASSERT_TRUE_MESSAGE(has(rows, n, "shadow.local_atlas"),
        "shadow_valid is about the DIRECTIONAL map; clearing it also removed "
        "the local light atlas, which has its own lifetime");
}

/* bgfx index 0 is a real slot.  This engine has shipped that confusion twice
 * -- an empty-slot sentinel and a memset descriptor -- and a table that
 * treated 0 as absent would drop whichever target happened to be allocated
 * first, which is machine-dependent and therefore intermittent. */
static void test_handle_zero_is_a_real_texture(void)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];
    int n, i;
    bool found = false;

    all_on(&st);
    st.brdf_lut = 0;
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    for (i = 0; i < n; ++i)
        if (rows[i].name && strcmp(rows[i].name, "lut.brdf") == 0) {
            found = true;
            TEST_ASSERT_EQUAL_UINT16(0, rows[i].texture.idx);
        }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "a texture at bgfx index 0 was treated as absent; 0 is a valid "
        "handle and UINT16_MAX is the sentinel");

    /* And the other half: UINT16_MAX really is dropped. */
    all_on(&st);
    st.brdf_lut = UINT16_MAX;
    n = jce_sr_debug_targets_build(&st, rows, CAP);
    TEST_ASSERT_FALSE_MESSAGE(has(rows, n, "lut.brdf"),
        "an absent texture was listed, so the assertion above is not about "
        "handle 0 at all -- it is about the table listing everything");
}

static void test_nothing_valid_lists_nothing_and_does_not_read_memory(void)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo rows[CAP];

    memset(&st, 0, sizeof st);
    /* A zeroed state has every handle at 0, which IS valid -- so the flags
     * are what must keep the G-buffer out, and the LUTs at handle 0 are
     * genuinely listed.  Spelled out because a zeroed struct meaning
     * "everything at slot 0" is the opposite of the usual intuition. */
    st.ssao_valid = false;
    TEST_ASSERT_EQUAL_INT(0, jce_sr_debug_targets_build(&st, rows, 0));
    TEST_ASSERT_EQUAL_INT(0, jce_sr_debug_targets_build(NULL, rows, CAP));
    TEST_ASSERT_EQUAL_INT(0, jce_sr_debug_targets_build(&st, NULL, CAP));

    /* A cap smaller than the table truncates rather than overruns. */
    JceSrDebugTargetState on;
    all_on(&on);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, jce_sr_debug_targets_build(&on, rows, 2),
        "the build wrote past the caller's cap");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_full_table_is_exactly_this);
    RUN_TEST(test_an_allocated_gbuffer_that_was_not_written_is_not_listed);
    RUN_TEST(test_velocity_needs_taa_not_just_a_valid_handle);
    RUN_TEST(test_the_two_shadow_targets_are_mutually_exclusive);
    RUN_TEST(test_handle_zero_is_a_real_texture);
    RUN_TEST(test_nothing_valid_lists_nothing_and_does_not_read_memory);
    return UNITY_END();
}
