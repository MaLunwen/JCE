/* test_jce_world_origin.c
 *
 * Unit tests for the floating-origin large-world coordinate core
 * (jce_world_origin.{h,c}) and the scene-side rebase primitive
 * (jce_scene_apply_world_shift).  All pure / headless — no GPU, no physics.
 *
 * Covered behaviour:
 *   CORE math
 *     - jce_world_origin_default seeds origin (0,0,0) + threshold.
 *     - update() within threshold → returns 0, zero shift, origin unchanged.
 *     - update() beyond threshold → returns 1, origin advanced by the
 *       QUANTIZED shift, out_shift = -(quantized), and the load-bearing
 *       invariant absolute = origin + local holds across the rebase for a
 *       FIXED world point (local += out_shift; its absolute is unchanged).
 *     - to_absolute / to_local round-trip.
 *     - NULL safety.
 *   SCENE shift
 *     - apply_world_shift moves only ROOT locals; child local is UNCHANGED but
 *       its WORLD moves by the shift (relative geometry preserved); a second
 *       root is shifted too.
 *     - zero shift = no-op (positions identical); determinism (re-run identical).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_world_origin.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"
#include <string.h>
#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

static JceTransform mk_trs(jce_vec3 pos)
{
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position = pos;
    t.rotation = (jce_quat){ 0.0f, 0.0f, 0.0f, 1.0f }; /* identity */
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    return t;
}

/* ── CORE math ─────────────────────────────────────────────────────────── */

static void test_default_seeds_origin_and_threshold(void)
{
    JceWorldOrigin wo = jce_world_origin_default(4096.0f);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, wo.origin[0]);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, wo.origin[1]);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, wo.origin[2]);
    TEST_ASSERT_EQUAL_FLOAT(4096.0f, wo.rebase_threshold);

    /* Non-positive thresholds are clamped to a small positive value. */
    JceWorldOrigin wz = jce_world_origin_default(0.0f);
    TEST_ASSERT_TRUE(wz.rebase_threshold > 0.0f);
}

static void test_update_within_threshold_no_rebase(void)
{
    JceWorldOrigin wo = jce_world_origin_default(4096.0f);
    float cam[3] = { 1000.0f, 0.0f, 0.0f };   /* well inside 4096 */
    float shift[3] = { 9.0f, 9.0f, 9.0f };    /* must be zeroed by the call */

    int rebased = jce_world_origin_update(&wo, cam, shift);
    TEST_ASSERT_EQUAL_INT(0, rebased);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[2]);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, wo.origin[0]);
}

static void test_update_beyond_threshold_rebases_and_preserves_invariant(void)
{
    JceWorldOrigin wo = jce_world_origin_default(4096.0f);

    /* A fixed world point well away from the camera; its ABSOLUTE coordinate
     * must be invariant across the rebase. */
    float fixed_local[3] = { 4990.0f, 0.0f, 0.0f };
    double fixed_abs_before[3];
    jce_world_origin_to_absolute(&wo, fixed_local, fixed_abs_before);

    float cam[3]   = { 5000.0f, 0.0f, 0.0f };   /* beyond 4096 */
    float shift[3] = { 0.0f, 0.0f, 0.0f };

    int rebased = jce_world_origin_update(&wo, cam, shift);
    TEST_ASSERT_EQUAL_INT(1, rebased);

    /* 5000 floored to a multiple of the quantum, negated. */
    float q = JCE_WORLD_ORIGIN_QUANTUM;
    float expect_chosen = floorf(5000.0f / q) * q;   /* +shift applied to origin */
    TEST_ASSERT_EQUAL_FLOAT(-expect_chosen, shift[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[2]);

    /* origin advanced by +chosen. */
    TEST_ASSERT_EQUAL_DOUBLE((double)expect_chosen, wo.origin[0]);

    /* The fixed point's local moves by out_shift; its absolute is unchanged. */
    float fixed_local_after[3] = {
        fixed_local[0] + shift[0],
        fixed_local[1] + shift[1],
        fixed_local[2] + shift[2],
    };
    double fixed_abs_after[3];
    jce_world_origin_to_absolute(&wo, fixed_local_after, fixed_abs_after);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fixed_abs_before[0], fixed_abs_after[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fixed_abs_before[1], fixed_abs_after[1]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fixed_abs_before[2], fixed_abs_after[2]);

    /* After applying out_shift the camera is pulled back toward the origin. */
    float cam_after = cam[0] + shift[0];
    TEST_ASSERT_TRUE(fabsf(cam_after) < fabsf(cam[0]));
    TEST_ASSERT_TRUE(fabsf(cam_after) < JCE_WORLD_ORIGIN_QUANTUM);
}

static void test_to_absolute_to_local_round_trip(void)
{
    JceWorldOrigin wo = jce_world_origin_default(4096.0f);
    wo.origin[0] = 100000.0;   /* a non-trivial accumulated origin */
    wo.origin[1] = -50000.0;
    wo.origin[2] = 12345.0;

    float local[3] = { 12.5f, -7.25f, 3.0f };
    double abs[3];
    jce_world_origin_to_absolute(&wo, local, abs);

    float back[3];
    jce_world_origin_to_local(&wo, abs, back);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, local[0], back[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, local[1], back[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, local[2], back[2]);
}

static void test_update_null_safe(void)
{
    float shift[3] = { 1.0f, 2.0f, 3.0f };
    TEST_ASSERT_EQUAL_INT(0, jce_world_origin_update(NULL, NULL, shift));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, shift[2]);

    /* NULL out_shift must not crash. */
    JceWorldOrigin wo = jce_world_origin_default(10.0f);
    float cam[3] = { 1000.0f, 0.0f, 0.0f };
    (void)jce_world_origin_update(&wo, cam, NULL);
}

static void test_update_deterministic(void)
{
    float cam[3] = { 7777.0f, -3333.0f, 5555.0f };

    JceWorldOrigin a = jce_world_origin_default(4096.0f);
    JceWorldOrigin b = jce_world_origin_default(4096.0f);
    float sa[3], sb[3];
    int ra = jce_world_origin_update(&a, cam, sa);
    int rb = jce_world_origin_update(&b, cam, sb);

    TEST_ASSERT_EQUAL_INT(ra, rb);
    TEST_ASSERT_EQUAL_FLOAT(sa[0], sb[0]);
    TEST_ASSERT_EQUAL_FLOAT(sa[1], sb[1]);
    TEST_ASSERT_EQUAL_FLOAT(sa[2], sb[2]);
    TEST_ASSERT_EQUAL_DOUBLE(a.origin[0], b.origin[0]);
    TEST_ASSERT_EQUAL_DOUBLE(a.origin[1], b.origin[1]);
    TEST_ASSERT_EQUAL_DOUBLE(a.origin[2], b.origin[2]);
}

/* ── SCENE shift ───────────────────────────────────────────────────────── */

static void test_scene_apply_world_shift_roots_only(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    /* root1 at {5000,0,0} with child at LOCAL {10,0,0} → child world {5010,0,0}. */
    JceEntity root1 = jce_scene_create_entity(s, "root1");
    JceEntity child = jce_scene_create_entity(s, "child");
    JceTransform tr1 = mk_trs(jce_v3(5000.0f, 0.0f, 0.0f));
    JceTransform tc  = mk_trs(jce_v3(10.0f, 0.0f, 0.0f));
    jce_scene_set_transform(s, root1, &tr1);
    jce_scene_set_transform(s, child, &tc);
    jce_scene_set_parent(s, child, root1);

    /* A second, independent root. */
    JceEntity root2 = jce_scene_create_entity(s, "root2");
    JceTransform tr2 = mk_trs(jce_v3(5000.0f, 100.0f, -20.0f));
    jce_scene_set_transform(s, root2, &tr2);

    /* Sanity: child world before the shift. */
    jce_mat4 cw_before = jce_scene_get_world_matrix(s, child);
    TEST_ASSERT_EQUAL_FLOAT(5010.0f, cw_before.col[3].x);

    float shift[3] = { -5000.0f, 0.0f, 0.0f };
    jce_scene_apply_world_shift(s, shift);

    /* root1 LOCAL moved by shift. */
    JceTransform *r1 = jce_scene_get_transform(s, root1);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r1->position.x);

    /* child LOCAL is UNCHANGED (parent-relative; must not be double-shifted). */
    JceTransform *cl = jce_scene_get_transform(s, child);
    TEST_ASSERT_NOT_NULL(cl);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, cl->position.x);

    /* child WORLD moved by the shift, preserving relative geometry. */
    jce_mat4 cw_after = jce_scene_get_world_matrix(s, child);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, cw_after.col[3].x);
    TEST_ASSERT_EQUAL_FLOAT(cw_before.col[3].x + shift[0], cw_after.col[3].x);

    /* second root shifted too. */
    JceTransform *r2 = jce_scene_get_transform(s, root2);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,    r2->position.x);
    TEST_ASSERT_EQUAL_FLOAT(100.0f,  r2->position.y);   /* untouched axis */
    TEST_ASSERT_EQUAL_FLOAT(-20.0f,  r2->position.z);

    jce_scene_destroy(s);
}

static void test_scene_apply_world_shift_zero_is_noop(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "e");
    JceTransform t = mk_trs(jce_v3(123.0f, 456.0f, 789.0f));
    jce_scene_set_transform(s, e, &t);

    float zero[3] = { 0.0f, 0.0f, 0.0f };
    jce_scene_apply_world_shift(s, zero);

    JceTransform *after = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_FLOAT(123.0f, after->position.x);
    TEST_ASSERT_EQUAL_FLOAT(456.0f, after->position.y);
    TEST_ASSERT_EQUAL_FLOAT(789.0f, after->position.z);

    /* NULL scene is also a no-op (no crash). */
    jce_scene_apply_world_shift(NULL, zero);

    jce_scene_destroy(s);
}

static void test_scene_apply_world_shift_deterministic(void)
{
    float shift[3] = { -1234.0f, 56.0f, -78.0f };
    float results[2][3];

    for (int run = 0; run < 2; ++run) {
        JceScene *s = jce_scene_create();
        JceEntity e = jce_scene_create_entity(s, "e");
        JceTransform t = mk_trs(jce_v3(2000.0f, 3000.0f, 4000.0f));
        jce_scene_set_transform(s, e, &t);
        jce_scene_apply_world_shift(s, shift);
        JceTransform *a = jce_scene_get_transform(s, e);
        results[run][0] = a->position.x;
        results[run][1] = a->position.y;
        results[run][2] = a->position.z;
        jce_scene_destroy(s);
    }

    TEST_ASSERT_EQUAL_FLOAT(results[0][0], results[1][0]);
    TEST_ASSERT_EQUAL_FLOAT(results[0][1], results[1][1]);
    TEST_ASSERT_EQUAL_FLOAT(results[0][2], results[1][2]);
}

int main(void)
{
    UNITY_BEGIN();
    /* CORE math */
    RUN_TEST(test_default_seeds_origin_and_threshold);
    RUN_TEST(test_update_within_threshold_no_rebase);
    RUN_TEST(test_update_beyond_threshold_rebases_and_preserves_invariant);
    RUN_TEST(test_to_absolute_to_local_round_trip);
    RUN_TEST(test_update_null_safe);
    RUN_TEST(test_update_deterministic);
    /* SCENE shift */
    RUN_TEST(test_scene_apply_world_shift_roots_only);
    RUN_TEST(test_scene_apply_world_shift_zero_is_noop);
    RUN_TEST(test_scene_apply_world_shift_deterministic);
    return UNITY_END();
}
