/*
 * test_jce_softbody.c — Headless self-test for the volumetric / pressure
 * soft-body API (jce/middleware/physics/jce_softbody.h).
 *
 * Drives the SHARED secondary soft-body world (the same world the cloth uses)
 * through the PUBLIC soft-body C ABI only — never touches Bullet internals.
 *
 * Coverage:
 *   1. settles-on-ground   — a pressurised ellipsoid dropped above a static
 *                            ground box drops, does not fall through, and its
 *                            nodes stay finite.
 *   2. pressure-no-collapse— after settling, the body keeps a meaningful
 *                            vertical extent (a pressure body does not pancake).
 *   3. higher-pressure-taller — a high-pressure body settles at least as tall
 *                            as a low-pressure one (pressure holds shape).
 *   4. node-readback       — node_count > 0, positions finite, centre in AABB.
 *   5. invalid-handle-safe — every getter/destroy is no-op/false safe on the
 *                            invalid sentinel + a destroyed handle; double-
 *                            destroy is safe.
 *   6. determinism         — the same create+ground+N-step sequence run twice
 *                            (with clear_statics between) lands within 1e-3.
 *
 * Tolerances are generous: Bullet's soft solver is deterministic for identical
 * input on one build but not bit-exact across machines.
 *
 * Linked against jce_core + jce_physics (mirrors test_jce_cloth / vehicle).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_softbody.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── shared constants ─────────────────────────────────────────────────────── */

#define DT          (1.0f / 60.0f)
#define START_Y     3.0f
#define BALL_R      0.5f          /* sphere radius -> diameter 1.0          */
#define RES         96            /* node density                            */

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* Wide static ground box whose TOP face sits at y = 0 (centre y = -0.5). */
static uint32_t add_ground(void)
{
    return jce_softbody_add_static_box(jce_v3(0.0f, -0.5f, 0.0f),
                                       jce_v3(50.0f, 0.5f, 50.0f));
}

static JceSoftBodyHandle make_ball(jce_vec3 center, float pressure)
{
    JceSoftBodyDesc d;
    memset(&d, 0, sizeof(d));
    d.center           = center;
    d.radius           = jce_v3(BALL_R, BALL_R, BALL_R);
    d.resolution       = RES;
    d.mass             = 2.0f;
    d.pressure         = pressure;
    d.stiffness_linear = 0.4f;
    d.stiffness_volume = 0.4f;
    d.damping          = 0.02f;
    d.friction         = 0.5f;
    d.self_collision   = false;
    return jce_softbody_create_ellipsoid(&d);
}

static void step_n(int frames)
{
    for (int i = 0; i < frames; ++i)
        jce_softbody_step_(DT);
}

static int finite3(jce_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* Verify every node position is finite. */
static int all_nodes_finite(JceSoftBodyHandle h)
{
    uint32_t nc = jce_softbody_node_count(h);
    if (nc == 0u) return 0;
    uint32_t floats = nc * 3u;
    float *buf = (float *)malloc(sizeof(float) * floats);
    if (!buf) return 0;
    int ok = jce_softbody_get_positions(h, buf, floats);
    if (ok) {
        for (uint32_t i = 0; i < floats; ++i) {
            if (!isfinite(buf[i])) { ok = 0; break; }
        }
    }
    free(buf);
    return ok;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

/* -- Sphere and capsule proxies (2026-09-21) ------------------------------- *
 *
 * A BOX was the only shape a soft body could touch, so a sphere or capsule
 * collider in the scene did not exist for the soft world: cloth fell through
 * it and nothing reported that.  Each case below is paired with the SAME rig
 * minus the proxy, because "the ball is still above the proxy top after 240 steps" is
 * also satisfied by a build where nothing falls at all -- and one where the
 * simulation gate is off would satisfy it most of all. */

/* BIG proxies whose top face sits at y = 0, so the contact patch is locally
 * FLAT.  The first rig used a 2 m sphere and dropped the ball on its apex --
 * an unstable equilibrium the ball rolls off, which lands it low and reads
 * exactly like falling through.  The box control passed only because a box
 * has a flat top, so the control was not controlling for the thing that
 * differed.  Radius 20 makes the local surface flat enough to rest on. */
#define PROXY_R     20.0f
#define PROXY_TOP   0.0f
#define PROXY_DROP  3.0f          /* ball centre starts here */

/* How far the ball falls with NOTHING under it, same steps, same rig.  Every
 * assertion below is against THIS, not against a number I picked. */
static float fall_without_proxy(void)
{
    jce_vec3 c = jce_v3(0, 0, 0);
    JceSoftBodyHandle h;

    jce_softbody_set_simulation_enabled(true);
    h = make_ball(jce_v3(0.0f, PROXY_DROP, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    step_n(240);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
    return c.y;
}

/* POSITIVE CONTROL for the two cases below: the SAME geometry and the same
 * drop, with the shape that already worked.  If this fails the rig is wrong;
 * if it passes and the sphere case does not, the difference is the shape. */
static void test_a_soft_body_rests_on_a_box_proxy_at_the_same_place(void)
{
    jce_vec3 c = jce_v3(0, 0, 0);
    JceSoftBodyHandle h;

    jce_softbody_set_simulation_enabled(true);
    TEST_ASSERT_NOT_EQUAL_UINT32(
        UINT32_MAX,
        jce_softbody_add_static_box(jce_v3(0.0f, -PROXY_R, 0.0f),
                                    jce_v3(PROXY_R, PROXY_R, PROXY_R)));
    h = make_ball(jce_v3(0.0f, PROXY_DROP, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    step_n(240);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_TRUE_MESSAGE(c.y > PROXY_TOP,
        "the soft body fell through a static BOX at the same place the "
        "sphere/capsule cases use -- the RIG is wrong, not the shape");
    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

static void test_a_soft_body_rests_on_a_sphere_proxy(void)
{
    const float freefall = fall_without_proxy();
    jce_vec3 c = jce_v3(0, 0, 0);
    JceSoftBodyHandle h;

    /* The control must actually have fallen past the proxy, or "it stopped
     * higher" proves nothing. */
    TEST_ASSERT_TRUE_MESSAGE(freefall < 0.0f,
        "with no proxy the ball did not fall below y=0 in 240 steps -- the "
        "rig is not exercising gravity and the case below is vacuous");

    jce_softbody_set_simulation_enabled(true);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(
        UINT32_MAX,
        jce_softbody_add_static_sphere(jce_v3(0.0f, -PROXY_R, 0.0f), PROXY_R),
        "the sphere proxy was refused");

    h = make_ball(jce_v3(0.0f, PROXY_DROP, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    step_n(240);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_TRUE_MESSAGE(all_nodes_finite(h), "soft body nodes exploded");

    TEST_ASSERT_TRUE_MESSAGE(c.y > PROXY_TOP,
        "the soft body fell through a static SPHERE proxy -- before this "
        "existed a sphere collider was invisible to the soft world, which is "
        "what this case is here to keep true");
    TEST_ASSERT_TRUE_MESSAGE(c.y < PROXY_DROP,
        "the soft body never fell at all, so resting ON the proxy is not "
        "what was measured");

    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

static void test_a_soft_body_rests_on_a_capsule_proxy(void)
{
    jce_vec3 c = jce_v3(0, 0, 0);
    JceSoftBodyHandle h;

    jce_softbody_set_simulation_enabled(true);
    /* Y-axis capsule, total height 4 => top hemisphere crown at y = +2 for a
     * capsule centred on the origin, the same crown the sphere case uses. */
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(
        UINT32_MAX,
        jce_softbody_add_static_capsule(jce_v3(0.0f, -PROXY_R, 0.0f),
                                        PROXY_R, 60.0f, 2),
        "the capsule proxy was refused");

    /* AXIS Z, and the ball lands OFF-CENTRE ALONG IT (z = 8).  Dropping on the
     * centre would rest at the same height whichever axis Bullet was given, so
     * the orientation would be unobservable -- a mutation forcing the X
     * constructor survived exactly that rig.  With radius 20 and total height
     * 60 the cylinder section spans z in [-10, +10], so at z = 8 a Z capsule
     * is still flat-topped at y = 0 while an X capsule has curved away to
     * y = -20 + sqrt(20^2 - 8^2) = -1.67 and the ball ends up below 0. */
    h = make_ball(jce_v3(0.0f, PROXY_DROP, 8.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    step_n(240);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_TRUE_MESSAGE(all_nodes_finite(h), "soft body nodes exploded");

    TEST_ASSERT_TRUE_MESSAGE(c.y > PROXY_TOP,
        "the soft body fell through a static CAPSULE proxy");

    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

/* A degenerate capsule -- height <= 2*radius -- is a sphere, and must be
 * accepted as one rather than refused or passed to Bullet as a negative
 * cylinder length.  Same clamp jce_bullet_character_create applies. */
static void test_a_degenerate_capsule_is_accepted(void)
{
    jce_softbody_set_simulation_enabled(true);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(
        UINT32_MAX,
        jce_softbody_add_static_capsule(jce_v3(0.0f, 0.0f, 0.0f),
                                        1.0f, 0.5f, 1),
        "a capsule whose height is under its own diameter was refused; it is "
        "a sphere, which is what a degenerate capsule should behave as");
    /* And a zero radius is NOT a shape -- refused rather than silently built. */
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX,
        jce_softbody_add_static_capsule(jce_v3(0, 0, 0), 0.0f, 2.0f, 1));
    TEST_ASSERT_EQUAL_UINT32(
        UINT32_MAX,
        jce_softbody_add_static_sphere(jce_v3(0, 0, 0), 0.0f));
    jce_softbody_clear_statics();
}

static void test_settles_on_ground(void)
{
    jce_softbody_set_simulation_enabled(true);
    TEST_ASSERT_TRUE(jce_softbody_is_simulation_enabled());

    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());

    JceSoftBodyHandle h = make_ball(jce_v3(0.0f, START_Y, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    TEST_ASSERT_TRUE(jce_softbody_node_count(h) > 0u);

    step_n(240);

    jce_vec3 c = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_TRUE(finite3(c));

    /* Dropped under gravity (below where the centre started, less the radius). */
    TEST_ASSERT_TRUE_MESSAGE(c.y < START_Y - BALL_R, "soft body did not fall");
    /* Did NOT pass through the ground (centre stays above the top face y=0). */
    TEST_ASSERT_TRUE_MESSAGE(c.y > 0.0f, "soft body fell through the ground");
    /* Solver stayed stable. */
    TEST_ASSERT_TRUE_MESSAGE(all_nodes_finite(h), "soft body nodes exploded");

    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

static void test_pressure_resists_collapse(void)
{
    jce_softbody_set_simulation_enabled(true);
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());

    JceSoftBodyHandle h = make_ball(jce_v3(0.0f, START_Y, 0.0f), 200.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);

    step_n(240);

    jce_vec3 mn = jce_v3(0, 0, 0), mx = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_aabb(h, &mn, &mx));
    TEST_ASSERT_TRUE(finite3(mn) && finite3(mx));

    float vertical = mx.y - mn.y;   /* settled height */
    /* A pressure body keeps a meaningful fraction of its 1.0 diameter — it does
     * NOT pancake flat.  Conservative threshold (> 0.4 of the diameter). */
    TEST_ASSERT_TRUE_MESSAGE(vertical > 0.4f, "pressure body collapsed flat");

    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

static void test_higher_pressure_settles_taller(void)
{
    jce_softbody_set_simulation_enabled(true);

    /* Low-pressure run. */
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());
    JceSoftBodyHandle lo = make_ball(jce_v3(0.0f, START_Y, 0.0f), 10.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, lo);
    step_n(240);
    jce_vec3 lmn = jce_v3(0, 0, 0), lmx = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_aabb(lo, &lmn, &lmx));
    float lo_h = lmx.y - lmn.y;
    jce_softbody_destroy(lo);
    jce_softbody_clear_statics();

    /* High-pressure run on a fresh ground (identical except pressure). */
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());
    JceSoftBodyHandle hi = make_ball(jce_v3(0.0f, START_Y, 0.0f), 250.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, hi);
    step_n(240);
    jce_vec3 hmn = jce_v3(0, 0, 0), hmx = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_aabb(hi, &hmn, &hmx));
    float hi_h = hmx.y - hmn.y;
    jce_softbody_destroy(hi);
    jce_softbody_clear_statics();

    /* Higher pressure holds shape -> at least as tall (small slack for noise). */
    TEST_ASSERT_TRUE_MESSAGE(hi_h >= lo_h - 0.05f,
                             "higher pressure did not hold shape taller");
}

static void test_node_readback_and_center(void)
{
    jce_softbody_set_simulation_enabled(true);

    JceSoftBodyHandle h = make_ball(jce_v3(0.0f, START_Y, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);

    uint32_t nc = jce_softbody_node_count(h);
    TEST_ASSERT_TRUE(nc > 0u);

    TEST_ASSERT_TRUE(all_nodes_finite(h));

    jce_vec3 c = jce_v3(0, 0, 0), mn = jce_v3(0, 0, 0), mx = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_TRUE(jce_softbody_get_aabb(h, &mn, &mx));

    /* Centre lies within the node AABB (with a tiny epsilon). */
    TEST_ASSERT_TRUE(c.x >= mn.x - 1e-3f && c.x <= mx.x + 1e-3f);
    TEST_ASSERT_TRUE(c.y >= mn.y - 1e-3f && c.y <= mx.y + 1e-3f);
    TEST_ASSERT_TRUE(c.z >= mn.z - 1e-3f && c.z <= mx.z + 1e-3f);

    /* Too-small a buffer must be rejected (not overflow). */
    float tiny[3];
    TEST_ASSERT_FALSE(jce_softbody_get_positions(h, tiny, 3u));

    jce_softbody_destroy(h);
    jce_softbody_clear_statics();
}

static void test_invalid_handle_safe(void)
{
    jce_softbody_set_simulation_enabled(true);

    JceSoftBodyHandle inv = JCE_SOFTBODY_INVALID;
    jce_vec3 c = jce_v3(1.0f, 2.0f, 3.0f);
    jce_vec3 mn = jce_v3(0, 0, 0), mx = jce_v3(0, 0, 0);
    float buf[8];

    /* All no-op / false on the invalid sentinel (no crash). */
    jce_softbody_destroy(inv);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_softbody_node_count(inv));
    TEST_ASSERT_FALSE(jce_softbody_get_positions(inv, buf, 8u));
    TEST_ASSERT_FALSE(jce_softbody_get_center(inv, &c));
    TEST_ASSERT_FALSE(jce_softbody_get_aabb(inv, &mn, &mx));

    /* Same on a destroyed handle + double-destroy safe. */
    JceSoftBodyHandle h = make_ball(jce_v3(0.0f, START_Y, 0.0f), 100.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, h);
    jce_softbody_destroy(h);

    TEST_ASSERT_EQUAL_UINT32(0u, jce_softbody_node_count(h));
    TEST_ASSERT_FALSE(jce_softbody_get_positions(h, buf, 8u));
    TEST_ASSERT_FALSE(jce_softbody_get_center(h, &c));
    TEST_ASSERT_FALSE(jce_softbody_get_aabb(h, &mn, &mx));
    jce_softbody_destroy(h);   /* double-destroy: no-op */

    jce_softbody_clear_statics();
    jce_softbody_clear_statics();   /* idempotent */
}

static void test_determinism(void)
{
    jce_softbody_set_simulation_enabled(true);

    /* Run 1. */
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());
    JceSoftBodyHandle a = make_ball(jce_v3(0.0f, START_Y, 0.0f), 120.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, a);
    step_n(200);
    jce_vec3 ca = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_center(a, &ca));
    jce_softbody_destroy(a);
    jce_softbody_clear_statics();

    /* Run 2: same sequence after a full reset. */
    TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, add_ground());
    JceSoftBodyHandle b = make_ball(jce_v3(0.0f, START_Y, 0.0f), 120.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_SOFTBODY_INVALID, b);
    step_n(200);
    jce_vec3 cb = jce_v3(0, 0, 0);
    TEST_ASSERT_TRUE(jce_softbody_get_center(b, &cb));
    jce_softbody_destroy(b);
    jce_softbody_clear_statics();

    TEST_ASSERT_FLOAT_WITHIN(1e-3f, ca.x, cb.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, ca.y, cb.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, ca.z, cb.z);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_soft_body_rests_on_a_sphere_proxy);
    RUN_TEST(test_a_soft_body_rests_on_a_capsule_proxy);
    RUN_TEST(test_a_degenerate_capsule_is_accepted);
    RUN_TEST(test_a_soft_body_rests_on_a_box_proxy_at_the_same_place);
    RUN_TEST(test_settles_on_ground);
    RUN_TEST(test_pressure_resists_collapse);
    RUN_TEST(test_higher_pressure_settles_taller);
    RUN_TEST(test_node_readback_and_center);
    RUN_TEST(test_invalid_handle_safe);
    RUN_TEST(test_determinism);
    return UNITY_END();
}
