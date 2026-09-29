/*
 * test_jce_frustum.c - AABB transform + frustum-overlap tests.
 */

#include "unity.h"

#include <jce/os/core/jce_frustum.h>

#include <float.h>

void setUp(void)    { }
void tearDown(void) { }

static void transform_aabb_reference(const jce_mat4 *m,
                                     jce_vec3 lmn,
                                     jce_vec3 lmx,
                                     jce_vec3 *out_mn,
                                     jce_vec3 *out_mx)
{
    jce_vec3 wmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    int c;

    for (c = 0; c < 8; ++c) {
        jce_vec4 lv = {
            (c & 1) ? lmx.x : lmn.x,
            (c & 2) ? lmx.y : lmn.y,
            (c & 4) ? lmx.z : lmn.z,
            1.0f
        };
        jce_vec4 wv = jce_m4_mul_v4(m, lv);

        if (wv.x < wmn.x) wmn.x = wv.x;
        if (wv.y < wmn.y) wmn.y = wv.y;
        if (wv.z < wmn.z) wmn.z = wv.z;
        if (wv.x > wmx.x) wmx.x = wv.x;
        if (wv.y > wmx.y) wmx.y = wv.y;
        if (wv.z > wmx.z) wmx.z = wv.z;
    }
    *out_mn = wmn;
    *out_mx = wmx;
}

static void assert_vec3_close(jce_vec3 expected, jce_vec3 actual)
{
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected.z, actual.z);
}

static void test_transform_aabb_matches_corner_reference_for_affine_matrix(void)
{
    jce_mat4 m = jce_m4_identity();
    jce_vec3 lmn = { -2.0f, -1.5f, -0.25f };
    jce_vec3 lmx = {  4.0f,  2.0f,  3.75f };
    jce_vec3 expected_min;
    jce_vec3 expected_max;
    jce_vec3 actual_min;
    jce_vec3 actual_max;

    m.col[0] = jce_v4( 0.0f,  2.0f,  0.5f, 0.0f);
    m.col[1] = jce_v4(-1.5f,  0.0f,  0.2f, 0.0f);
    m.col[2] = jce_v4( 0.3f, -0.4f, -3.0f, 0.0f);
    m.col[3] = jce_v4(12.0f, -7.0f,  5.0f, 1.0f);

    transform_aabb_reference(&m, lmn, lmx, &expected_min, &expected_max);
    jce_transform_aabb(&m, lmn, lmx, &actual_min, &actual_max);
    assert_vec3_close(expected_min, actual_min);
    assert_vec3_close(expected_max, actual_max);
}

static void test_transform_aabb_handles_negative_nonuniform_scale(void)
{
    jce_mat4 m = jce_m4_identity();
    jce_vec3 lmn = { -1.0f, -2.0f, -3.0f };
    jce_vec3 lmx = {  2.0f,  4.0f,  6.0f };
    jce_vec3 expected_min;
    jce_vec3 expected_max;
    jce_vec3 actual_min;
    jce_vec3 actual_max;

    m.col[0].x = -2.0f;
    m.col[1].y =  0.5f;
    m.col[2].z = -4.0f;
    m.col[3] = jce_v4(-8.0f, 3.0f, 11.0f, 1.0f);

    transform_aabb_reference(&m, lmn, lmx, &expected_min, &expected_max);
    jce_transform_aabb(&m, lmn, lmx, &actual_min, &actual_max);
    assert_vec3_close(expected_min, actual_min);
    assert_vec3_close(expected_max, actual_max);
}


/* ── frustum corners + the SAT-complemented overlap test ─────────────────── */

static jce_mat4 make_view_proj(void)
{
    /* Camera at the origin looking down +Z, 60 deg vertical, 16:9. */
    const jce_vec3 eye = { 0.0f, 0.0f, 0.0f };
    const jce_vec3 at  = { 0.0f, 0.0f, 1.0f };
    const jce_vec3 up  = { 0.0f, 1.0f, 0.0f };
    jce_mat4 view = jce_m4_look_at(eye, at, up);
    jce_mat4 proj = jce_m4_perspective(1.0471976f, 16.0f / 9.0f, 1.0f, 100.0f, true);
    return jce_m4_multiply(&proj, &view);
}

static void test_frustum_corners_lie_on_their_three_planes(void)
{
    jce_mat4 vp = make_view_proj();
    jce_vec4 planes[6];
    jce_vec3 c[8];
    int i, p;

    jce_frustum_extract_planes(&vp, planes);
    TEST_ASSERT_TRUE(jce_frustum_corners(planes, c));

    /* Every corner must sit ON its own three planes and INSIDE the other
     * three -- that is what makes it a corner and not just any point. */
    for (i = 0; i < 8; i++) {
        for (p = 0; p < 6; p++) {
            const float d = planes[p].x * c[i].x + planes[p].y * c[i].y
                          + planes[p].z * c[i].z + planes[p].w;
            const int owns = (p == ((i & 1) ? 1 : 0)) ||
                             (p == ((i & 2) ? 3 : 2)) ||
                             (p == ((i & 4) ? 5 : 4));
            if (owns) TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, d);
            else      TEST_ASSERT_TRUE(d > -0.01f);
        }
    }
}

static void test_exact_test_rejects_the_sliver_the_plane_test_accepts(void)
{
    /* The regression this exists for.  street_demo's 200k bench spawns props
     * 24 x 1200 x 24 -- so elongated that a box parked off to the side
     * straddles the frustum's corner region without leaving any single plane's
     * half-space.  The plane test alone calls that visible; it is not, and the
     * spatial grid (which tests each cell of the box separately) knew better --
     * which is how 2368 phantom "cull misses" got reported. */
    jce_mat4 vp = make_view_proj();
    jce_vec4 planes[6];
    jce_vec3 c[8];
    /* Off to the side and past the far plane.  Each plane's positive vertex is
     * a DIFFERENT corner -- the far plane is cleared by z=90 while the side
     * planes are cleared by z=200 -- so no single plane sees the whole box as
     * outside, yet no point of the box is inside all of them at once. */
    const jce_vec3 mn = { 120.0f, -600.0f,  90.0f };
    const jce_vec3 mx = { 300.0f,  600.0f, 200.0f };

    jce_frustum_extract_planes(&vp, planes);
    TEST_ASSERT_TRUE(jce_frustum_corners(planes, c));

    TEST_ASSERT_TRUE(jce_aabb_in_frustum(planes, mn, mx));            /* lies */
    TEST_ASSERT_FALSE(jce_aabb_in_frustum_exact(planes, c, mn, mx));  /* does not */
}

static void test_exact_test_never_drops_a_box_that_really_overlaps(void)
{
    /* No false negatives is the property culling actually depends on: sweep a
     * box through the view volume and assert the tight test keeps every
     * placement whose centre is inside the frustum. */
    jce_mat4 vp = make_view_proj();
    jce_vec4 planes[6];
    jce_vec3 c[8];
    int ix, iy, iz;

    jce_frustum_extract_planes(&vp, planes);
    TEST_ASSERT_TRUE(jce_frustum_corners(planes, c));

    for (iz = 1; iz <= 9; iz++)
    for (iy = -4; iy <= 4; iy++)
    for (ix = -4; ix <= 4; ix++) {
        const float z = (float)iz * 10.0f;
        const jce_vec3 pt = { (float)ix * 2.0f, (float)iy * 2.0f, z };
        const jce_vec3 mn = { pt.x - 0.5f, pt.y - 0.5f, pt.z - 0.5f };
        const jce_vec3 mx = { pt.x + 0.5f, pt.y + 0.5f, pt.z + 0.5f };
        const jce_vec3 pmn = { pt.x, pt.y, pt.z };
        if (!jce_aabb_in_frustum(planes, pmn, pmn)) continue;  /* centre outside */
        TEST_ASSERT_TRUE(jce_aabb_in_frustum_exact(planes, c, mn, mx));
    }
}

static void test_exact_test_without_corners_matches_the_plain_test(void)
{
    /* NULL corners is the documented "skip the extra axes" path -- callers
     * that cannot afford the corner solve must still get a sound answer. */
    jce_mat4 vp = make_view_proj();
    jce_vec4 planes[6];
    const jce_vec3 in_mn  = { -1.0f, -1.0f, 10.0f };
    const jce_vec3 in_mx  = {  1.0f,  1.0f, 12.0f };
    const jce_vec3 out_mn = { -1.0f, -1.0f, -50.0f };
    const jce_vec3 out_mx = {  1.0f,  1.0f, -40.0f };
    const jce_vec3 sl_mn  = { 120.0f, -600.0f,  90.0f };
    const jce_vec3 sl_mx  = { 300.0f,  600.0f, 200.0f };

    jce_frustum_extract_planes(&vp, planes);
    TEST_ASSERT_TRUE(jce_aabb_in_frustum_exact(planes, NULL, in_mn, in_mx));
    TEST_ASSERT_FALSE(jce_aabb_in_frustum_exact(planes, NULL, out_mn, out_mx));
    /* including inheriting the plane test's false positive, not hiding it */
    TEST_ASSERT_TRUE(jce_aabb_in_frustum_exact(planes, NULL, sl_mn, sl_mx));
}

static void test_frustum_corners_reports_a_degenerate_frustum(void)
{
    /* All six planes identical -> the 3x3 solve is singular.  Callers rely on
     * the false return to fall back instead of using uninitialised corners. */
    jce_vec4 planes[6];
    jce_vec3 c[8];
    int i;
    for (i = 0; i < 6; i++) planes[i] = jce_v4(0.0f, 0.0f, 1.0f, 0.0f);
    TEST_ASSERT_FALSE(jce_frustum_corners(planes, c));
}


/* jce_aabb_in_frustum_fast is sold as the SAME predicate in center/extent form:
 *     n . p_positive + w  ==  (n . c + w) + |n| . e
 * That identity holds in real arithmetic. In float it does not have to -- the
 * two sides sum different magnitudes in a different order -- so this pins how
 * far apart they can drift, over boxes deliberately straddling the planes where
 * any disagreement must live.
 *
 * The contract that matters is one-sided: the broad-phase may keep something it
 * could have culled (a wasted draw), it may never drop something visible. So a
 * fast=true / plain=false disagreement is tolerable and counted; the reverse is
 * a defect and fails outright. */
static void test_fast_test_agrees_with_the_plain_one(void)
{
    jce_mat4 v = jce_m4_look_at(jce_v3(6.0f, 4.0f, 9.0f),
                                jce_v3(0.0f, 0.0f, 0.0f),
                                jce_v3(0.0f, 1.0f, 0.0f));
    jce_mat4 p = jce_m4_perspective(55.0f * 3.14159265f / 180.0f, 1.7778f, 0.15f, 220.0f, false);
    jce_mat4 vp = jce_m4_multiply(&p, &v);
    jce_vec4 planes[6];
    float absn[6][3];
    jce_frustum_extract_planes(&vp, planes);
    jce_frustum_abs_normals(planes, absn);

    uint32_t rng = 0x1234567u;
    int disagree_safe = 0, disagree_bad = 0, both = 0;
    for (int i = 0; i < 200000; i++) {
        float r[6];
        for (int k = 0; k < 6; k++) {
            rng = rng * 1664525u + 1013904223u;
            r[k] = (float)((rng >> 8) & 0xFFFFu) / 65535.0f;
        }
        /* Centres spread across and beyond the frustum, extents from sliver to
         * huge, so a large share of samples land on a boundary. */
        jce_vec3 c = jce_v3((r[0] - 0.5f) * 260.0f,
                            (r[1] - 0.5f) * 260.0f,
                            (r[2] - 0.5f) * 260.0f);
        jce_vec3 e = jce_v3(r[3] * 30.0f + 0.001f,
                            r[4] * 30.0f + 0.001f,
                            r[5] * 30.0f + 0.001f);
        jce_vec3 mn = jce_v3(c.x - e.x, c.y - e.y, c.z - e.z);
        jce_vec3 mx = jce_v3(c.x + e.x, c.y + e.y, c.z + e.z);

        const bool a = jce_aabb_in_frustum(planes, mn, mx);
        const bool b = jce_aabb_in_frustum_fast(planes, absn, mn, mx);
        if (a && b) both++;
        else if (!a && b) disagree_safe++;   /* fast keeps more: conservative */
        else if (a && !b) disagree_bad++;    /* fast drops one: NOT allowed */
    }
    TEST_ASSERT_TRUE_MESSAGE(both > 1000, "the sample never hit the frustum");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, disagree_bad,
        "jce_aabb_in_frustum_fast culled a box the plain test keeps -- the "
        "broad-phase contract is one-sided and this is the forbidden side");
    /* Rounding-scale only. A real predicate change would show up in percent. */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(both / 1000 + 8, disagree_safe,
        "far more conservative extras than rounding explains");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_transform_aabb_matches_corner_reference_for_affine_matrix);
    RUN_TEST(test_transform_aabb_handles_negative_nonuniform_scale);
    RUN_TEST(test_frustum_corners_lie_on_their_three_planes);
    RUN_TEST(test_exact_test_rejects_the_sliver_the_plane_test_accepts);
    RUN_TEST(test_exact_test_never_drops_a_box_that_really_overlaps);
    RUN_TEST(test_exact_test_without_corners_matches_the_plain_test);
    RUN_TEST(test_frustum_corners_reports_a_degenerate_frustum);
    RUN_TEST(test_fast_test_agrees_with_the_plain_one);
    return UNITY_END();
}
