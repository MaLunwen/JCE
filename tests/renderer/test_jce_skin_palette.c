/* test_jce_skin_palette.c
 *
 * Pure-math unit tests for jce_skin_build_world_palette — the shared
 * world-space bone-palette builder consumed by both the color pass
 * (jce_model_draw) and the new shadow pass (jce_model_draw_shadow).
 * No bgfx is touched; this only walks jce_math.
 */

#include <jce/renderer/jce_skin_palette.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

/* +90deg rotation about Z, column-major (raw[col][row]).
 * Image of +X axis = (0,1,0); image of +Y axis = (-1,0,0). */
static jce_mat4 rot_z_90(void)
{
    jce_mat4 m = jce_m4_identity();
    m.raw[0][0] =  0.0f; m.raw[0][1] = 1.0f;
    m.raw[1][0] = -1.0f; m.raw[1][1] = 0.0f;
    return m;
}

/* out[i] must be root * joints[i] (joint applied first, then root).
 * root = rotZ(90deg), joint = translate(1,0,0).
 * Applied to the origin: translate -> (1,0,0), then rotZ90 -> (0,1,0).
 * So the product's translation column is (0,1,0).  The reversed order
 * (joint * root) would yield (1,0,0), so this also pins the multiply order. */
static void test_world_palette_applies_root_times_joint(void)
{
    jce_mat4 root  = rot_z_90();
    jce_mat4 joint = jce_m4_translate(jce_v3(1.0f, 0.0f, 0.0f));
    jce_mat4 out[1];

    uint32_t n = jce_skin_build_world_palette(&root, &joint, 1, out, 1);

    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0].col[3].x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0].col[3].y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0].col[3].z);
}

/* num_joints > out_cap must clamp to out_cap. */
static void test_world_palette_clamps_to_out_cap(void)
{
    jce_mat4 root = jce_m4_identity();
    jce_mat4 joints[5];
    for (int i = 0; i < 5; i++) joints[i] = jce_m4_identity();
    jce_mat4 out[3];

    uint32_t n = jce_skin_build_world_palette(&root, joints, 5, out, 3);
    TEST_ASSERT_EQUAL_UINT32(3, n);
}

/* NULL root behaves like identity: joints copied through unchanged. */
static void test_world_palette_null_root_is_identity_passthrough(void)
{
    jce_mat4 joint = jce_m4_translate(jce_v3(2.0f, 3.0f, 4.0f));
    jce_mat4 out[1];

    uint32_t n = jce_skin_build_world_palette(NULL, &joint, 1, out, 1);

    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, out[0].col[3].x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, out[0].col[3].y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 4.0f, out[0].col[3].z);
}

/* Degenerate inputs return 0 and write nothing. */
static void test_world_palette_null_args_return_zero(void)
{
    jce_mat4 joint = jce_m4_identity();
    jce_mat4 out[1];
    TEST_ASSERT_EQUAL_UINT32(0, jce_skin_build_world_palette(NULL, NULL,  1, out,  1));
    TEST_ASSERT_EQUAL_UINT32(0, jce_skin_build_world_palette(NULL, &joint, 1, NULL, 1));
    TEST_ASSERT_EQUAL_UINT32(0, jce_skin_build_world_palette(NULL, &joint, 0, out,  1));
    TEST_ASSERT_EQUAL_UINT32(0, jce_skin_build_world_palette(NULL, &joint, 1, out,  0));
}

/* ── jce_skin_bone_world: where a BONE is, not where its vertices go ──────
 *
 * A palette entry is `global * inverseBind` -- it moves a vertex, and it is
 * not a pose.  Attaching a weapon to a hand needs the bone's own transform,
 * and jce_skeleton.h:92 has stated the recovery since it was written while
 * nothing in the engine used it.
 *
 * THE LOAD-BEARING ASSERTION IS NOT "it returns a matrix".  It is that a bone
 * which has MOVED reports where it moved to, because the bind pose is also a
 * perfectly plausible-looking answer -- a helper that ignored the palette
 * entirely would pass every test that only checks the rest case, and would
 * pin a weapon to the hand's T-pose position forever.
 */

/* Build the palette entry a real renderer would hold for a bone whose global
 * transform is `global`, so the recovery is tested against its own input
 * rather than against arithmetic restated in the test. */
static jce_mat4 palette_entry_for(jce_mat4 global, jce_mat4 inverse_bind)
{
    return jce_m4_multiply(&global, &inverse_bind);
}

static void test_bone_world_recovers_the_moved_bone(void)
{
    /* Bind: the bone rests at x=1.  Animated: it has swung to y=3.
     * These must be DISTINGUISHABLE -- that is the whole point. */
    const jce_mat4 bind    = jce_m4_translate(jce_v3(1.0f, 0.0f, 0.0f));
    const jce_mat4 inv_bind = jce_m4_inverse(&bind);
    const jce_mat4 global  = jce_m4_translate(jce_v3(0.0f, 3.0f, 0.0f));
    const jce_mat4 pal     = palette_entry_for(global, inv_bind);

    jce_mat4 out;
    TEST_ASSERT_TRUE(jce_skin_bone_world(NULL, &pal, 1, &inv_bind, 0, &out));

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 0.0f, out.col[3].x,
        "the bone reported its BIND x, not the pose it animated to -- a "
        "helper that ignores the palette passes every rest-pose test");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 3.0f, out.col[3].y,
        "the animated translation did not survive the recovery");
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.col[3].z);
}

static void test_bone_world_applies_root_last(void)
{
    /* Same order jce_skin_build_world_palette pins: root * global.  The
     * reversed order puts the answer somewhere else, so this is a real
     * constraint and not decoration. */
    const jce_mat4 bind     = jce_m4_identity();
    const jce_mat4 inv_bind = jce_m4_identity();
    const jce_mat4 global   = jce_m4_translate(jce_v3(1.0f, 0.0f, 0.0f));
    const jce_mat4 pal      = palette_entry_for(global, inv_bind);
    const jce_mat4 root     = rot_z_90();
    (void)bind;

    jce_mat4 out;
    TEST_ASSERT_TRUE(jce_skin_bone_world(&root, &pal, 1, &inv_bind, 0, &out));

    /* translate(1,0,0) then rotZ90 -> (0,1,0).  The reversed order gives
     * (1,0,0), which is what a transposed convention would produce. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 0.0f, out.col[3].x,
        "root was applied in the wrong order -- this is the transpose bug "
        "this tree has paid for four times");
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out.col[3].y);
}

static void test_bone_world_falls_back_to_bind(void)
{
    /* skin_palette_count == 0 is how this renderer says "draw the bind pose".
     * An attachment on a model with no clip playing must sit where the bone
     * RESTS -- not at the origin, and not nowhere. */
    const jce_mat4 bind     = jce_m4_translate(jce_v3(0.0f, 0.0f, 5.0f));
    const jce_mat4 inv_bind = jce_m4_inverse(&bind);

    jce_mat4 out;
    TEST_ASSERT_TRUE(jce_skin_bone_world(NULL, NULL, 0, &inv_bind, 0, &out));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 5.0f, out.col[3].z,
        "an empty palette did not fall back to the bind pose");

    /* A bone index past the end of a NON-empty palette takes the same path. */
    const jce_mat4 moved = jce_m4_translate(jce_v3(9.0f, 9.0f, 9.0f));
    const jce_mat4 pal   = palette_entry_for(moved, inv_bind);
    TEST_ASSERT_TRUE(jce_skin_bone_world(NULL, &pal, 1, &inv_bind, 7, &out));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 5.0f, out.col[3].z,
        "an out-of-range bone index read past the palette");

    /* POSITIVE CONTROL: index 0 of that same palette DOES report the moved
     * pose, so the two assertions above are about the fallback and not about
     * a helper that always answers bind. */
    TEST_ASSERT_TRUE(jce_skin_bone_world(NULL, &pal, 1, &inv_bind, 0, &out));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 9.0f, out.col[3].z,
        "positive control failed: the in-range bone also answered bind, so "
        "the fallback cases prove nothing");
}

static void test_bone_world_rejects_null_args(void)
{
    const jce_mat4 inv_bind = jce_m4_identity();
    jce_mat4 out;
    TEST_ASSERT_FALSE(jce_skin_bone_world(NULL, NULL, 0, NULL, 0, &out));
    TEST_ASSERT_FALSE(jce_skin_bone_world(NULL, NULL, 0, &inv_bind, 0, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_world_palette_applies_root_times_joint);
    RUN_TEST(test_world_palette_clamps_to_out_cap);
    RUN_TEST(test_world_palette_null_root_is_identity_passthrough);
    RUN_TEST(test_world_palette_null_args_return_zero);
    RUN_TEST(test_bone_world_recovers_the_moved_bone);
    RUN_TEST(test_bone_world_applies_root_last);
    RUN_TEST(test_bone_world_falls_back_to_bind);
    RUN_TEST(test_bone_world_rejects_null_args);
    return UNITY_END();
}
