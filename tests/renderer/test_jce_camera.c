/* test_jce_camera.c
 *
 * Pure-math unit tests for the renderer's camera struct.  No bgfx is
 * touched: every entry point exercised here only walks jce_math and
 * the camera's internal state.  We rely on the public API only so the
 * test stays useful even if the struct is reshuffled.
 */

#include <jce/renderer/jce_camera.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

static JceCameraDesc default_persp_desc(void)
{
    JceCameraDesc d;
    memset(&d, 0, sizeof(d));
    d.mode       = JCE_CAMERA_PERSPECTIVE;
    d.position   = jce_v3(0, 0, 5);
    d.target     = jce_v3(0, 0, 0);
    d.up         = jce_v3(0, 1, 0);
    d.fov_deg    = 60.0f;
    d.near_plane = 0.1f;
    d.far_plane  = 1000.0f;
    return d;
}

static void test_create_null_desc_returns_null(void)
{
    TEST_ASSERT_NULL(jce_camera_create(NULL));
}

static void test_create_destroy_persp(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(JCE_CAMERA_PERSPECTIVE, jce_camera_get_mode(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 60.0f, jce_camera_get_fov(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.1f, jce_camera_get_near(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1000.0f, jce_camera_get_far(c));

    jce_vec3 p = jce_camera_get_position(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, p.z);

    jce_camera_destroy(c);
    jce_camera_destroy(NULL);
}

static void test_create_applies_defaults(void)
{
    JceCameraDesc d;
    memset(&d, 0, sizeof(d));
    d.target = jce_v3(0, 0, -1);
    JceCamera *c = jce_camera_create(&d);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 60.0f, jce_camera_get_fov(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.1f, jce_camera_get_near(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1000.0f, jce_camera_get_far(c));
    jce_camera_destroy(c);
}

static void test_initial_forward_points_at_target(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);

    jce_vec3 f = jce_camera_get_forward(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.0f, f.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.0f, f.y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, f.z);

    jce_vec3 r = jce_camera_get_right(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, r.x);

    jce_camera_destroy(c);
}

static void test_setters_clamp_and_apply(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);

    jce_camera_set_position(c, jce_v3(1, 2, 3));
    jce_vec3 p = jce_camera_get_position(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, p.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, p.y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, p.z);

    jce_camera_set_fov(c, 90.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, jce_camera_get_fov(c));

    jce_camera_set_fov(c, -1.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 90.0f, jce_camera_get_fov(c));

    jce_camera_set_near_far(c, 1.0f, 500.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,   jce_camera_get_near(c));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 500.0f, jce_camera_get_far(c));

    jce_camera_set_mode(c, JCE_CAMERA_ORTHO);
    TEST_ASSERT_EQUAL_INT(JCE_CAMERA_ORTHO, jce_camera_get_mode(c));

    jce_camera_destroy(c);
}

static void test_rotate_clamps_pitch(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);

    /* Pitch up wildly; |fwd.y| must stay below sin(89). */
    jce_camera_rotate(c, 0.0f, 10.0f);
    jce_vec3 f = jce_camera_get_forward(c);
    TEST_ASSERT_TRUE(fabsf(f.y) <= sinf(89.0f * 3.14159265f / 180.0f) + EPS);

    jce_camera_rotate(c, 0.0f, -100.0f);
    f = jce_camera_get_forward(c);
    TEST_ASSERT_TRUE(fabsf(f.y) <= sinf(89.0f * 3.14159265f / 180.0f) + EPS);

    jce_camera_destroy(c);
}

static void test_move_forward_advances_along_view(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);

    jce_vec3 before = jce_camera_get_position(c);
    jce_camera_move_forward(c, 2.0f);
    jce_vec3 after  = jce_camera_get_position(c);

    /* Looking at origin from +Z  forward is -Z. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.z - 2.0f, after.z);
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.x, after.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.y, after.y);

    jce_camera_destroy(c);
}

static void test_look_at_reorients_camera(void)
{
    JceCameraDesc d = default_persp_desc();
    JceCamera *c = jce_camera_create(&d);

    jce_camera_set_position(c, jce_v3(0, 0, 0));
    jce_camera_look_at(c, jce_v3(0, 0, -10));
    jce_vec3 f = jce_camera_get_forward(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, f.z);

    jce_camera_look_at(c, jce_v3(0, 0, 0));
    /* Zero direction must be ignored, forward unchanged. */
    f = jce_camera_get_forward(c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, f.z);

    jce_camera_destroy(c);
}

static void test_ortho_mode_basic(void)
{
    JceCameraDesc d = default_persp_desc();
    d.mode    = JCE_CAMERA_ORTHO;
    d.ortho_w = 1280.0f;
    d.ortho_h = 720.0f;
    JceCamera *c = jce_camera_create(&d);
    TEST_ASSERT_EQUAL_INT(JCE_CAMERA_ORTHO, jce_camera_get_mode(c));

    jce_camera_set_ortho_size(c, 800.0f, 600.0f);
    /* No getter for ortho size, but proj matrix must not crash. */
    jce_mat4 m = jce_camera_proj(c, 4.0f / 3.0f, true);
    (void)m;

    jce_camera_destroy(c);
}

static void test_null_getters_return_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_CAMERA_PERSPECTIVE, jce_camera_get_mode(NULL));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 60.0f,   jce_camera_get_fov(NULL));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.1f,    jce_camera_get_near(NULL));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1000.0f, jce_camera_get_far(NULL));
    jce_vec3 p = jce_camera_get_position(NULL);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, p.x);
    jce_vec3 f = jce_camera_get_forward(NULL);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, f.z);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_null_desc_returns_null);
    RUN_TEST(test_create_destroy_persp);
    RUN_TEST(test_create_applies_defaults);
    RUN_TEST(test_initial_forward_points_at_target);
    RUN_TEST(test_setters_clamp_and_apply);
    RUN_TEST(test_rotate_clamps_pitch);
    RUN_TEST(test_move_forward_advances_along_view);
    RUN_TEST(test_look_at_reorients_camera);
    RUN_TEST(test_ortho_mode_basic);
    RUN_TEST(test_null_getters_return_defaults);
    return UNITY_END();
}
