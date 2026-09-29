/* test_jce_scene_world_pose.c
 *
 * THE SCENE HAD NO WAY TO SAY WHERE AN ENTITY ACTUALLY IS.
 *
 * jce_scene_get_world_matrix composed the parent chain for RENDERING, and
 * everything else in the tree read the raw JceTransform -- which for a child
 * is an offset from its parent, not a place.  Physics read it as a place.  A
 * trigger volume parented to a door at x=4 and authored at local x=0 had its
 * collider spawned at the WORLD ORIGIN, measured by a peer session as
 * bit-identical to giving it no parent at all.
 *
 * Unity has Transform.position vs localPosition, UE has GetWorldLocation vs
 * GetRelativeLocation, Godot has global_position vs position.  This scene had
 * only the local half, so "put it where the door is" could not be written.
 *
 * WHAT THESE CASES PIN, and why each one is here:
 *
 *   1. A ROOT IS VERBATIM.  The load-bearing case.  Every scene authored
 *      before this existed is unparented, and must read back the exact bytes
 *      of its Transform -- not a value that survived jce_m4_from_trs and
 *      jce_m4_decompose and came back within 1e-6.  Asserted with EQUAL_MEMORY
 *      on the struct, because EQUAL_FLOAT would pass on a round-trip.
 *   2. A CHILD COMPOSES.  The defect itself, in the geometry that produced it.
 *   3. A CHILD OF A ROTATED PARENT composes the ROTATION too -- a local +X
 *      offset under a 90-degree-yawed parent is a world -Z offset.  Without
 *      this, "add the parent's position" would pass case 2 and still be wrong.
 *   4. SET IS THE INVERSE OF GET, under a parent that both moves and rotates.
 *   5. A ZERO-SCALED PARENT STILL YIELDS A FINITE POSE.  This case was
 *      written to assert that the setter REFUSES such a parent, because
 *      jce_m4_inverse returns IDENTITY below |det| 1e-8 rather than failing.
 *      It failed, and the reason is the finding: jce_v3_safe_scale maps a
 *      zero component to 1, so the frame is never actually singular and the
 *      guard in set_world_pose cannot be reached through this API.  The case
 *      now pins the guarantee that is real -- no NaN, correct composition --
 *      and the guard is documented as a floor rather than as behaviour.
 *   6. SET LEAVES SCALE ALONE.  Physics and scripts move things; neither
 *      authors scale, and silently normalising it would resize colliders.
 *
 * NOT ASSERTED HERE: that physics uses any of this.  That is the next commit
 * and it has its own measurement; this file is about the scene API being
 * correct, which is a different claim from it being consumed.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceTransform trs(float px, float py, float pz, jce_quat rot, float s)
{
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position = jce_v3(px, py, pz);
    t.rotation = rot;
    t.scale    = jce_v3(s, s, s);
    return t;
}

static jce_quat ident_q(void)
{
    jce_quat q;
    q.x = q.y = q.z = 0.0f;
    q.w = 1.0f;
    return q;
}

static JceEntity spawn(JceScene *s, const char *name, JceTransform t)
{
    JceEntity e = jce_scene_create_entity(s, name);
    jce_scene_set_transform(s, e, &t);
    return e;
}

/* ---------------------------------------------------------------------- */

static void test_a_root_reads_back_its_transform_byte_for_byte(void)
{
    /* THE ONE THAT PROTECTS EVERY EXISTING SCENE.  A root's world pose is its
     * local pose, and it must be the SAME BYTES -- composing a matrix and
     * decomposing it costs three sqrtf and does not round-trip exactly, so a
     * caller comparing against the Transform it can read itself would see
     * drift that is entirely the decomposition's. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    /* Deliberately awkward values: a non-unit scale and an off-axis rotation,
     * so a round-trip would show up rather than being hidden by symmetry. */
    jce_quat r = jce_q_from_axis_angle(jce_v3(0.3f, 0.8f, -0.5f), 0.7f);
    JceTransform want = trs(3.25f, -7.5f, 11.125f, r, 1.0f);
    want.scale = jce_v3(2.0f, 0.5f, 3.0f);
    JceEntity e = spawn(s, "root", want);

    jce_vec3 p, sc;
    jce_quat q;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, e, &p, &q, &sc));

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want.position, &p, sizeof p,
        "a root entity's world position was not its Transform's bytes -- an "
        "unparented scene is now only approximately where it was authored");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want.rotation, &q, sizeof q,
        "a root entity's world rotation was round-tripped through a matrix");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want.scale, &sc, sizeof sc,
        "a root entity's world scale was round-tripped through a matrix");

    jce_scene_destroy(s);
}

static void test_a_child_is_where_its_parent_puts_it(void)
{
    /* THE DEFECT, in the geometry a peer measured it in: a trigger parented to
     * a door at x=4, authored at local x=0.  Reading the Transform gives 0 --
     * the world origin, which in that scene is where the player falls. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity door = spawn(s, "Door", trs(4.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    JceEntity trig = spawn(s, "Trigger", trs(0.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    jce_scene_set_parent(s, trig, door);

    jce_vec3 p;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, trig, &p, NULL, NULL));
    printf("  child of a door at x=4, local x=0 -> world x=%.5f\n", (double)p.x);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(4.0f, p.x,
        "a child entity's world position is still its LOCAL offset -- this is "
        "the bug: parenting it to something at x=4 puts it exactly where no "
        "parent at all puts it");

    /* NEGATIVE CONTROL: the identical entity WITHOUT a parent must still read
     * 0, or the test above would pass on a build that simply added 4 to
     * everything. */
    JceEntity lone = spawn(s, "Lone", trs(0.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    jce_vec3 lp;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, lone, &lp, NULL, NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lp.x);

    jce_scene_destroy(s);
}

static void test_a_rotated_parent_rotates_the_offset(void)
{
    /* "Add the parent's position" passes the case above and is still wrong.
     * A local +X offset under a parent yawed 90 degrees is a world -Z offset;
     * this is the assertion that separates composition from addition. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    const float half_pi = 1.57079632679f;
    jce_quat yaw90 = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), half_pi);

    JceEntity turret = spawn(s, "Turret", trs(0.0f, 0.0f, 0.0f, yaw90, 1.0f));
    JceEntity muzzle = spawn(s, "Muzzle", trs(2.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    jce_scene_set_parent(s, muzzle, turret);

    jce_vec3 p;
    jce_quat q;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, muzzle, &p, &q, NULL));
    printf("  local +2X under a 90deg-yawed parent -> (%.4f, %.4f, %.4f)\n",
           (double)p.x, (double)p.y, (double)p.z);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f, p.x,
        "the parent's ROTATION was not applied to the child's offset -- the "
        "composition is an addition");
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, p.y);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, -2.0f, p.z,
        "a local +X offset under a 90-degree yaw should be a world -Z offset");

    /* The child inherits the orientation too: a muzzle authored facing +X on a
     * turret that has turned is facing -Z in the world. */
    jce_vec3 fwd = jce_q_rotate(q, jce_v3(1.0f, 0.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, -1.0f, fwd.z,
        "the child's world ROTATION did not inherit the parent's");

    jce_scene_destroy(s);
}

static void test_set_is_the_inverse_of_get(void)
{
    /* A parent that both moves and rotates, so an implementation that handles
     * only translation fails here rather than passing by luck. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    jce_quat r = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), 0.9f);
    JceEntity parent = spawn(s, "Parent", trs(10.0f, -2.0f, 5.0f, r, 1.0f));
    JceEntity child  = spawn(s, "Child",  trs(0.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    jce_scene_set_parent(s, child, parent);

    const jce_vec3 want_p = jce_v3(-3.5f, 8.25f, 0.75f);
    const jce_quat want_q = jce_q_from_axis_angle(jce_v3(1.0f, 0.0f, 0.0f), 0.4f);

    TEST_ASSERT_TRUE_MESSAGE(jce_scene_set_world_pose(s, child, want_p, want_q),
        "set_world_pose refused a perfectly ordinary parent");

    /* The local transform must NOT be the world pose -- if it were, the setter
     * did nothing and the read-back below would still agree with itself. */
    JceTransform *lt = jce_scene_get_transform(s, child);
    TEST_ASSERT_NOT_NULL(lt);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(lt->position.x - want_p.x) > 1e-3f,
        "the local position equals the world position, so the setter stored "
        "the world pose verbatim under a parent that is not at the origin");

    jce_vec3 got_p;
    jce_quat got_q;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, child, &got_p, &got_q, NULL));
    printf("  asked (%.4f, %.4f, %.4f), read back (%.4f, %.4f, %.4f)\n",
           (double)want_p.x, (double)want_p.y, (double)want_p.z,
           (double)got_p.x, (double)got_p.y, (double)got_p.z);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, want_p.x, got_p.x,
        "set then get did not return the pose that was set");
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, want_p.y, got_p.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, want_p.z, got_p.z);

    /* Quaternions are double-cover: q and -q are the same rotation, so compare
     * what they DO, not their components. */
    jce_vec3 a = jce_q_rotate(want_q, jce_v3(0.0f, 0.0f, 1.0f));
    jce_vec3 b = jce_q_rotate(got_q,  jce_v3(0.0f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, a.x, b.x, "rotation did not survive");
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, a.y, b.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, a.z, b.z);

    jce_scene_destroy(s);
}

static void test_a_zero_scaled_parent_yields_a_finite_pose(void)
{
    /* THIS CASE ASSERTED THE WRONG THING FIRST, AND THE CORRECTION IS THE
     * INTERESTING PART.  It was written expecting set_world_pose to REFUSE a
     * zero-scaled parent, because jce_m4_inverse returns IDENTITY below
     * |det| 1e-8 rather than failing, and an identity "inverse" would place
     * the child at the world pose as though it had no parent.  It failed:
     * jce_v3_safe_scale, which every TRS composition in the scene goes
     * through, substitutes 1 for a zero component, so the parent arrives as a
     * well-conditioned unit frame and the setter correctly succeeds.
     *
     * The guard in set_world_pose is therefore UNREACHABLE through this API,
     * and is documented as a floor rather than as behaviour.  What IS worth
     * pinning is the guarantee that survives: a degenerate authored scale
     * produces a finite, correct pose and never NaN.  That protection comes
     * from safe_scale, so this is the test that would notice it being
     * removed. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceTransform flat = trs(6.0f, 0.0f, 0.0f, ident_q(), 1.0f);
    flat.scale = jce_v3(0.0f, 0.0f, 0.0f);      /* degenerate authored scale */
    JceEntity parent = spawn(s, "Collapsed", flat);
    JceEntity child  = spawn(s, "Child", trs(1.0f, 2.0f, 3.0f, ident_q(), 1.0f));
    jce_scene_set_parent(s, child, parent);

    /* Reading composes as though the parent were unit-scaled: 6 + 1 = 7. */
    jce_vec3 wp;
    TEST_ASSERT_TRUE(jce_scene_get_world_pose(s, child, &wp, NULL, NULL));
    printf("  child of a zero-scaled parent at x=6 -> world x=%.5f\n",
           (double)wp.x);
    TEST_ASSERT_TRUE_MESSAGE(wp.x == wp.x,           /* NaN != NaN */
        "a zero authored scale on the parent produced NaN in the child's "
        "world position");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.0f, wp.x,
        "a zero parent scale did not compose as unit scale -- jce_v3_safe_"
        "scale is no longer sanitising the TRS, and every matrix in this "
        "scene now collapses on a degenerate authored value");

    TEST_ASSERT_TRUE_MESSAGE(
        jce_scene_set_world_pose(s, child, jce_v3(99.0f, 0.0f, 0.0f),
                                 ident_q()),
        "set_world_pose refused a parent that safe_scale had already made "
        "well-conditioned");
    const float lx = jce_scene_get_transform(s, child)->position.x;
    TEST_ASSERT_TRUE_MESSAGE(lx == lx, "the solved local position is NaN");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 93.0f, lx,
        "the solved local position is not world minus the parent's, so the "
        "inverse used was not the parent's frame");

    jce_scene_destroy(s);
}

static void test_set_does_not_touch_scale(void)
{
    /* Physics and gameplay scripts move things; neither authors scale.  A
     * setter that normalised it would silently resize every collider it
     * repositioned. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity parent = spawn(s, "P", trs(1.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    JceTransform ct = trs(0.0f, 0.0f, 0.0f, ident_q(), 1.0f);
    ct.scale = jce_v3(2.0f, 3.0f, 4.0f);
    JceEntity child = spawn(s, "C", ct);
    jce_scene_set_parent(s, child, parent);

    TEST_ASSERT_TRUE(jce_scene_set_world_pose(s, child, jce_v3(5.0f, 5.0f, 5.0f),
                                              ident_q()));
    JceTransform *lt = jce_scene_get_transform(s, child);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&ct.scale, &lt->scale, sizeof ct.scale,
        "set_world_pose rewrote the authored scale");

    /* And on a root, which takes the other branch entirely. */
    JceEntity root = spawn(s, "R", ct);
    TEST_ASSERT_TRUE(jce_scene_set_world_pose(s, root, jce_v3(1.0f, 1.0f, 1.0f),
                                              ident_q()));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        &ct.scale, &jce_scene_get_transform(s, root)->scale, sizeof ct.scale,
        "set_world_pose rewrote the authored scale on a ROOT entity");

    jce_scene_destroy(s);
}

static void test_solving_writes_nothing(void)
{
    /* THE WHOLE REASON THE SOLVE IS A SEPARATE CALL.  The physics write-back
     * runs for every non-static body every frame and mutates Transforms IN
     * PLACE on purpose: jce_scene_set_world_pose goes through
     * jce_scene_set_transform, which calls ecs_set_ptr and bumps the entity
     * subtree's world-cache generation, and paying that per body per frame is
     * exactly what the in-place design avoids.  So the hot path asks for the
     * numbers and writes them itself -- which is only safe if asking is free
     * of side effects.  A solver that quietly wrote would turn one shared
     * derivation back into two behaviours. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity parent = spawn(s, "P", trs(10.0f, 0.0f, 0.0f, ident_q(), 1.0f));
    JceTransform ct = trs(1.0f, 2.0f, 3.0f, ident_q(), 1.0f);
    JceEntity child = spawn(s, "C", ct);
    jce_scene_set_parent(s, child, parent);

    const JceTransform before = *jce_scene_get_transform(s, child);

    jce_vec3 lp;
    jce_quat lr;
    TEST_ASSERT_TRUE(jce_scene_solve_local_pose(s, child,
                                                jce_v3(40.0f, 0.0f, 0.0f),
                                                ident_q(), &lp, &lr));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 30.0f, lp.x,
        "the solved local position is not world minus the parent's");

    const JceTransform after = *jce_scene_get_transform(s, child);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &after, sizeof before,
        "jce_scene_solve_local_pose MUTATED the entity -- every in-place "
        "physics write-back now writes twice, once through the solver's hidden "
        "store and once through its own pointer");

    /* And it agrees with the setter, which is built on it: same inputs, same
     * local pose.  If these ever diverge, the convenience path and the hot
     * path are two derivations again. */
    TEST_ASSERT_TRUE(jce_scene_set_world_pose(s, child, jce_v3(40.0f, 0.0f, 0.0f),
                                              ident_q()));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, lp.x,
        jce_scene_get_transform(s, child)->position.x,
        "set_world_pose and solve_local_pose disagree on the same input");

    /* NULL out params are allowed -- the write-back sites that only move a
     * position pass NULL for rotation. */
    TEST_ASSERT_TRUE(jce_scene_solve_local_pose(s, child, jce_v3(1.0f, 1.0f,
                                                1.0f), ident_q(), NULL, NULL));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_root_reads_back_its_transform_byte_for_byte);
    RUN_TEST(test_a_child_is_where_its_parent_puts_it);
    RUN_TEST(test_a_rotated_parent_rotates_the_offset);
    RUN_TEST(test_set_is_the_inverse_of_get);
    RUN_TEST(test_a_zero_scaled_parent_yields_a_finite_pose);
    RUN_TEST(test_set_does_not_touch_scale);
    RUN_TEST(test_solving_writes_nothing);
    return UNITY_END();
}
