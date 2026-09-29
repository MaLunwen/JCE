/*
 * test_jce_steering.c — Unit tests for jce_steering.h (L3 AI).
 *
 * Steering behaviors are pure functions on vec3 inputs.  We verify
 * direction of returned force, magnitude clamps via truncate, and
 * degenerate inputs (zero distance, empty neighbor arrays).
 */

#include "unity.h"

#include <jce/middleware/ai/jce_steering.h>
#include <jce/os/core/jce_math.h>

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

static void assert_v3_near(jce_vec3 a, jce_vec3 b, float eps)
{
    TEST_ASSERT_FLOAT_WITHIN(eps, b.x, a.x);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.y, a.y);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.z, a.z);
}

/* ------------------------------------------------------------------ */
/* seek / flee / arrive                                                */
/* ------------------------------------------------------------------ */

static void test_seek_points_toward_target(void)
{
    jce_vec3 f = jce_steer_seek(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                jce_v3(10, 0, 0), 5.0f);
    /* desired = (5,0,0), vel=0 → force = (5,0,0). */
    assert_v3_near(f, jce_v3(5, 0, 0), EPS);
}

static void test_seek_zero_when_at_target(void)
{
    jce_vec3 f = jce_steer_seek(jce_v3(1, 2, 3), jce_v3(0, 0, 0),
                                jce_v3(1, 2, 3), 5.0f);
    assert_v3_near(f, jce_v3(0, 0, 0), EPS);
}

static void test_flee_opposes_threat_direction(void)
{
    jce_vec3 f = jce_steer_flee(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                jce_v3(0, 0, 10), 3.0f);
    /* away from +Z threat → -Z, scaled by max_speed=3. */
    assert_v3_near(f, jce_v3(0, 0, -3), EPS);
}

static void test_arrive_brakes_when_inside_radius(void)
{
    jce_vec3 f = jce_steer_arrive(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                  jce_v3(1, 0, 0), 10.0f, 5.0f);
    /* d=1, slowing_radius=5 → speed = 10*(1/5)=2; desired=(2,0,0)-vel=(2,0,0) */
    assert_v3_near(f, jce_v3(2, 0, 0), EPS);
}

static void test_arrive_brakes_when_coincident(void)
{
    jce_vec3 f = jce_steer_arrive(jce_v3(0, 0, 0), jce_v3(4, 0, 0),
                                  jce_v3(0, 0, 0), 10.0f, 5.0f);
    /* d≈0 → returns -vel. */
    assert_v3_near(f, jce_v3(-4, 0, 0), EPS);
}

/* ------------------------------------------------------------------ */
/* pursue / evade                                                      */
/* ------------------------------------------------------------------ */

static void test_pursue_leads_moving_target(void)
{
    /* Target moving +X at speed 5 ahead of us — should aim past it. */
    jce_vec3 f = jce_steer_pursue(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                  jce_v3(10, 0, 0), jce_v3(5, 0, 0), 5.0f);
    /* The force must have a positive +X component. */
    TEST_ASSERT_TRUE(f.x > 0.0f);
}

static void test_evade_runs_from_moving_threat(void)
{
    jce_vec3 f = jce_steer_evade(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                 jce_v3(10, 0, 0), jce_v3(-5, 0, 0), 5.0f);
    /* Threat coming at us; flee direction has negative-X component. */
    TEST_ASSERT_TRUE(f.x < 0.0f);
}

/* ------------------------------------------------------------------ */
/* truncate / clamp                                                    */
/* ------------------------------------------------------------------ */

static void test_truncate_shortens_long_vector(void)
{
    jce_vec3 v = jce_steer_truncate(jce_v3(10, 0, 0), 3.0f);
    assert_v3_near(v, jce_v3(3, 0, 0), EPS);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, jce_v3_len(v));
}

static void test_truncate_preserves_short_vector(void)
{
    jce_vec3 v = jce_steer_truncate(jce_v3(1, 0, 0), 5.0f);
    assert_v3_near(v, jce_v3(1, 0, 0), EPS);
}

/* ------------------------------------------------------------------ */
/* path follow                                                          */
/* ------------------------------------------------------------------ */

static void test_path_follow_advances_index_on_arrival(void)
{
    jce_vec3 wps[3] = { jce_v3(0, 0, 0), jce_v3(10, 0, 0), jce_v3(10, 0, 10) };
    uint32_t idx = 0;
    bool finished = false;

    /* Position right on first waypoint → should advance idx to 1. */
    jce_vec3 f = jce_steer_path_follow(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                       wps, 3, &idx, 1.0f, 5.0f, &finished);
    (void)f;
    TEST_ASSERT_EQUAL_UINT32(1u, idx);
    TEST_ASSERT_FALSE(finished);
}

static void test_path_follow_finishes_after_last_waypoint(void)
{
    jce_vec3 wps[1] = { jce_v3(0, 0, 0) };
    uint32_t idx = 0;
    bool finished = false;

    (void)jce_steer_path_follow(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                wps, 1, &idx, 1.0f, 5.0f, &finished);
    TEST_ASSERT_TRUE(finished);
}

/* ------------------------------------------------------------------ */
/* flocking                                                            */
/* ------------------------------------------------------------------ */

static void test_separation_empty_neighbors_is_zero(void)
{
    jce_vec3 f = jce_steer_separation(jce_v3(0, 0, 0), NULL, 0, 2.0f);
    assert_v3_near(f, jce_v3(0, 0, 0), EPS);
}

static void test_separation_pushes_away_from_neighbor(void)
{
    jce_vec3 n[1] = { jce_v3(1, 0, 0) };
    jce_vec3 f = jce_steer_separation(jce_v3(0, 0, 0), n, 1, 5.0f);
    /* Neighbor at +X within radius → force points -X. */
    TEST_ASSERT_TRUE(f.x < 0.0f);
}

static void test_alignment_matches_neighbor_velocity(void)
{
    jce_vec3 vels[2] = { jce_v3(2, 0, 0), jce_v3(2, 0, 0) };
    jce_vec3 f = jce_steer_alignment(jce_v3(0, 0, 0), vels, 2, 5.0f);
    /* Avg vel = (2,0,0), normalized, scaled by max_speed=5 → (5,0,0). */
    assert_v3_near(f, jce_v3(5, 0, 0), EPS);
}

static void test_cohesion_pulls_toward_centroid(void)
{
    jce_vec3 ps[2] = { jce_v3(10, 0, 0), jce_v3(10, 0, 0) };
    jce_vec3 f = jce_steer_cohesion(jce_v3(0, 0, 0), jce_v3(0, 0, 0),
                                    ps, 2, 4.0f);
    /* Centroid at +X → force +X. */
    TEST_ASSERT_TRUE(f.x > 0.0f);
}

/* ------------------------------------------------------------------ */
/* wander (xorshift deterministic)                                     */
/* ------------------------------------------------------------------ */

static void test_wander_is_deterministic_for_same_seed(void)
{
    JceSteerWander a = { 0.0f, 12345u };
    JceSteerWander b = { 0.0f, 12345u };
    jce_vec3 fa = jce_steer_wander(jce_v3(0, 0, 0), jce_v3(1, 0, 0),
                                   &a, 1.0f, 0.5f, 1.0f, 0.016f, 5.0f);
    jce_vec3 fb = jce_steer_wander(jce_v3(0, 0, 0), jce_v3(1, 0, 0),
                                   &b, 1.0f, 0.5f, 1.0f, 0.016f, 5.0f);
    assert_v3_near(fa, fb, EPS);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_seek_points_toward_target);
    RUN_TEST(test_seek_zero_when_at_target);
    RUN_TEST(test_flee_opposes_threat_direction);
    RUN_TEST(test_arrive_brakes_when_inside_radius);
    RUN_TEST(test_arrive_brakes_when_coincident);
    RUN_TEST(test_pursue_leads_moving_target);
    RUN_TEST(test_evade_runs_from_moving_threat);
    RUN_TEST(test_truncate_shortens_long_vector);
    RUN_TEST(test_truncate_preserves_short_vector);
    RUN_TEST(test_path_follow_advances_index_on_arrival);
    RUN_TEST(test_path_follow_finishes_after_last_waypoint);
    RUN_TEST(test_separation_empty_neighbors_is_zero);
    RUN_TEST(test_separation_pushes_away_from_neighbor);
    RUN_TEST(test_alignment_matches_neighbor_velocity);
    RUN_TEST(test_cohesion_pulls_toward_centroid);
    RUN_TEST(test_wander_is_deterministic_for_same_seed);
    return UNITY_END();
}
