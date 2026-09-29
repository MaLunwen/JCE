/*
 * test_jce_character_platform.c
 *
 * A character standing on a moving platform must be carried by it.
 *
 * Until this change it was not, and the reason was that BOTH mechanisms a
 * physics engine normally has for this were absent at once:
 *
 *   - the capsule is created with m_friction = 0 on purpose ("horizontal is
 *     velocity-driven"), so nothing drags it along the surface, and
 *   - jce_bullet_character_move recomputes the horizontal velocity
 *     ABSOLUTELY every tick from walk_dir alone, so even the momentum the
 *     solver did impart was overwritten before it could move anything.
 *
 * The ground probe stored a normal and four booleans and no ground BODY, so
 * nothing downstream could have recovered the surface's velocity either.
 *
 * WHAT MAKES THESE CASES A MATCHED SET.  The carry is now a term added to the
 * commanded velocity, and a term added to a velocity is indistinguishable
 * from a drift unless something pins down that it is ZERO when it should be.
 * So B is not decoration: it is the only case that separates "rides a moving
 * platform" from "slides in the +x direction for unrelated reasons".
 *
 * P runs FIRST and asserts the instrument, not the subject.  The platform's
 * velocity is DERIVED by Bullet for a kinematic body -- saveKinematicState
 * computes it once per step from the transform delta -- and if that chain
 * does not hold, every later case fails for a reason that has nothing to do
 * with the character.  A test whose failure cannot be attributed is worth
 * less than no test.
 *
 * C is the case that earns getVelocityInLocalPoint over getLinearVelocity.
 * A platform spinning about its own centre has ZERO linear velocity, so an
 * implementation that reads the body velocity passes A, B and D and fails
 * only here.  That is the plausible wrong implementation, and this is the
 * only case that can see it.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_math.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define DT           (1.0f / 60.0f)
#define CAPSULE_H    1.8f
#define CAPSULE_R    0.3f
#define START_Y      2.0f

/* Platform top face at y = 0; the capsule centre settles near half its height. */
#define PLATFORM_HALF_Y  1.0f
#define PLATFORM_Y      (-PLATFORM_HALF_Y)

/* Metres per second the translating platform moves along +x. */
#define PLATFORM_SPEED   2.0f
/* Radians per second the spinning platform turns about +y. */
#define PLATFORM_OMEGA   1.5f

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    wd.max_bodies     = 64;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

static JceBodyHandle make_platform(JcePhysicsWorld *w, JceBodyType type)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type         = type;
    bd.shape        = JCE_SHAPE_BOX;
    bd.position     = jce_v3(0.0f, PLATFORM_Y, 0.0f);
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(20.0f, PLATFORM_HALF_Y, 20.0f);
    bd.mass         = 0.0f;
    /* Deliberately HIGH.  If friction were what carried the character this
     * would be the mechanism doing it, and the capsule's own m_friction = 0
     * is what makes that impossible -- so a pass here cannot be friction. */
    bd.friction     = 1.0f;

    JceBodyHandle b = jce_physics_body_create(w, &bd);
    TEST_ASSERT_TRUE(jce_body_valid(b));
    return b;
}

static JceCharacterHandle make_character_at(JcePhysicsWorld *w, float x, float y)
{
    JceCharacterDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.position      = jce_v3(x, y, 0.0f);
    cd.radius        = CAPSULE_R;
    cd.height        = CAPSULE_H;
    cd.step_height   = 0.35f;
    cd.max_slope_deg = 50.0f;
    cd.gravity       = 9.81f;
    cd.jump_speed    = 5.0f;

    JceCharacterHandle ch = jce_physics_character_create(w, &cd);
    TEST_ASSERT_TRUE(jce_character_valid(ch));
    return ch;
}

static JceCharacterHandle make_character(JcePhysicsWorld *w, float x)
{
    return make_character_at(w, x, START_Y);
}

/* Drop the character onto the platform and let it settle, platform still. */
static void settle(JcePhysicsWorld *w, JceCharacterHandle ch)
{
    for (int i = 0; i < 120; ++i) {
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    TEST_ASSERT_TRUE_MESSAGE(
        jce_physics_character_is_grounded(w, ch),
        "character never landed on the platform -- the rig is wrong, so "
        "nothing this file asserts about carrying means anything");
}

static float char_x(JcePhysicsWorld *w, JceCharacterHandle ch)
{
    jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
    jce_physics_character_get_position(w, ch, &p);
    return p.x;
}

static float char_z(JcePhysicsWorld *w, JceCharacterHandle ch)
{
    jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
    jce_physics_character_get_position(w, ch, &p);
    return p.z;
}

/* ── P: the instrument.  Does Bullet derive a velocity for a kinematic body
 *      that is moved by setting its transform?  Everything else depends on
 *      this, and it is not something this change controls. ──────────────── */
static void test_kinematic_platform_reports_its_velocity(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle plat = make_platform(w, JCE_BODY_KINEMATIC);

    float x = 0.0f;
    for (int i = 0; i < 10; ++i) {
        x += PLATFORM_SPEED * DT;
        jce_physics_body_set_transform(w, plat, jce_v3(x, PLATFORM_Y, 0.0f),
                                       jce_q_identity());
        jce_physics_step(w, DT);
    }
    jce_vec3 v = jce_physics_body_get_velocity(w, plat);
    jce_physics_destroy(w);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.3f, PLATFORM_SPEED, v.x,
        "a kinematic body moved by set_transform reports no velocity -- the "
        "character carry reads exactly this value, so every other case in "
        "this file would fail for a reason unrelated to the character");
}

/* ── A: the regression.  Translating platform carries a still character. ── */
static void test_character_rides_a_translating_platform(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle      plat = make_platform(w, JCE_BODY_KINEMATIC);
    JceCharacterHandle ch   = make_character(w, 0.0f);
    settle(w, ch);

    const float x0 = char_x(w, ch);
    float px = 0.0f;
    for (int i = 0; i < 60; ++i) {           /* 1.0 s */
        px += PLATFORM_SPEED * DT;
        jce_physics_body_set_transform(w, plat, jce_v3(px, PLATFORM_Y, 0.0f),
                                       jce_q_identity());
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    const float moved = char_x(w, ch) - x0;
    jce_physics_destroy(w);

    /* The carry ACCELERATES toward the platform velocity at `accel`, so the
     * first few ticks lag; a 25% window covers that ramp without being loose
     * enough to accept a character that merely drifted. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.25f * PLATFORM_SPEED, PLATFORM_SPEED, moved,
        "character was not carried by a moving platform -- the capsule is "
        "frictionless by design and its horizontal velocity is recomputed "
        "absolutely each tick, so the platform's velocity is the only thing "
        "that could move it");
}

/* ── B: negative control.  A STATIC platform must carry nothing. ────────── */
static void test_character_does_not_drift_on_a_static_platform(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    make_platform(w, JCE_BODY_STATIC);
    JceCharacterHandle ch = make_character(w, 0.0f);
    settle(w, ch);

    const float x0 = char_x(w, ch);
    for (int i = 0; i < 60; ++i) {
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    const float moved = char_x(w, ch) - x0;
    jce_physics_destroy(w);

    /* Without this, "the character moved +2 m in x" is satisfied by any bug
     * that adds a constant to the commanded velocity. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.01f, 0.0f, moved,
        "character drifted while standing still on a STATIC platform -- the "
        "carry term is not zero when the surface is not moving");
}

/* ── C: rotation.  A platform spinning about its own centre has ZERO linear
 *      velocity, so only a contact-point velocity can carry a rider. ───── */
static void test_character_rides_a_rotating_platform(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle plat = make_platform(w, JCE_BODY_KINEMATIC);

    /* Off the axis, so there is a tangential velocity to be carried by. */
    const float r  = 3.0f;
    JceCharacterHandle ch = make_character(w, r);
    settle(w, ch);

    const float z0 = char_z(w, ch);
    float ang = 0.0f;
    for (int i = 0; i < 30; ++i) {           /* 0.5 s */
        ang += PLATFORM_OMEGA * DT;
        jce_physics_body_set_transform(
            w, plat, jce_v3(0.0f, PLATFORM_Y, 0.0f),
            jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), ang));
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    const float moved_z = char_z(w, ch) - z0;
    jce_physics_destroy(w);

    /* A rider at +x on a platform turning +omega about +y moves toward -z in
     * a right-handed frame.  Only the SIGN and a floor on the magnitude are
     * asserted: the rider leaves the tangent as soon as it starts moving, so
     * the exact displacement is not omega*r*t and pinning it would be
     * asserting an integration scheme rather than the behaviour. */
    TEST_ASSERT_TRUE_MESSAGE(
        moved_z < -0.10f,
        "character was not carried by a ROTATING platform -- a platform "
        "spinning about its own centre has zero linear velocity, so this is "
        "what separates a contact-point velocity from a body velocity");
}

/* ── D: leaving.  The carry must stop when the feet leave the surface. ─── */
static void test_carry_stops_when_the_character_leaves_the_platform(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle      plat = make_platform(w, JCE_BODY_KINEMATIC);
    JceCharacterHandle ch   = make_character(w, 0.0f);
    settle(w, ch);

    /* Ride for half a second so the capsule is at platform speed. */
    float px = 0.0f;
    for (int i = 0; i < 30; ++i) {
        px += PLATFORM_SPEED * DT;
        jce_physics_body_set_transform(w, plat, jce_v3(px, PLATFORM_Y, 0.0f),
                                       jce_q_identity());
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }

    /* Teleport clear of the platform and STOP moving it.  The probe loses the
     * body, so the carry term must go to zero. */
    jce_physics_character_set_position(w, ch, jce_v3(100.0f, 20.0f, 0.0f));
    for (int i = 0; i < 30; ++i) {
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    jce_vec3 v = jce_v3(0.0f, 0.0f, 0.0f);
    jce_physics_character_get_velocity(w, ch, &v);
    jce_physics_destroy(w);

    /* DOCUMENTED BEHAVIOUR, asserted so the comment and the code cannot
     * disagree: the inherited speed is NOT conserved.  Airborne the capsule
     * decelerates toward the bare walk command at accel*air_control, so after
     * half a second off the platform the horizontal velocity has bled away.
     * If someone later implements impart-on-leave, this is the case that
     * tells them they changed a behaviour rather than fixed one. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.5f, 0.0f, v.x,
        "the platform's velocity is still being applied after the character "
        "left it -- the carry is not gated on the ground probe");
}

/* ── E: the carry is gated on GROUNDED, not on merely touching something.
 *
 * A face too steep to walk on supports no locomotion, so it must impart none
 * either -- the same rule the max-slope strip already applies to the command.
 * The distinction is invisible in every case above, because there `touching`
 * and `grounded` are both true: a mutation replacing `grounded` with "any
 * hit" SURVIVED A, B, C and D.  This case is the one that kills it.
 *
 * WHY THE RIG IS ORTHOGONAL.  The platform is tilted about +x, so its normal
 * leans into z and the capsule slides along z; it is translated along +x, so
 * the carry -- if wrongly applied -- shows up in x.  The uncontrolled motion
 * and the measured axis are perpendicular, which is what makes a tight
 * threshold on x honest while the character is sliding the whole time. ──── */
static void test_a_too_steep_moving_face_carries_nothing(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type         = JCE_BODY_KINEMATIC;
    bd.shape        = JCE_SHAPE_BOX;
    bd.position     = jce_v3(0.0f, PLATFORM_Y, 0.0f);
    /* 60 deg > the 50 deg max slope, so normal.y = 0.5 < cos(50 deg). */
    bd.rotation     = jce_q_from_axis_angle(jce_v3(1.0f, 0.0f, 0.0f), 1.0472f);
    bd.half_extents = jce_v3(20.0f, PLATFORM_HALF_Y, 20.0f);
    bd.mass         = 0.0f;
    bd.friction     = 1.0f;
    JceBodyHandle plat = jce_physics_body_create(w, &bd);
    TEST_ASSERT_TRUE(jce_body_valid(plat));

    /* The tilted face sits at y = 1.0 directly above the centre. */
    JceCharacterHandle ch = make_character_at(w, 0.0f, 3.0f);
    for (int i = 0; i < 60; ++i) {
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }

    /* Self-validation: if this ever reads grounded, the rig stopped testing
     * what it claims and the x assertion below would pass for free. */
    TEST_ASSERT_FALSE_MESSAGE(
        jce_physics_character_is_grounded(w, ch),
        "the character is GROUNDED on a 60-degree face -- the rig is not "
        "producing the touching-but-not-grounded state this case needs");

    const float x0 = char_x(w, ch);
    float px = 0.0f;
    for (int i = 0; i < 60; ++i) {
        px += PLATFORM_SPEED * DT;
        jce_physics_body_set_transform(
            w, plat, jce_v3(px, PLATFORM_Y, 0.0f), bd.rotation);
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), DT);
        jce_physics_step(w, DT);
    }
    const float moved_x = char_x(w, ch) - x0;
    jce_physics_destroy(w);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.30f, 0.0f, moved_x,
        "a face too steep to stand on dragged the character along with it -- "
        "the carry is gated on touching something rather than on standing "
        "on it");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_kinematic_platform_reports_its_velocity);
    RUN_TEST(test_character_rides_a_translating_platform);
    RUN_TEST(test_character_does_not_drift_on_a_static_platform);
    RUN_TEST(test_character_rides_a_rotating_platform);
    RUN_TEST(test_carry_stops_when_the_character_leaves_the_platform);
    RUN_TEST(test_a_too_steep_moving_face_carries_nothing);
    return UNITY_END();
}
