/* CPU ray/triangle tests require no graphics device. */
#include "unity.h"
#include "renderer/jce_pick_geometry_internal.h"

void setUp(void) {}
void tearDown(void) {}

static const float triangle[][3] = {
    {0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
};

static void nearest_hit_respects_stride_indices_and_ray_parameter(void)
{
    const float vertices[][4] = {
        {0.0f, 0.0f, -2.0f, 99.0f},
        {1.0f, 0.0f, -2.0f, 99.0f},
        {0.0f, 1.0f, -2.0f, 99.0f},
        {0.0f, 0.0f, 0.0f, 99.0f},
        {1.0f, 0.0f, 0.0f, 99.0f},
        {0.0f, 1.0f, 0.0f, 99.0f},
    };
    const uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    const JcePickGeometry geometry = {vertices, sizeof(vertices[0]), 6, indices, 6};
    jce_mat4 transform = jce_m4_identity();
    transform.col[0].x = 2.0f;
    transform.col[1].y = 3.0f;
    transform.col[3].x = 4.0f;
    const jce_vec3 origin = jce_v3(4.5f, 0.5f, 4.0f);
    const jce_vec3 direction = jce_v3(0.0f, 0.0f, -2.0f);
    float distance = -1.0f;

    TEST_ASSERT_TRUE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 10.0f, &distance));
    TEST_ASSERT_EQUAL_FLOAT(2.0f, distance);
    TEST_ASSERT_FALSE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 2.0f, &distance));
    TEST_ASSERT_EQUAL_FLOAT(2.0f, distance);
}

static void bounding_box_interior_can_miss_actual_triangle(void)
{
    const JcePickGeometry geometry = {triangle, sizeof(triangle[0]), 3, NULL, 0};
    const jce_mat4 transform = jce_m4_identity();
    const jce_vec3 origin = jce_v3(0.8f, 0.8f, 1.0f);
    const jce_vec3 direction = jce_v3(0.0f, 0.0f, -1.0f);
    float distance = -1.0f;

    TEST_ASSERT_FALSE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 5.0f, &distance));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, distance);
}

static void reflections_reverse_faces_and_singular_transforms_preserve_surfaces(void)
{
    const JcePickGeometry geometry = {triangle, sizeof(triangle[0]), 3, NULL, 0};
    jce_mat4 transform = jce_m4_identity();
    const jce_vec3 origin = jce_v3(-0.2f, 0.2f, 1.0f);
    const jce_vec3 direction = jce_v3(0.0f, 0.0f, -1.0f);
    float distance = -1.0f;
    transform.col[0].x = -1.0f;
    transform.col[2].z = 0.0f;

    TEST_ASSERT_FALSE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 5.0f, &distance));
    TEST_ASSERT_TRUE(jce_pick_geometry_ray(&geometry, &transform, true,
        origin, direction, 5.0f, &distance));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, distance);
    transform.col[1].y = 0.0f;
    TEST_ASSERT_FALSE(jce_pick_geometry_ray(&geometry, &transform, true,
        origin, direction, 5.0f, &distance));
}

static void bad_indices_do_not_hide_valid_triangles(void)
{
    const uint32_t indices[] = {0, 1, UINT32_MAX, 0, 1, 2};
    JcePickGeometry geometry = {triangle, sizeof(triangle[0]), 3, indices, 6};
    const jce_mat4 transform = jce_m4_identity();
    const jce_vec3 origin = jce_v3(0.2f, 0.2f, 1.0f);
    const jce_vec3 direction = jce_v3(0.0f, 0.0f, -1.0f);

    TEST_ASSERT_TRUE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 5.0f, NULL));
    geometry.index_count = 3;
    TEST_ASSERT_FALSE(jce_pick_geometry_ray(&geometry, &transform, false,
        origin, direction, 5.0f, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(nearest_hit_respects_stride_indices_and_ray_parameter);
    RUN_TEST(bounding_box_interior_can_miss_actual_triangle);
    RUN_TEST(reflections_reverse_faces_and_singular_transforms_preserve_surfaces);
    RUN_TEST(bad_indices_do_not_hide_valid_triangles);
    return UNITY_END();
}
