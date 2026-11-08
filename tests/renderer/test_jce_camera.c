/* test_jce_camera.c
 *
 * Pure-math unit tests for the renderer's camera struct.  No bgfx is
 * touched: every entry point exercised here only walks jce_math and
 * the camera's internal state.  We rely on the public API only so the
 * test stays useful even if the struct is reshuffled.
 */

#include <jce/renderer/jce_camera.h>

#include <math.h>
#include <float.h>
#include <string.h>
#include <stdlib.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>

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

static void assert_direction(jce_vec3 expected, jce_vec3 actual)
{
    TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, expected.z, actual.z);
}

static void test_exact_pose_preserves_poles_near_poles_and_roll(void)
{
    JceCameraDesc desc = default_persp_desc();
    JceCamera *camera = jce_camera_create(&desc);
    const jce_vec3 forwards[] = {{0, -1, 0}, {0, 1, 0},
        {.0001f, -1, .0002f}, {.3f, .4f, -.5f}, {0, 0, -1}};
    const jce_vec3 ups[] = {{0, 0, -1}, {1, 0, 0},
        {0, 0, -1}, {1, 2, 1}, {1, 0, 0}};
    jce_vec3 position = jce_v3(0, 30, 0);
    for (unsigned i = 0; i < sizeof forwards / sizeof forwards[0]; ++i) {
        jce_vec3 forward = jce_v3_normalize(forwards[i]);
        jce_vec3 up = jce_v3_normalize(jce_v3_sub(ups[i],
            jce_v3_scale(forward, jce_v3_dot(ups[i], forward))));
        TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, forwards[i], ups[i]));
        assert_direction(forward, jce_camera_get_forward(camera));
        assert_direction(up, jce_camera_get_up(camera));
        assert_direction(jce_v3_normalize(jce_v3_cross(forward, up)),
                         jce_camera_get_right(camera));
        jce_mat4 view = jce_camera_view(camera);
        jce_vec3 ahead = jce_v3_add(position, jce_v3_scale(forward, 10));
        jce_vec4 projected = jce_m4_mul_v4(&view,
            jce_v4(ahead.x, ahead.y, ahead.z, 1));
        TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 0, projected.x);
        TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 0, projected.y);
        TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, -10, projected.z);
        jce_camera_move_forward(camera, 2);
        assert_direction(jce_v3_add(position, jce_v3_scale(forward, 2)),
                         jce_camera_get_position(camera));
    }
    /* Interactive FPS rotation must still clamp an authored pole. */
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, jce_v3(0, -1, 0), jce_v3(0, 0, -1)));
    jce_camera_rotate(camera, 0, -1);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -sinf(89 * JCE_DEG2RAD),
                             jce_camera_get_forward(camera).y);
    jce_camera_destroy(camera);
}

static void test_invalid_pose_does_not_mutate_exact_pose(void)
{
    JceCameraDesc desc = default_persp_desc();
    JceCamera *camera = jce_camera_create(&desc);
    jce_vec3 position = jce_v3(1, 2, 3);
    jce_vec3 forward = jce_v3(0, -1, 0), up = jce_v3(0, 0, -1);
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, forward, up));
    TEST_ASSERT_FALSE(jce_camera_set_pose(camera, jce_v3(9, 9, 9), forward, forward));
    TEST_ASSERT_FALSE(jce_camera_set_pose(camera, position, jce_v3(0, 0, 0), up));
    TEST_ASSERT_FALSE(jce_camera_set_pose(camera, position, forward, jce_v3(FLT_MAX, 0, 0)));
    TEST_ASSERT_FALSE(jce_camera_set_pose(camera, jce_v3(NAN, 0, 0), forward, up));
    assert_direction(position, jce_camera_get_position(camera));
    assert_direction(forward, jce_camera_get_forward(camera));
    assert_direction(up, jce_camera_get_up(camera));
    jce_camera_destroy(camera);
}

static void assert_finite_camera_basis(JceCamera *camera)
{
    jce_vec3 forward = jce_camera_get_forward(camera);
    jce_vec3 up = jce_camera_get_up(camera);
    jce_vec3 right = jce_camera_get_right(camera);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1, jce_v3_len(forward));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1, jce_v3_len(up));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1, jce_v3_len(right));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0, jce_v3_dot(forward, up));
    jce_mat4 view = jce_camera_view(camera);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            TEST_ASSERT_TRUE(isfinite(view.raw[row][col]));
}

static void test_exact_pole_transitions_to_safe_fps_basis(void)
{
    JceCameraDesc desc = default_persp_desc();
    JceCamera *camera = jce_camera_create(&desc);
    jce_vec3 position = jce_v3(0, 30, 0);
    jce_vec3 forward = jce_v3(0, -1, 0), up = jce_v3(0, 0, -1);
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, forward, up));
    jce_camera_rotate(camera, 0, 0);
    assert_direction(forward, jce_camera_get_forward(camera));
    assert_direction(up, jce_camera_get_up(camera));
    jce_camera_rotate(camera, 0, JCE_PI * .5f);
    assert_direction(jce_v3(0, 0, -1), jce_camera_get_forward(camera));
    assert_finite_camera_basis(camera);
    /* Both signs of pole zeros must preserve a real prior heading. */
    jce_camera_rotate(camera, JCE_PI * .5f, 0);
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position,
        jce_v3(-0.0f, -1, +0.0f), up));
    jce_camera_rotate(camera, 0, JCE_PI * .5f);
    assert_direction(jce_v3(1, 0, 0), jce_camera_get_forward(camera));
    assert_finite_camera_basis(camera);
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, forward, up));
    jce_camera_look_at(camera, jce_v3(0, 30, -10));
    assert_finite_camera_basis(camera);
    TEST_ASSERT_TRUE(jce_camera_set_pose(camera, position, forward, up));
    jce_camera_third_person_follow(camera, jce_v3(0, 0, 0));
    assert_finite_camera_basis(camera);
    jce_camera_destroy(camera);
}

static unsigned projection_log_count;
static unsigned projection_nonwarn_count;
static void projection_log_sink(const JceLogRecord *record, void *user)
{
    (void)user;
    if (strncmp(record->message, "camera proj:", 12) != 0) return;
    ++projection_log_count;
    if (record->level != JCE_LOG_LEVEL_WARN) ++projection_nonwarn_count;
}
static void test_projection_diagnostics_are_opt_in(void)
{
    JceCameraDesc desc = default_persp_desc();
    JceCamera *camera = jce_camera_create(&desc);
    TEST_ASSERT_NOT_NULL(camera);
    const char *value = getenv("JCE_LOG_CAMERA");
    const bool enabled = value && value[0] && value[0] != '0';
    projection_log_count = projection_nonwarn_count = 0;
    jce_log_set_sink(projection_log_sink, NULL);
    for (int i = 0; i < 10; ++i) {
        (void)jce_camera_proj(camera, i % 2 ? 1.0f : 2.0f, false);
        /* An unchanged diagnostic remains deduplicated when enabled. */
        (void)jce_camera_proj(camera, i % 2 ? 1.0f : 2.0f, false);
    }
    jce_log_flush();
    jce_log_set_sink(NULL, NULL);
    jce_camera_destroy(camera);
    TEST_ASSERT_EQUAL_UINT(enabled ? 2u : 0u, projection_log_count);
    TEST_ASSERT_EQUAL_UINT(0u, projection_nonwarn_count);
}

/* The generated private struct + complete clone body come from the actual
 * camera owner. Only its allocation spelling is a typed counted port; the
 * source and successful clones use the real shared tracked heap. */
static unsigned clone_owner_alloc_calls;
static bool clone_owner_fail;
static void *clone_owner_alloc_port(size_t count, size_t size)
{
    ++clone_owner_alloc_calls;
    if (clone_owner_fail || (size && count > SIZE_MAX / size)) return NULL;
    void *memory = jce_malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
#define JCE_CALLOC(count, size) clone_owner_alloc_port((count), (size))
#define jce_camera_clone camera_clone_actual_owner
#include JCE_CAMERA_CLONE_OWNER
#undef jce_camera_clone
#undef JCE_CALLOC

static void test_clone_preserves_complete_public_projection_and_independent_pose(void)
{
    for (int projection = 0; projection < 3; ++projection) {
        JceCameraDesc desc = default_persp_desc();
        JceCamera *source = jce_camera_create(&desc);
        TEST_ASSERT_NOT_NULL(source);
        jce_camera_set_fov(source, 79.0f);
        jce_camera_set_near_far(source, 0.23f, 487.0f);
        jce_camera_set_mode(source, projection ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
        if (projection == 1) jce_camera_set_ortho_size(source, 17.0f, 11.0f);
        if (projection == 2) jce_camera_set_ortho_height(source, 19.0f);
        TEST_ASSERT_TRUE(jce_camera_set_pose(source, jce_v3(2, 4, 6),
            jce_v3(0, -1, 0), jce_v3(1, 0, 0)));
        const jce_mat4 view = jce_camera_view(source);
        const jce_mat4 wide = jce_camera_proj(source, 16.0f / 9.0f, false);
        const jce_mat4 square = jce_camera_proj(source, 1.0f, true);
        JceCamera *copy = jce_camera_clone(source);
        TEST_ASSERT_NOT_NULL(copy);
        const bool distinct = copy != source;
        jce_mat4 actual = jce_camera_view(copy);
        const bool same_view = memcmp(&view, &actual, sizeof actual) == 0;
        actual = jce_camera_proj(copy, 16.0f / 9.0f, false);
        const bool same_wide = memcmp(&wide, &actual, sizeof actual) == 0;
        actual = jce_camera_proj(copy, 1.0f, true);
        const bool same_square = memcmp(&square, &actual, sizeof actual) == 0;
        jce_camera_move_right(copy, 3);
        actual = jce_camera_view(source);
        const bool source_unchanged = memcmp(&view, &actual, sizeof actual) == 0;
        jce_camera_destroy(source);
        const float copied_fov = jce_camera_get_fov(copy);
        jce_camera_destroy(copy);
        TEST_ASSERT_TRUE(distinct);
        TEST_ASSERT_TRUE(same_view);
        TEST_ASSERT_TRUE(same_wide);
        TEST_ASSERT_TRUE(same_square);
        TEST_ASSERT_TRUE(source_unchanged);
        TEST_ASSERT_FLOAT_WITHIN(EPS, 79.0f, copied_fov);
    }
    TEST_ASSERT_NULL(jce_camera_clone(NULL));
}

static void test_actual_clone_allocation_failure_and_full_state_retry(void)
{
    clone_owner_alloc_calls = 0; clone_owner_fail = false;
    TEST_ASSERT_NULL(camera_clone_actual_owner(NULL));
    TEST_ASSERT_EQUAL_UINT(0, clone_owner_alloc_calls);
    JceCameraDesc desc = default_persp_desc();
    JceCamera *source = jce_camera_create(&desc);
    TEST_ASSERT_NOT_NULL(source);
    jce_camera_set_mode(source, JCE_CAMERA_ORTHO);
    jce_camera_set_ortho_height(source, 31.0f);
    jce_camera_set_near_far(source, 0.4f, 641.0f);
    jce_camera_set_fov(source, 47.0f);
    TEST_ASSERT_TRUE(jce_camera_set_pose(source, jce_v3(7, 8, 9),
        jce_v3(0, 0, -1), jce_v3(1, 1, 0)));
    const JceCamera before = *source;
    clone_owner_fail = true;
    JceCamera *failed = camera_clone_actual_owner(source);
    const bool unchanged = memcmp(&before, source, sizeof before) == 0;
    clone_owner_fail = false;
    JceCamera *retried = camera_clone_actual_owner(source);
    const bool complete = retried && memcmp(source, retried, sizeof *source) == 0;
    const bool independent = retried && retried != source;
    jce_camera_destroy(retried);
    jce_camera_destroy(source);
    TEST_ASSERT_NULL(failed);
    TEST_ASSERT_EQUAL_UINT(2, clone_owner_alloc_calls);
    TEST_ASSERT_TRUE(unchanged);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(independent);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_projection_diagnostics_are_opt_in);
    RUN_TEST(test_clone_preserves_complete_public_projection_and_independent_pose);
    RUN_TEST(test_actual_clone_allocation_failure_and_full_state_retry);
    RUN_TEST(test_create_null_desc_returns_null);
    RUN_TEST(test_create_destroy_persp);
    RUN_TEST(test_create_applies_defaults);
    RUN_TEST(test_initial_forward_points_at_target);
    RUN_TEST(test_setters_clamp_and_apply);
    RUN_TEST(test_rotate_clamps_pitch);
    RUN_TEST(test_exact_pose_preserves_poles_near_poles_and_roll);
    RUN_TEST(test_invalid_pose_does_not_mutate_exact_pose);
    RUN_TEST(test_exact_pole_transitions_to_safe_fps_basis);
    RUN_TEST(test_move_forward_advances_along_view);
    RUN_TEST(test_look_at_reorients_camera);
    RUN_TEST(test_ortho_mode_basic);
    RUN_TEST(test_null_getters_return_defaults);
    return UNITY_END();
}
