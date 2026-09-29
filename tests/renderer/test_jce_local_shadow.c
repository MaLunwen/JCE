/* test_jce_local_shadow.c
 *
 * Pure-math unit tests for the local (spot/point) shadow helpers:
 *   - jce_local_shadow_vp       : perspective light-view-proj
 *   - jce_local_shadow_atlas_tile: atlas tile rect packing
 * No bgfx is touched.
 */

#include <jce/renderer/jce_local_shadow.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 2e-2f

/* out = M * v   (column-major M, raw[col][row]). */
static void mul_m4v4(const jce_mat4 *m, const float v[4], float out[4])
{
    for (int r = 0; r < 4; r++)
        out[r] = m->raw[0][r] * v[0] + m->raw[1][r] * v[1]
               + m->raw[2][r] * v[2] + m->raw[3][r] * v[3];
}

/* A point straight in front of the light projects to NDC center (x/w≈0,
 * y/w≈0) and lies in front (w>0). This pins aim + look_at + perspective +
 * multiply order all at once. */
static void test_vp_forward_point_projects_to_center(void)
{
    jce_vec3 pos = jce_v3(5.0f, 3.0f, 2.0f);
    jce_vec3 dir = jce_v3(0.0f, 0.0f, -1.0f);
    jce_mat4 vp = jce_local_shadow_vp(pos, dir, 1.0472f /*60deg*/,
                                      0.5f, 10.0f, false);

    /* point 2 units along the aim axis */
    float p[4] = { pos.x + dir.x * 2.0f, pos.y + dir.y * 2.0f,
                   pos.z + dir.z * 2.0f, 1.0f };
    float c[4];
    mul_m4v4(&vp, p, c);

    TEST_ASSERT_TRUE(c[3] > 0.0f);                  /* in front of the light */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, c[0] / c[3]); /* ndc x ≈ 0 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, c[1] / c[3]); /* ndc y ≈ 0 */
}

/* A near-vertical aim must NOT degenerate the look-at up-vector into NaN. */
static void test_vp_vertical_aim_is_finite(void)
{
    jce_vec3 pos = jce_v3(0.0f, 5.0f, 0.0f);
    jce_vec3 dir = jce_v3(0.0f, -1.0f, 0.0f);     /* straight down */
    jce_mat4 vp = jce_local_shadow_vp(pos, dir, 2.4f, 0.5f, 10.0f, false);

    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            TEST_ASSERT_TRUE(isfinite(vp.raw[c][r]));

    /* forward (downward) point still projects near center */
    float p[4] = { 0.0f, 3.0f, 0.0f, 1.0f };
    float o[4];
    mul_m4v4(&vp, p, o);
    TEST_ASSERT_TRUE(o[3] > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, o[0] / o[3]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, o[1] / o[3]);
}

/* 2x2 grid in a 2048 atlas -> 1024 tiles, row-major. */
static void test_atlas_tile_2x2(void)
{
    uint16_t x = 9, y = 9, sz = 9;

    TEST_ASSERT_TRUE(jce_local_shadow_atlas_tile(0, 2048, 2, &x, &y, &sz));
    TEST_ASSERT_EQUAL_UINT16(0, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    TEST_ASSERT_EQUAL_UINT16(1024, sz);

    TEST_ASSERT_TRUE(jce_local_shadow_atlas_tile(1, 2048, 2, &x, &y, &sz));
    TEST_ASSERT_EQUAL_UINT16(1024, x); TEST_ASSERT_EQUAL_UINT16(0, y);

    TEST_ASSERT_TRUE(jce_local_shadow_atlas_tile(2, 2048, 2, &x, &y, &sz));
    TEST_ASSERT_EQUAL_UINT16(0, x); TEST_ASSERT_EQUAL_UINT16(1024, y);

    TEST_ASSERT_TRUE(jce_local_shadow_atlas_tile(3, 2048, 2, &x, &y, &sz));
    TEST_ASSERT_EQUAL_UINT16(1024, x); TEST_ASSERT_EQUAL_UINT16(1024, y);
    TEST_ASSERT_EQUAL_UINT16(1024, sz);
}

/* Out-of-range slot and degenerate inputs return false. */
static void test_atlas_tile_out_of_range(void)
{
    uint16_t x, y, sz;
    TEST_ASSERT_FALSE(jce_local_shadow_atlas_tile(4, 2048, 2, &x, &y, &sz));
    TEST_ASSERT_FALSE(jce_local_shadow_atlas_tile(0, 2048, 0, &x, &y, &sz));
    TEST_ASSERT_FALSE(jce_local_shadow_atlas_tile(0, 0, 2, &x, &y, &sz));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vp_forward_point_projects_to_center);
    RUN_TEST(test_vp_vertical_aim_is_finite);
    RUN_TEST(test_atlas_tile_2x2);
    RUN_TEST(test_atlas_tile_out_of_range);
    return UNITY_END();
}
