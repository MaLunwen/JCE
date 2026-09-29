/*
 * test_jce_runtime_buoyancy_ripple.c -- the buoyancy pass itself.
 *
 * WHY THIS EXISTS AND THE OTHER WATER TESTS DO NOT COVER IT.
 *
 * tests/middleware/scene/test_jce_water_ripple.c pins the solver, and
 * test_jce_water_ripple_wiring.c pins the scene's ownership of it. Neither can
 * reach rt_apply_buoyancy: it is static inside jce_runtime.c, needs a physics
 * world, and is only called from the fixed tick. Mutation showed exactly what
 * that costs -- deleting the disturbance term from the buoyancy sample, and
 * flipping the impulse sign at the call site, BOTH left every existing water
 * test green. A wiring seam nothing exercises is a wiring seam that will be
 * silently unwired again.
 *
 * So this test drives the real thing: a real runtime with physics, a real
 * water body, a real buoyant rigid body, and real fixed ticks. It asserts the
 * two properties the call site owns and the solver cannot:
 *
 *   1. Something in the water DISTURBS it -- after a body has been ticked in a
 *      pond, the scene's ripple carries energy. Nothing else in the engine can
 *      put energy there.
 *   2. The disturbance FEEDS BACK -- the surface a body floats on is the sum,
 *      so a body sitting in a disturbed pond does not settle to the same place
 *      it would in a still one.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_water_ripple.h>

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* A pond at the origin with a buoyant box dropped into it. Returns the scene
 * (owned by the caller) with the runtime already created. */
static JceEntity add_water(JceScene *sc)
{
    JceEntity w = jce_scene_create_entity(sc, "Pond");
    JceWaterComponent wc;
    memset(&wc, 0, sizeof wc);
    wc.size_x      = 40.0f;
    wc.size_z      = 40.0f;
    wc.base_height = 0.0f;
    wc.visible     = true;
    /* Gerstner with no waves: a FLAT ambient surface, so anything the test
     * measures on the surface came from the disturbance layer and not from a
     * wind wave that happened to be passing. */
    wc.wave_count  = 0;
    jce_scene_set_water(sc, w, &wc);
    return w;
}

static JceEntity add_floater(JceScene *sc, float y)
{
    JceEntity e = jce_scene_create_entity(sc, "Float");
    /* Placed through the transform, which is the only setter: there is no
     * jce_scene_set_position. */
    JceTransform tr;
    memset(&tr, 0, sizeof tr);
    tr.position = jce_v3(0.0f, y, 0.0f);
    /* The IDENTITY quaternion, not a zeroed one. memset leaves (0,0,0,0),
     * which is degenerate: normalising it divides by zero and every world
     * matrix built from it comes out NaN. The symptom was a body whose height
     * read -nan from the first frame, which looks exactly like "buoyancy never
     * ran". */
    tr.rotation = jce_q_identity();
    tr.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(sc, e, &tr);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.mass         = 1.0f;
    rb.use_gravity  = true;
    rb.gravity_scale = 1.0f;
    rb.drag         = 0.0f;
    rb.angular_drag = 0.05f;
    jce_scene_set_rigidbody(sc, e, &rb);

    JceBoxColliderComponent bx;
    memset(&bx, 0, sizeof bx);
    bx.size[0] = 1.0f; bx.size[1] = 1.0f; bx.size[2] = 1.0f;
    jce_scene_set_box_collider(sc, e, &bx);

    JceBuoyancyComponent bo;
    memset(&bo, 0, sizeof bo);
    bo.buoyancy_strength = 20.0f;
    bo.drag              = 2.0f;
    bo.enabled           = true;
    jce_scene_set_buoyancy(sc, e, &bo);
    return e;
}

static JceRuntime *boot(JceScene *sc)
{
    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = sc;
    d.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&d);
    return rt;
}

/* ── 1. A body falling into the pond puts energy into it ───────────────── */

static void test_a_falling_body_disturbs_the_water(void)
{
    JceScene *sc = jce_scene_create();
    TEST_ASSERT_NOT_NULL(sc);
    add_water(sc);
    add_floater(sc, 3.0f);              /* above the surface, will fall in */

    JceRuntime *rt = boot(sc);
    TEST_ASSERT_NOT_NULL_MESSAGE(rt, "runtime with physics failed to start");

    /* Before any tick there is no ripple at all: it is created by the buoyancy
     * pass, which is the only code that knows which water body is active. */
    TEST_ASSERT_NULL_MESSAGE(jce_scene_water_ripple(sc, NULL),
        "a ripple existed before the buoyancy pass ran");

    /* Watch the surface under the body, and remember the FIRST time it moved.
     *
     * The direction of that first motion is the only thing that pins the SIGN
     * at the call site: rt_apply_buoyancy passes -vel.y, and a falling body has
     * vel.y < 0, so the impulse speed is positive and the header defines
     * positive as downward. Flip the sign there and the energy is deposited
     * just the same -- the pond still ripples, the test above still passes, and
     * the water rises to meet a falling object. Mutation showed exactly that:
     * every other assertion here survived the flip. */
    float first_move = 0.0f;
    for (int i = 0; i < 240; ++i) {
        jce_runtime_step(rt, 1.0f / 60.0f);
        if (first_move == 0.0f) {
            JceWaterRipple *rr = jce_scene_water_ripple(sc, NULL);
            if (rr) {
                const float h = jce_water_ripple_height(rr, 0.0f, 0.0f);
                if (h < -1e-6f || h > 1e-6f) first_move = h;
            }
        }
    }

    JceWaterRipple *r = jce_scene_water_ripple(sc, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(r,
        "the buoyancy pass never created the disturbance layer");

    TEST_ASSERT_TRUE_MESSAGE(jce_water_ripple_energy(r) > 0.0f,
        "a body fell into the pond and the water never moved -- the impulse "
        "at the buoyancy call site is not reaching the solver");

    TEST_ASSERT_TRUE_MESSAGE(first_move < 0.0f,
        "the water's first motion under a FALLING body was upward -- the "
        "impulse sign at the buoyancy call site is inverted");

    jce_runtime_destroy(rt);
    jce_scene_destroy(sc);
}

/* ── 2. The disturbance is ADDED to the surface a body floats on ───────── */

static void test_the_disturbance_reaches_the_float_height(void)
{
    /* Two identical runs. In the second, the pond is disturbed by hand right
     * after it exists -- a large, off-centre impulse the body cannot have made
     * itself. If the buoyancy sample reads the ambient surface alone, the two
     * bodies settle to the SAME height; if it reads the sum, they do not.
     *
     * The impulse is applied through the same public entry a game would use,
     * so this also covers "gameplay can disturb the water and physics
     * notices". */
    float rest[2] = { 0.0f, 0.0f };

    for (int pass = 0; pass < 2; ++pass) {
        JceScene *sc = jce_scene_create();
        add_water(sc);
        JceEntity e = add_floater(sc, 0.5f);
        JceRuntime *rt = boot(sc);
        TEST_ASSERT_NOT_NULL(rt);

        for (int i = 0; i < 300; ++i) {
            jce_runtime_step(rt, 1.0f / 60.0f);
            if (pass == 1 && i == 60) {
                JceWaterRipple *r = jce_scene_water_ripple(sc, NULL);
                TEST_ASSERT_NOT_NULL(r);
                /* Big, slow, and centred ON the body so the effect is a height
                 * change rather than a wave that has left by the time we
                 * look. */
                jce_water_ripple_impulse(r, 0.0f, 0.0f, 6.0f, 3.0f);
            }
        }
        /* World Y from the transform matrix: there is no
         * jce_scene_get_position, and the world matrix is what every other
         * consumer of an entity's placement reads. */
        rest[pass] = jce_scene_get_world_matrix(sc, e).col[3].y;
        jce_runtime_destroy(rt);
        jce_scene_destroy(sc);
    }

    TEST_ASSERT_TRUE_MESSAGE(fabsf(rest[0] - rest[1]) > 1e-4f,
        "disturbing the pond did not change where the body floats -- the "
        "buoyancy sample is reading the ambient surface alone");
}

/* -- 3. The grid advances even when the buoyancy pass bails ------------
 *
 * The stepper was originally at the bottom of rt_apply_buoyancy, which reads
 * as the natural home: that pass runs exactly once per executed fixed tick.
 * But it has four early returns above that point -- no rigid bodies, no
 * enabled+visible water, no field desc, no free field slot -- and the last of
 * them fires AFTER the grid has been created. So a grid could exist and never
 * advance: a ring frozen on the water, in a scene that looks fine.
 *
 * This drives the case with no argument about it: a pond, a disturbance
 * applied from gameplay, and NOT ONE rigid body. Under the old placement the
 * pass returned at `rt->body_count <= 0` and the ring never moved.
 */

static void test_the_grid_advances_with_no_bodies_at_all(void)
{
    JceScene *sc = jce_scene_create();
    add_water(sc);                       /* a pond, and nothing else */

    JceRuntime *rt = boot(sc);
    TEST_ASSERT_NOT_NULL(rt);

    /* Nothing creates the grid without the buoyancy pass, so make one the way
     * gameplay would: ask for it with a desc. */
    JceWaterRippleDesc rd = jce_water_ripple_default_desc();
    rd.resolution = 64;
    rd.size_m     = 40.0f;
    rd.damping    = 0.0f;                /* lossless, so decay cannot be read
                                          * as motion or motion as decay */
    JceWaterRipple *r = jce_scene_water_ripple(sc, &rd);
    TEST_ASSERT_NOT_NULL(r);

    jce_water_ripple_impulse(r, 0.0f, 0.0f, 3.0f, 1.0f);
    const float h0 = jce_water_ripple_height(r, 0.0f, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, h0,
        "an impulse displaced the surface before any step - it is supposed to "
        "set the velocity, not the height");

    for (int i = 0; i < 30; ++i) jce_runtime_step(rt, 1.0f / 60.0f);

    TEST_ASSERT_TRUE_MESSAGE(
        fabsf(jce_water_ripple_height(r, 0.0f, 0.0f)) > 1e-5f,
        "the disturbance never advanced in a scene with no rigid bodies - the "
        "stepper is behind one of the buoyancy pass's early returns");

    jce_runtime_destroy(rt);
    jce_scene_destroy(sc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_falling_body_disturbs_the_water);
    RUN_TEST(test_the_disturbance_reaches_the_float_height);
    RUN_TEST(test_the_grid_advances_with_no_bodies_at_all);
    return UNITY_END();
}
