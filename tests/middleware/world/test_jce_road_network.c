/*
 * test_jce_road_network.c — Unit tests for jce_road_network.h (L4 world).
 *
 * Pure CPU graph + polyline storage; brute-force geometric queries.
 */

#include "unity.h"

#include <jce/middleware/world/jce_road_network.h>
#include <jce/os/core/jce_math.h>

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static JceRoadNetwork *make_net(void)
{
    JceRoadNetworkDesc d = { 0, 0, 0 };
    return jce_road_network_create(&d);
}

/* Build a straight horizontal segment from (x0,0,0) to (x1,0,0). */
static uint32_t add_straight(JceRoadNetwork *n, float x0, float x1,
                              uint8_t lanes_ab, uint8_t lanes_ba)
{
    uint32_t a = jce_road_network_add_node(n, jce_v3(x0, 0, 0));
    uint32_t b = jce_road_network_add_node(n, jce_v3(x1, 0, 0));
    jce_vec3 pts[2] = { jce_v3(x0, 0, 0), jce_v3(x1, 0, 0) };
    return jce_road_network_add_segment(n, a, b, pts, 2,
                                         lanes_ab, lanes_ba,
                                         JCE_ROAD_CLASS_LOCAL, 50.0f);
}

/* ------------------------------------------------------------------ */
/* Lifecycle / NULL                                                     */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceRoadNetwork *n = make_net();
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_road_network_node_count(n));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_road_network_segment_count(n));
    jce_road_network_destroy(n);
}

static void test_destroy_null_safe(void)
{
    jce_road_network_destroy(NULL);
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* Insert                                                               */
/* ------------------------------------------------------------------ */

static void test_add_node_returns_sequential_ids(void)
{
    JceRoadNetwork *n = make_net();
    uint32_t a = jce_road_network_add_node(n, jce_v3(0, 0, 0));
    uint32_t b = jce_road_network_add_node(n, jce_v3(10, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, a);
    TEST_ASSERT_EQUAL_UINT32(1u, b);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_road_network_node_count(n));
    jce_road_network_destroy(n);
}

static void test_add_segment_length_is_polyline_sum(void)
{
    JceRoadNetwork *n = make_net();
    uint32_t a = jce_road_network_add_node(n, jce_v3(0, 0, 0));
    uint32_t b = jce_road_network_add_node(n, jce_v3(10, 0, 0));
    /* Three-point L-shape: (0,0,0) → (10,0,0) → (10,0,5) length = 15. */
    jce_vec3 pts[3] = { jce_v3(0,0,0), jce_v3(10,0,0), jce_v3(10,0,5) };
    uint32_t seg = jce_road_network_add_segment(n, a, b, pts, 3, 2, 2,
                                                 JCE_ROAD_CLASS_LOCAL, 60.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_ROAD_INVALID_ID, seg);
    const JceRoadSegment *s = jce_road_network_get_segment(n, seg);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, s->length);
    jce_road_network_destroy(n);
}

static void test_add_segment_rejects_invalid(void)
{
    JceRoadNetwork *n = make_net();
    (void)jce_road_network_add_node(n, jce_v3(0, 0, 0));
    /* Bogus second node id. */
    jce_vec3 pts[2] = { jce_v3(0,0,0), jce_v3(1,0,0) };
    uint32_t seg = jce_road_network_add_segment(n, 0, 99, pts, 2, 1, 1,
                                                 JCE_ROAD_CLASS_LOCAL, 30.0f);
    TEST_ASSERT_EQUAL_UINT32(JCE_ROAD_INVALID_ID, seg);
    /* Too few polyline points. */
    seg = jce_road_network_add_segment(n, 0, 0, pts, 1, 1, 1,
                                        JCE_ROAD_CLASS_LOCAL, 30.0f);
    TEST_ASSERT_EQUAL_UINT32(JCE_ROAD_INVALID_ID, seg);
    jce_road_network_destroy(n);
}

/* ------------------------------------------------------------------ */
/* Queries                                                              */
/* ------------------------------------------------------------------ */

static void test_sample_segment_endpoints_and_midpoint(void)
{
    JceRoadNetwork *n = make_net();
    uint32_t seg = add_straight(n, 0.0f, 10.0f, 1, 1);
    jce_vec3 pos, fwd;
    TEST_ASSERT_TRUE(jce_road_network_sample_segment(n, seg, 0.0f, &pos, &fwd));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pos.x);
    TEST_ASSERT_TRUE(jce_road_network_sample_segment(n, seg, 1.0f, &pos, &fwd));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, pos.x);
    TEST_ASSERT_TRUE(jce_road_network_sample_segment(n, seg, 0.5f, &pos, &fwd));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, pos.x);
    /* Forward should be +X-ish, unit length. */
    float fl = sqrtf(fwd.x*fwd.x + fwd.y*fwd.y + fwd.z*fwd.z);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, fl);
    TEST_ASSERT_TRUE(fwd.x > 0.9f);
    jce_road_network_destroy(n);
}

static void test_closest_segment_picks_nearby(void)
{
    JceRoadNetwork *n = make_net();
    (void)add_straight(n, 0.0f, 10.0f, 1, 1);
    float t, dist;
    uint32_t hit = jce_road_network_closest_segment(n, jce_v3(5.0f, 0.0f, 0.5f),
                                                     &t, &dist);
    TEST_ASSERT_NOT_EQUAL(JCE_ROAD_INVALID_ID, hit);
    TEST_ASSERT_TRUE(t >= 0.0f && t <= 1.0f);
    TEST_ASSERT_TRUE(dist < 1.0f);
    jce_road_network_destroy(n);
}

static void test_finalize_marks_then_clear_resets(void)
{
    JceRoadNetwork *n = make_net();
    (void)add_straight(n, 0.0f, 10.0f, 1, 1);
    jce_road_network_finalize(n);
    /* finalize is idempotent; clear wipes counts. */
    jce_road_network_clear(n);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_road_network_node_count(n));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_road_network_segment_count(n));
    jce_road_network_destroy(n);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_destroy_null_safe);
    RUN_TEST(test_add_node_returns_sequential_ids);
    RUN_TEST(test_add_segment_length_is_polyline_sum);
    RUN_TEST(test_add_segment_rejects_invalid);
    RUN_TEST(test_sample_segment_endpoints_and_midpoint);
    RUN_TEST(test_closest_segment_picks_nearby);
    RUN_TEST(test_finalize_marks_then_clear_resets);
    return UNITY_END();
}
