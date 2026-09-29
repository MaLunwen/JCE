/*
 * test_jce_water_ripple_wiring.c -- the disturbance layer's SEAMS.
 *
 * test_jce_water_ripple.c pins the solver. This pins the two contracts the
 * rest of the engine depends on, which the solver's own tests cannot see:
 *
 *   1. The scene OWNS one ripple, created by whoever supplies a desc and
 *      returned unchanged to everyone else. A reader must never be able to
 *      create one, or the size of the pond would be decided by whichever
 *      subsystem happened to run first.
 *   2. The buoyancy contract is that a body floats on ambient PLUS
 *      disturbance. The arithmetic of that sum is what this file can reach
 *      without a physics world: sampling the ripple at a disturbed point must
 *      return a non-zero height, and sampling outside must return exactly
 *      zero -- because "outside" is where the ADD has to be a no-op or every
 *      body in the world would be lifted by a pond it is nowhere near.
 *
 * Headless: no physics, no GPU, no clock.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_water_ripple.h>

#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* ── 1. One ripple per scene, and only a desc can create it ───────────── */

static void test_scene_owns_exactly_one_ripple(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    /* A reader (NULL desc) must NOT create one. This is the whole reason the
     * parameter is optional: the renderer and the buoyancy pass both read, and
     * only the buoyancy pass knows which water body is active and how big it
     * is. If a read could create, the first reader would size the pond. */
    TEST_ASSERT_NULL_MESSAGE(jce_scene_water_ripple(s, NULL),
        "a NULL-desc read created a ripple - the size of the grid is now "
        "decided by whoever reads first");

    JceWaterRippleDesc d = jce_water_ripple_default_desc();
    d.resolution = 48;
    d.size_m     = 24.0f;
    JceWaterRipple *a = jce_scene_water_ripple(s, &d);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(48, jce_water_ripple_resolution(a));

    /* Same instance afterwards, whether asked with a desc or without. */
    TEST_ASSERT_EQUAL_PTR(a, jce_scene_water_ripple(s, NULL));

    /* And a SECOND desc must not resize or replace it: the grid carries the
     * state that is the entire point of it, and a resize would discard the
     * ripples currently on the water. */
    JceWaterRippleDesc d2 = jce_water_ripple_default_desc();
    d2.resolution = 128;
    d2.size_m     = 100.0f;
    JceWaterRipple *b = jce_scene_water_ripple(s, &d2);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(a, b, "a second desc replaced the ripple");
    TEST_ASSERT_EQUAL_INT_MESSAGE(48, jce_water_ripple_resolution(b),
        "a second desc resized the ripple and threw its state away");

    jce_scene_destroy(s);          /* must not leak or double-free the ripple */
}

static void test_null_scene_is_answerable(void)
{
    TEST_ASSERT_NULL(jce_scene_water_ripple(NULL, NULL));
    JceWaterRippleDesc d = jce_water_ripple_default_desc();
    TEST_ASSERT_NULL(jce_scene_water_ripple(NULL, &d));
}

/* ── 2. The term buoyancy adds: non-zero inside, EXACTLY zero outside ──── */

static void test_the_added_term_is_zero_away_from_the_pond(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceWaterRippleDesc d = jce_water_ripple_default_desc();
    d.resolution      = 64;
    d.size_m          = 32.0f;      /* centred on the origin, so +-16 m */
    d.center_x        = 0.0f;
    d.center_z        = 0.0f;
    d.damping         = 0.0f;
    JceWaterRipple *r = jce_scene_water_ripple(s, &d);
    TEST_ASSERT_NOT_NULL(r);

    /* Nothing has happened yet: the ADD must be a no-op everywhere, or every
     * body in the scene shifts the moment a pond exists. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r,   0.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, 100.0f, 0.0f));

    /* A body drops in at the centre: impulse, then the step the buoyancy pass
     * performs once per fixed tick. */
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 2.0f, 1.0f);
    for (int i = 0; i < 6; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    TEST_ASSERT_TRUE_MESSAGE(
        fabsf(jce_water_ripple_height(r, 0.0f, 0.0f)) > 1e-5f,
        "the disturbance is flat where a body just entered - buoyancy would "
        "float on the ambient surface alone and the wiring is inert");

    /* Far outside the grid it must be EXACTLY zero, not merely small: this
     * value is added to every buoyant body's water height, including bodies in
     * a different pond a kilometre away. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r,  1000.0f,    0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, -1000.0f,    0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r,     0.0f, 1000.0f));

    /* A NULL ripple must add nothing rather than crash: a scene with no pond
     * is the common case, and the buoyancy pass calls this unconditionally. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(NULL, 0.0f, 0.0f));

    jce_scene_destroy(s);
}

/* ── 3. The wake's sign: a body going DOWN pushes the surface down ─────── */

static void test_a_descending_body_pushes_the_surface_down(void)
{
    JceScene *s = jce_scene_create();
    JceWaterRippleDesc d = jce_water_ripple_default_desc();
    d.resolution = 64; d.size_m = 32.0f; d.damping = 0.0f;
    JceWaterRipple *r = jce_scene_water_ripple(s, &d);
    TEST_ASSERT_NOT_NULL(r);

    /* rt_apply_buoyancy passes -vel.y: a body falling has vel.y < 0, so the
     * impulse speed is POSITIVE, and the header defines positive as downward.
     * The conversion is reproduced here, which pins the SOLVER's half of that
     * contract -- positive means down.
     *
     * It does NOT pin the call site: this file cannot reach rt_apply_buoyancy,
     * and mutation confirmed that flipping the sign there left every assertion
     * in this file green. That half is pinned by
     * tests/application/test_jce_runtime_buoyancy_ripple.c, which watches which
     * way the surface first moves under a falling body. An earlier version of
     * this comment claimed otherwise and was wrong. */
    const float vel_y = -2.0f;                 /* falling */
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 1.0f, -vel_y);
    jce_water_ripple_step(r, 1.0f / 240.0f);

    TEST_ASSERT_TRUE_MESSAGE(jce_water_ripple_height(r, 0.0f, 0.0f) < 0.0f,
        "a body falling into the water lifted the surface instead of "
        "displacing it downward");

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scene_owns_exactly_one_ripple);
    RUN_TEST(test_null_scene_is_answerable);
    RUN_TEST(test_the_added_term_is_zero_away_from_the_pond);
    RUN_TEST(test_a_descending_body_pushes_the_surface_down);
    return UNITY_END();
}
