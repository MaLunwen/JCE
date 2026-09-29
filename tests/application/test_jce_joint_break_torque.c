/*
 * test_jce_joint_break_torque.c — the other half of a break threshold.
 *
 * A JceConfigurableJoint has break_force AND break_torque.  The force half
 * worked end to end.  The torque half was authored in the inspector, parsed
 * and serialised by the scene, copied into the runtime's ConfigJointEntry --
 * and then read by nothing.  A designer set "this joint snaps at 40 N*m", the
 * scene saved it, the runtime tracked it, and the joint never snapped.  No
 * error anywhere, because there was nothing to error about: the monitor simply
 * had no angular quantity to compare against.  getAppliedImpulse() returns one
 * combined magnitude, and the 2D sibling (rt_monitor_joints2d) already read
 * both halves, which is what makes this a gap rather than a decision.
 *
 * WHY THE TEST IS AT THE RUNTIME LEVEL.  The physics primitive is only half
 * the fix; the defect was that nothing CALLED it.  A test of
 * jce_physics_constraint_applied_torque alone would have gone green over a
 * runtime that still ignored break_torque entirely.  So the fixture is a
 * scene, the driver is an authored ConstantForce torque, and the observable is
 * rt->cfg_joint_count -- the registry the break sweep compacts.
 *
 * WHAT CASE 1 IS FOR: UNITS, MEASURED RATHER THAN REMEMBERED.  Bullet ships
 * headers only in this package -- there is no solver source in the tree to
 * read -- and whether the backend divides by the timestep before storing joint
 * feedback decides whether the monitor must compare `tq > break_torque` or
 * `tq > break_torque * dt`.  Guessed wrong, the threshold is off by 60x and
 * the symptom is indistinguishable from the original defect: "break_torque
 * still does nothing".  So case 1 reads the raw value under a KNOWN applied
 * torque and brackets it.  The two candidate answers are a factor of dt apart,
 * which is a factor of 60 -- a bracket of [0.25x, 4x] separates them with room
 * to spare, and does not pretend to a precision Bullet's soft 6DOF solver does
 * not have.  If it ever fails low, the fix is one line in rt_cfg_joint_breaks
 * and this comment says which.
 *
 * WHY CASE 2 SETS breakForce TO 0 EXPLICITLY.  That is not decoration, it is
 * the assertion.  The two limits are tested under separate `if`s precisely so
 * an unset break_force cannot silently disable break_torque; nesting them
 * again would still pass with breakForce at its 1e30 scene default, because
 * 1e30 > 0.  Only an explicit 0 makes the nested form fail.
 *
 * WHY CASE 3 IS NOT JUST A CONTROL FOR CASE 2.  It also pins that the feedback
 * is PER STEP.  If the backend accumulated it across steps instead of clearing
 * at each solve, any finite threshold would eventually be crossed and every
 * joint in every scene would break on a long enough run -- which reads as an
 * unrelated stability bug.  300 steps under a torque that stays well under the
 * threshold is what makes that a red rather than a mystery.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */
#include "application/jce_rt_internal.h"

#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static JceScene   *s_scene;
static JceRuntime *s_rt;

void setUp(void)    { s_scene = NULL; s_rt = NULL; }
void tearDown(void)
{
    if (s_rt)    jce_runtime_destroy(s_rt);
    if (s_scene) jce_scene_destroy(s_scene);
    s_rt = NULL; s_scene = NULL;
}

static const float DT  = 1.0f / 60.0f;
/* The authored driving torque, in N*m.  Every threshold below is a multiple of
 * it, so no number in this file is a magic constant that a solver change could
 * quietly invalidate without also moving the reading it is compared to. */
static const float TAU = 50.0f;

static void boot(const char *json)
{
    JceJson *root = jce_json_parse(json, strlen(json));
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "the fixture must parse");
    s_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s_scene);
    TEST_ASSERT_TRUE(jce_scene_load_json(s_scene, root) > 0);
    jce_json_free(root);

    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
}

/* One dynamic box, jointed to the WORLD with every axis locked, twisted by an
 * authored ConstantForce torque about Y.
 *
 * World-anchored (connectedBody omitted -> 0) on purpose: a second body would
 * add its own inertia and its own solver rows to a measurement whose whole
 * job is to be readable.  Anchors are at the body centre (the parse default),
 * so the force holding the box against gravity has no lever arm and
 * contributes ~nothing to the TORQUE channel -- the reading is the twist.
 *
 * Every axis locked -> the joint must resist the full applied torque, so in
 * steady state the constraint torque is the authored TAU.  That is what makes
 * case 1's bracket meaningful: it compares the reading against a number the
 * fixture chose, not against a previous run of itself. */
static const char *const TWISTED_JOINT_FMT =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Twisted\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":4,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"Rigidbody\",\"mass\":1.0,\"isKinematic\":false},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0},"
   "{\"type\":\"ConstantForce\",\"torqueY\":%.1f},"
   "{\"type\":\"ConfigurableJoint\"%s}]}"
"]}";

/* Build the fixture with an explicit break-threshold clause (or none at all,
 * which is what a scene authored before this field existed looks like). */
/* snprintf TRUNCATES SILENTLY, and a truncated fixture is still a string --
 * it just stops being JSON, and the failure surfaces as "the fixture must
 * parse" three frames away from the buffer that caused it.  Asserting on the
 * return value costs one line and turns a future edit that grows the fixture
 * past the buffer into a message that names the real problem. */
#define ASSERT_FIT(n, cap) \
    TEST_ASSERT_TRUE_MESSAGE((n) > 0 && (size_t)(n) < (cap), \
        "the fixture did not fit its buffer -- grow it; a truncated fixture " \
        "fails as a parse error and hides which buffer overflowed")

static void boot_with(const char *break_clause)
{
    char json[4096];
    int n = snprintf(json, sizeof json, TWISTED_JOINT_FMT,
                     (double)TAU, break_clause);
    ASSERT_FIT(n, sizeof json);
    boot(json);
}

static void step(int frames)
{
    for (int i = 0; i < frames; ++i) jce_runtime_step(s_rt, DT);
}

/* ── 1. units ──────────────────────────────────────────────────────────────
 * The reading exists, is finite, and is a TORQUE rather than an angular
 * impulse.  Thresholds high enough never to fire, so the joint survives to be
 * read. */
static void test_the_reading_is_a_torque_not_an_angular_impulse(void)
{
    boot_with(",\"breakForce\":1e30,\"breakTorque\":1e30");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "the fixture must produce exactly one tracked configurable joint -- "
        "without it every assertion below is about an empty registry");

    step(60);

    const JceConstraintHandle h = s_rt->cfg_joints[0].handle;
    TEST_ASSERT_TRUE_MESSAGE(jce_constraint_valid(h),
        "the joint must still be live: a 1e30 threshold is 'unlimited'");

    const float tq = jce_physics_constraint_applied_torque(s_rt->physics, h);
    printf("applied_torque = %.4f N*m under an authored %.1f N*m "
           "(dt-scaled would be ~%.4f)\n",
           (double)tq, (double)TAU, (double)(TAU * DT));

    TEST_ASSERT_TRUE_MESSAGE(isfinite(tq), "the reading must be finite");
    TEST_ASSERT_TRUE_MESSAGE(tq > 0.0f,
        "ZERO HERE IS THE ORIGINAL DEFECT WEARING A NEW HAT: the constraint "
        "has no joint feedback installed, so break_torque has nothing to read "
        "and is inert no matter what the monitor compares");
    TEST_ASSERT_TRUE_MESSAGE(tq > TAU * 0.25f,
        "the reading is far below the torque the joint is resisting, which is "
        "what an ANGULAR IMPULSE (torque*dt, ~1/60th) looks like.  If this is "
        "the failure, the backend does not divide by the timestep and the fix "
        "is ONE line: rt_cfg_joint_breaks must compare "
        "`tq > ce->break_torque * cs->dt`, the way the break_force branch "
        "directly above it already does");
    TEST_ASSERT_TRUE_MESSAGE(tq < TAU * 4.0f,
        "the reading is far above the torque the joint is resisting -- a "
        "quantity divided by dt one time too many, or feedback accumulating "
        "across steps instead of being cleared each solve");
}

/* ── 2. it breaks, and break_force == 0 does not disable it ──────────────── */
static void test_a_torque_past_the_threshold_breaks_the_joint(void)
{
    /* breakForce EXPLICITLY 0 -- see the header.  0 means "unlimited on this
     * axis", so nothing here can break on force; if the joint goes, torque
     * broke it. */
    boot_with(",\"breakForce\":0,\"breakTorque\":12.5");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "the fixture must produce exactly one tracked configurable joint");

    step(120);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_rt->cfg_joint_count,
        "the joint must SNAP: it is resisting ~50 N*m against an authored "
        "12.5 N*m limit.  Surviving is the defect this unit exists for -- "
        "break_torque authored, serialised, tracked, and read by nothing.  It "
        "is also what re-nesting the torque test inside the break_force branch "
        "looks like, because breakForce here is 0");
}

/* ── 3. control: a threshold above the load holds, for 300 steps ─────────── */
static void test_a_torque_under_the_threshold_leaves_it_alone(void)
{
    boot_with(",\"breakForce\":0,\"breakTorque\":400.0");
    TEST_ASSERT_EQUAL_INT(1, s_rt->cfg_joint_count);

    step(300);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "the joint must HOLD: ~50 N*m against a 400 N*m limit.  Breaking here "
        "means either the comparison is inverted, or -- the reason this runs "
        "300 steps rather than a token few -- the joint feedback ACCUMULATES "
        "instead of being cleared at each solve, in which case every joint in "
        "every scene eventually breaks on a long enough run");
}

/* ── 4. an unset threshold is unlimited, not zero ────────────────────────── */
static void test_an_unauthored_threshold_never_breaks(void)
{
    /* No breakForce, no breakTorque: exactly what a scene written before
     * either field existed deserialises to.  The scene parser's default is
     * 1e30, and the monitor's guard is `> 0`, so both must combine to "never".
     * An inverted guard, or a default that arrived as 0 and was then read as a
     * literal threshold, silently destroys every joint in the project on the
     * first tick -- with no message, because breaking is the feature. */
    boot_with("");
    TEST_ASSERT_EQUAL_INT(1, s_rt->cfg_joint_count);

    step(300);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "a joint that authored no break threshold must be unbreakable");
}

/* A TWO-BODY joint: the same twist, but connected to a kinematic anchor
 * instead of to the world.
 *
 * THIS CASE IS HERE BECAUSE IT WOULD HAVE PASSED.  Bullet's single-body
 * constructor takes the body you give it as side B and substitutes its static
 * getFixedBody() for side A; the two-body constructor puts the jointed body on
 * side A.  So a query that reads only m_appliedTorqueBodyA works perfectly
 * here and returns a flat 0.0000 for every world-anchored joint -- and
 * world-anchored is what `connected_body == 0` builds, which is most authored
 * joints in a scene.  A test suite containing only this case would have gone
 * green over a break_torque that never fires in the common configuration.
 *
 * Kinematic rather than a bare collider on purpose: a small STATIC primitive
 * is DEFERRED at spawn by the big-world lazy path and would not exist yet when
 * the post-spawn joint pass resolves connected_body, so the joint would never
 * be created and this case would pass by spawning nothing at all. */
static const char *const TWO_BODY_FMT =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Anchor\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":4,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"Rigidbody\",\"mass\":1.0,\"isKinematic\":true},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]},"
 "{\"name\":\"Twisted\",\"id\":2,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":6,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"Rigidbody\",\"mass\":1.0,\"isKinematic\":false},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0},"
   "{\"type\":\"ConstantForce\",\"torqueY\":%.1f},"
   "{\"type\":\"ConfigurableJoint\",\"connectedBody\":1,"
    "\"breakForce\":0,\"breakTorque\":%.1f}]}"
"]}";

static void boot_two_body(float break_torque)
{
    char json[4096];
    int n = snprintf(json, sizeof json, TWO_BODY_FMT,
                     (double)TAU, (double)break_torque);
    ASSERT_FIT(n, sizeof json);
    boot(json);
}

static void test_a_two_body_joint_breaks_on_torque_too(void)
{
    boot_two_body(12.5f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "the two-body fixture must produce one tracked joint -- if the "
        "connected body had no body yet, the joint was never created and "
        "everything below would pass by measuring nothing");

    step(120);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_rt->cfg_joint_count,
        "a joint connected to another body must break on torque exactly as a "
        "world-anchored one does");
}

static void test_a_two_body_joint_holds_under_its_threshold(void)
{
    boot_two_body(400.0f);
    TEST_ASSERT_EQUAL_INT(1, s_rt->cfg_joint_count);
    step(300);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->cfg_joint_count,
        "...and must hold below it, or the case above proves only that "
        "two-body joints always break");
}

/* ── 5. the query is safe on a handle that is gone ───────────────────────── */
static void test_the_query_is_safe_after_the_joint_is_gone(void)
{
    boot_with(",\"breakForce\":0,\"breakTorque\":12.5");
    const JceConstraintHandle h = s_rt->cfg_joints[0].handle;

    step(120);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_rt->cfg_joint_count,
        "precondition: the joint broke, so the handle is now stale");

    /* The break sweep destroyed it.  Anything still holding the handle -- a
     * script, a debug panel, the next tick of a monitor that was mid-walk --
     * must get 0 rather than read freed feedback storage, which is owned by
     * the constraint registry and reused by the next joint created. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
        jce_physics_constraint_applied_torque(s_rt->physics, h),
        "a destroyed constraint must report 0, not stale feedback");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
        jce_physics_constraint_applied_torque(s_rt->physics,
                                              JCE_CONSTRAINT_INVALID),
        "an invalid handle must report 0");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_reading_is_a_torque_not_an_angular_impulse);
    RUN_TEST(test_a_torque_past_the_threshold_breaks_the_joint);
    RUN_TEST(test_a_torque_under_the_threshold_leaves_it_alone);
    RUN_TEST(test_an_unauthored_threshold_never_breaks);
    RUN_TEST(test_a_two_body_joint_breaks_on_torque_too);
    RUN_TEST(test_a_two_body_joint_holds_under_its_threshold);
    RUN_TEST(test_the_query_is_safe_after_the_joint_is_gone);
    return UNITY_END();
}
