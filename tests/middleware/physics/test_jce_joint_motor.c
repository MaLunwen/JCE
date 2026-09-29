/*
 * test_jce_joint_motor.c — a 3D joint can be DRIVEN.
 *
 * Before this, JCE's 3D constraint surface could limit a joint and it could
 * break one, and it could not power one: no motor speed, no max force, no
 * target, no spring, no damper anywhere in jce_physics.h.  A powered door, an
 * elevator, a turret and a suspension were all inexpressible -- while the 2D
 * side has had hinge motors since jce_physics2d.h:179.  Unity's
 * HingeJoint.motor and ConfigurableJoint drives, Unreal's linear/angular
 * drives and Godot's HingeJoint3D motor all cover this.
 *
 * WHAT EACH CASE HAS TO RULE OUT, because "the body moved" is not "the motor
 * moved it":
 *
 *   1. hinge-motor        an UNPOWERED hinge under the same setup is the
 *                         control.  It must stay still; the powered one must
 *                         turn, in the commanded DIRECTION.
 *   2. hinge-runtime      the same joint, reversed through
 *                         jce_physics_constraint_set_motor while it runs.
 *                         An authoring-only motor gives a door that is
 *                         always opening or never opening, which is not the
 *                         feature.
 *   3. hinge-force-scale  THE UNIT ASSERTION.  Bullet's hinge wants a maximum
 *                         IMPULSE while an author gives a torque, so the
 *                         conversion is a multiply by the fixed step -- and
 *                         getting it wrong yields a motor at 1/60th strength
 *                         that still turns, just weakly.  A motor whose
 *                         max_force is far BELOW what the load needs must
 *                         fail to hold it; the same motor with a large
 *                         max_force must succeed.  Bracketing the two proves
 *                         the number reaching Bullet is within the right
 *                         order of magnitude rather than merely non-zero.
 *   4. slider-motor       the linear half, with its own unpowered control.
 *                         Its strength is a FORCE with no conversion, which
 *                         is the asymmetry case 3 exists to protect.
 *   5. cfg-velocity-drive a configurable joint's per-axis velocity motor on a
 *                         FREE axis, against an undriven control.
 *   6. cfg-spring-drive   a spring pulling to an authored equilibrium: the
 *                         bodies must converge TOWARD the target from the
 *                         wrong side, not merely move.
 *
 * ZERO GRAVITY throughout (mirrors test_jce_configurable_joint), so the only
 * motion is what the joint does.  Tolerances are loose: Bullet is
 * deterministic for identical input on one build and not bit-exact across
 * machines, and every assertion here is about a direction or an order of
 * magnitude rather than a value.
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static const float DT       = 1.0f / 60.0f;
static const float BOX_HALF = 0.5f;
static const float BOX_MASS = 1.0f;

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity        = jce_v3(0.0f, 0.0f, 0.0f);
    wd.max_bodies     = 64u;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

static JceBodyHandle make_box(JcePhysicsWorld *w, jce_vec3 pos)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type            = JCE_BODY_DYNAMIC;
    bd.shape           = JCE_SHAPE_BOX;
    bd.position        = pos;
    bd.rotation        = jce_q_identity();
    bd.half_extents    = jce_v3(BOX_HALF, BOX_HALF, BOX_HALF);
    bd.mass            = BOX_MASS;
    bd.friction        = 0.5f;
    bd.linear_damping  = 0.0f;
    bd.angular_damping = 0.0f;
    bd.collision_group = JCE_COLLISION_DEFAULT_GROUP;
    bd.collision_mask  = JCE_COLLISION_ALL_MASK;
    return jce_physics_body_create(w, &bd);
}

static jce_vec3 body_pos(JcePhysicsWorld *w, JceBodyHandle b)
{
    jce_vec3 p; jce_quat r;
    jce_physics_body_get_transform(w, b, &p, &r);
    return p;
}

/* ANGULAR VELOCITY ABOUT Z, not accumulated angle.
 *
 * The first version of this file read the angle back as 2*atan2(r.z, r.w),
 * which is single-valued only on (-2pi, 2pi].  The control body's true
 * rotation under a 3 N*m load was about -36 rad and came back as +1.9991 --
 * so the control reported "the load never arrived" while the load was
 * arriving perfectly well, and the instrument was the thing that failed.
 *
 * Velocity is what a velocity motor actually commands, it does not wrap, and
 * every assertion in this file was really about it. */
static float body_spin_z(JcePhysicsWorld *w, JceBodyHandle b)
{
    return jce_physics_body_get_angular_velocity(w, b).z;
}

/* A hinge about Z between a world anchor and one dynamic box. */
static JceConstraintHandle make_hinge(JcePhysicsWorld *w, JceBodyHandle b,
                                      bool motor, float target, float max_force)
{
    JceConstraintDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.type    = JCE_CONSTRAINT_HINGE;
    cd.body_a  = b;
    cd.body_b  = JCE_BODY_INVALID;
    cd.pivot_a = jce_v3(0.0f, 0.0f, 0.0f);
    cd.pivot_b = jce_v3(0.0f, 0.0f, 0.0f);
    cd.axis    = jce_v3(0.0f, 0.0f, 1.0f);
    cd.use_motor             = motor;
    cd.motor_target_velocity = target;
    cd.motor_max_force       = max_force;
    return jce_physics_constraint_create(w, &cd);
}

static void spin(JcePhysicsWorld *w, int frames)
{
    for (int i = 0; i < frames; ++i)
        jce_physics_step(w, DT);
}

/* ---------------------------------------------------------------------- */

static void test_a_powered_hinge_turns_and_an_unpowered_one_does_not(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle driven = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle idle   = make_box(w, jce_v3(0.0f, 8.0f, 0.0f));

    /* Same joint, same everything, one switch. */
    JceConstraintHandle dj = make_hinge(w, driven, true,  2.0f, 50.0f);
    JceConstraintHandle ij = make_hinge(w, idle,   false, 2.0f, 50.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(dj));
    TEST_ASSERT_TRUE(jce_constraint_valid(ij));

    spin(w, 120);

    const float a_driven = body_spin_z(w, driven);
    const float a_idle   = body_spin_z(w, idle);
    printf("  driven=%.4f rad/s  idle=%.4f rad/s\n", a_driven, a_idle);

    TEST_ASSERT_TRUE(isfinite(a_driven) && isfinite(a_idle));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(a_idle) < 0.05f,
        "the UNPOWERED hinge turned; something other than the motor is "
        "moving these bodies and the next assertion would mean nothing");
    /* DIRECTION, not just magnitude: a motor wired to the wrong sign turns
     * the door the wrong way and |angle| cannot see it. */
    TEST_ASSERT_TRUE_MESSAGE(a_driven > 0.5f,
        "the POWERED hinge did not turn the commanded way -- the motor is "
        "inert, or its sign is inverted");

    jce_physics_constraint_destroy(w, dj);
    jce_physics_constraint_destroy(w, ij);
    jce_physics_destroy(w);
}

static void test_the_motor_can_be_reversed_while_the_world_runs(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceConstraintHandle j = make_hinge(w, b, true, 2.0f, 50.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    spin(w, 60);
    const float forward = body_spin_z(w, b);
    TEST_ASSERT_TRUE_MESSAGE(forward > 0.3f,
        "the joint never turned in the first place");

    /* A door that opens is a motor whose target changes while the game runs. */
    jce_physics_constraint_set_motor(w, j, true, -2.0f, 50.0f);
    spin(w, 60);
    const float back = body_spin_z(w, b);
    printf("  forward=%.4f rad/s  after reversal=%.4f rad/s\n", forward, back);

    TEST_ASSERT_TRUE_MESSAGE(back < forward - 0.3f,
        "reversing the motor at runtime did not reverse the joint -- the "
        "setter is inert, and the motor is authoring-time only");

    jce_physics_constraint_destroy(w, j);
    jce_physics_destroy(w);
}

static void test_max_force_is_a_torque_not_an_impulse(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* THE UNIT BRACKET.  Both hinges are commanded to the same speed against
     * the same steady opposing torque; they differ only in how much the motor
     * may spend.  If the author's N*m reached Bullet as if it were already an
     * impulse, the weak motor would be ~60x stronger than intended and would
     * hold the load -- so "weak fails, strong wins" brackets the conversion
     * to within the very factor the mistake would introduce. */
    JceBodyHandle weak   = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle strong = make_box(w, jce_v3(0.0f, 8.0f, 0.0f));
    JceConstraintHandle wj = make_hinge(w, weak,   true, 2.0f, 0.05f);
    JceConstraintHandle sj = make_hinge(w, strong, true, 2.0f, 50.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(wj));
    TEST_ASSERT_TRUE(jce_constraint_valid(sj));

    /* THE CONTROL THAT MAKES THE BRACKET READABLE.  An unjointed body under
     * the same load: if IT does not spin the wrong way, the load never
     * reached anything and the two hinge results below are about the motor
     * alone.  Without this the case cannot tell "the weak motor is too
     * strong" from "the opposing torque was never applied". */
    JceBodyHandle freebody = make_box(w, jce_v3(0.0f, 16.0f, 0.0f));

    const jce_vec3 load = jce_v3(0.0f, 0.0f, -3.0f);   /* N*m, opposing */
    for (int i = 0; i < 120; ++i) {
        jce_physics_body_apply_torque(w, weak,     load);
        jce_physics_body_apply_torque(w, strong,   load);
        jce_physics_body_apply_torque(w, freebody, load);
        jce_physics_step(w, DT);
    }

    const float a_free = body_spin_z(w, freebody);
    printf("  free body under the same load = %.4f rad/s\n", a_free);
    TEST_ASSERT_TRUE_MESSAGE(a_free < -0.5f,
        "the CONTROL body did not spin under the opposing torque, so the "
        "load never arrived and nothing below is about the motor");

    const float a_weak   = body_spin_z(w, weak);
    const float a_strong = body_spin_z(w, strong);
    printf("  weak(max 0.05 N*m)=%.4f rad/s  strong(max 50 N*m)=%.4f rad/s  load=3 N*m\n",
           a_weak, a_strong);

    TEST_ASSERT_TRUE(isfinite(a_weak) && isfinite(a_strong));
    TEST_ASSERT_TRUE_MESSAGE(a_strong > 0.5f,
        "a 50 N*m motor could not drive against a 3 N*m load");
    TEST_ASSERT_TRUE_MESSAGE(a_weak < 0.0f,
        "a 0.05 N*m motor held a 3 N*m load -- max_force is reaching the "
        "solver far larger than the author asked for, which is what happens "
        "when a torque is passed where an impulse is expected");

    jce_physics_constraint_destroy(w, wj);
    jce_physics_constraint_destroy(w, sj);
    jce_physics_destroy(w);
}

static void test_a_powered_slider_travels_and_an_unpowered_one_does_not(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle driven = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle idle   = make_box(w, jce_v3(0.0f, 8.0f, 0.0f));

    JceConstraintDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.type        = JCE_CONSTRAINT_SLIDER;
    cd.body_b      = JCE_BODY_INVALID;
    cd.axis        = jce_v3(1.0f, 0.0f, 0.0f);
    cd.lower_limit = -10.0f;
    cd.upper_limit =  10.0f;

    cd.body_a                = driven;
    cd.use_motor             = true;
    cd.motor_target_velocity = 1.5f;      /* m/s */
    cd.motor_max_force       = 100.0f;    /* N, no conversion on this path */
    JceConstraintHandle dj = jce_physics_constraint_create(w, &cd);

    cd.body_a    = idle;
    cd.use_motor = false;
    JceConstraintHandle ij = jce_physics_constraint_create(w, &cd);

    TEST_ASSERT_TRUE(jce_constraint_valid(dj));
    TEST_ASSERT_TRUE(jce_constraint_valid(ij));

    const jce_vec3 p0_driven = body_pos(w, driven);
    const jce_vec3 p0_idle   = body_pos(w, idle);
    spin(w, 120);
    const float d_driven = body_pos(w, driven).x - p0_driven.x;
    const float d_idle   = body_pos(w, idle).x   - p0_idle.x;
    printf("  slider driven dx=%.4f  idle dx=%.4f\n", d_driven, d_idle);

    TEST_ASSERT_TRUE(isfinite(d_driven) && isfinite(d_idle));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(d_idle) < 0.05f,
        "the UNPOWERED slider moved on its own");
    TEST_ASSERT_TRUE_MESSAGE(d_driven > 0.5f,
        "the POWERED slider did not travel along its axis");

    jce_physics_constraint_destroy(w, dj);
    jce_physics_constraint_destroy(w, ij);
    jce_physics_destroy(w);
}

/* A configurable joint between two boxes, X free, everything else locked. */
static JceConstraintHandle make_cfg(JcePhysicsWorld *w,
                                    JceBodyHandle a, JceBodyHandle b,
                                    int mode, float target,
                                    float spring, float damper, float max_force)
{
    JceConfigurableJointDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.body_a = a;
    jd.body_b = b;
    /* The two boxes are built coincident so the joint's own axis is the only
     * thing that separates them -- which means CONTACT would otherwise do it
     * first.  Measured without this: both the driven and the undriven pair
     * flew to a separation of 23.8, identically, and the drive was invisible
     * underneath the collision response. */
    jd.disable_collision = true;
    jd.lin_motion[0] = 2;   /* FREE  */
    jd.lin_motion[1] = 0;
    jd.lin_motion[2] = 0;
    jd.ang_motion[0] = 0;
    jd.ang_motion[1] = 0;
    jd.ang_motion[2] = 0;
    jd.drive_mode[0]      = mode;
    jd.drive_target[0]    = target;
    jd.drive_spring[0]    = spring;
    jd.drive_damper[0]    = damper;
    jd.drive_max_force[0] = max_force;
    return jce_physics_configurable_joint_create(w, &jd);
}

static void test_a_configurable_joint_axis_can_be_driven_at_a_velocity(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle da = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle db = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle ia = make_box(w, jce_v3(0.0f, 8.0f, 0.0f));
    JceBodyHandle ib = make_box(w, jce_v3(0.0f, 8.0f, 0.0f));

    JceConstraintHandle dj = make_cfg(w, da, db, 1 /*VELOCITY*/, 1.0f,
                                      0.0f, 0.0f, 100.0f);
    JceConstraintHandle ij = make_cfg(w, ia, ib, 0 /*OFF*/, 1.0f,
                                      0.0f, 0.0f, 100.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(dj));
    TEST_ASSERT_TRUE(jce_constraint_valid(ij));

    spin(w, 120);

    const float d_sep = fabsf(body_pos(w, db).x - body_pos(w, da).x);
    const float i_sep = fabsf(body_pos(w, ib).x - body_pos(w, ia).x);
    printf("  cfg velocity-driven sep=%.4f  undriven sep=%.4f\n", d_sep, i_sep);

    TEST_ASSERT_TRUE(isfinite(d_sep) && isfinite(i_sep));
    TEST_ASSERT_TRUE_MESSAGE(i_sep < 0.05f,
        "the UNDRIVEN pair separated on its own -- a free axis with no drive "
        "should sit still in zero gravity");
    TEST_ASSERT_TRUE_MESSAGE(d_sep > 0.5f,
        "the velocity drive did not move the free axis");

    jce_physics_constraint_destroy(w, dj);
    jce_physics_constraint_destroy(w, ij);
    jce_physics_destroy(w);
}

static void test_a_spring_drive_pulls_toward_its_authored_target(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* ZERO IS WHERE THEY START, not the world origin.  The create path
     * auto-configures the connected frame to the joint's world pose at spawn
     * (Unity's autoConfigureConnectedAnchor default), so the axis position
     * the spring measures begins at 0 whatever the bodies' coordinates are.
     * An earlier version of this case started them 4 apart and set the
     * equilibrium to 0, which is the one value that asks the spring to hold
     * still -- and `4.0000 -> 4.0000` was it doing exactly that, correctly.
     *
     * So: start coincident and pull to a NAMED non-zero offset.  Converging
     * on that value is the property; merely moving is not, since any push
     * moves them. */
    const float TARGET = 2.0f;
    JceBodyHandle a = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle b = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceConstraintHandle j = make_cfg(w, a, b, 2 /*SPRING*/, TARGET,
                                     40.0f, 4.0f, 0.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    const float sep0 = fabsf(body_pos(w, b).x - body_pos(w, a).x);
    spin(w, 600);
    const float sep1 = fabsf(body_pos(w, b).x - body_pos(w, a).x);
    printf("  spring start sep=%.4f  after=%.4f  target=%.4f\n",
           sep0, sep1, TARGET);

    TEST_ASSERT_TRUE(isfinite(sep1));
    TEST_ASSERT_TRUE_MESSAGE(sep0 < 0.05f, "the pair did not start coincident");
    /* CONVERGENCE ON THE AUTHORED VALUE, not just motion.  A velocity motor
     * would keep going; a spring stops where it was told to. */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(sep1 - TARGET) < 0.5f,
        "the spring did not settle at its authored equilibrium");

    jce_physics_constraint_destroy(w, j);
    jce_physics_destroy(w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_powered_hinge_turns_and_an_unpowered_one_does_not);
    RUN_TEST(test_the_motor_can_be_reversed_while_the_world_runs);
    RUN_TEST(test_max_force_is_a_torque_not_an_impulse);
    RUN_TEST(test_a_powered_slider_travels_and_an_unpowered_one_does_not);
    RUN_TEST(test_a_configurable_joint_axis_can_be_driven_at_a_velocity);
    RUN_TEST(test_a_spring_drive_pulls_toward_its_authored_target);
    return UNITY_END();
}
