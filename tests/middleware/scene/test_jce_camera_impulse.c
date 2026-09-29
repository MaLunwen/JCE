/* test_jce_camera_impulse.c
 *
 * FEATURE 6.5 — wiring the orphaned trauma-shake generator into the camera
 * system.  These tests exercise the REAL dynamic path:
 *
 *     jce_vcam_system_add_trauma  ->  jce_vcam_system_evaluate  ->
 *     jce_camera_shake (offset folded onto the resolved active-VCam pose)
 *
 * not a mock / reimplementation.  We build a JceScene with a single active
 * VirtualCamera component, evaluate a clean baseline pose, add trauma, and
 * assert the resolved camera position now differs from baseline by a NONZERO
 * amount that is BOUNDED by the configured max shake amplitude — then that it
 * decays back to ~zero after enough simulated time.
 *
 * Damping is set to 0 (snap) so the resolver's damped pose equals the static
 * VirtualCamera position every frame; the only thing that can move the output
 * is the shake offset, which makes the deltas unambiguous.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_vcam_system.h>

#include "unity.h"
#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The generator's default max positional amplitude per axis (jce_camera_shake
 * _init: 0.3 world units).  Each shake component is bounded by |offset| <= max
 * because the noise is normalised to [-1,1] and the shake factor is trauma²
 * which never exceeds 1. */
#define SHAKE_MAX_POS 0.3f
#define BOUND_EPS     1e-4f

static const float BASE_POS[3] = { 5.0f, 6.0f, 7.0f };

/* Author one active, highest-priority VirtualCamera at a fixed position with
 * NO follow/look-at targets and damping 0 (snap), so the resolved pose is
 * deterministically BASE_POS plus only the shake offset. */
static JceScene *make_scene_with_vcam(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "vcam");
    JceVirtualCameraComponent vc;
    memset(&vc, 0, sizeof vc);
    strncpy(vc.vcam_name, "vcam", sizeof vc.vcam_name - 1);
    vc.priority    = 10;
    vc.active      = true;
    vc.track_mode  = JCE_VCAM_COMP_TRACK_NONE;
    vc.position[0] = BASE_POS[0];
    vc.position[1] = BASE_POS[1];
    vc.position[2] = BASE_POS[2];
    vc.fov_deg     = 60.0f;
    vc.damping     = 0.0f;                 /* snap: pose == position each frame */
    jce_scene_set_virtual_camera(s, e, &vc);
    TEST_ASSERT_TRUE(jce_scene_has_virtual_camera(s, e));
    return s;
}

static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Baseline: with no trauma added, an active VCam resolves to its static
 * position and the shake offset is exactly zero. */
static void test_baseline_pose_has_no_shake(void)
{
    JceScene *s = make_scene_with_vcam();
    jce_vcam_system_reset();

    JceVcamOutput out;
    bool has_active = false;
    memset(&out, 0, sizeof out);
    jce_vcam_system_evaluate(s, 1.0f / 60.0f, &out, &has_active);

    TEST_ASSERT_TRUE(has_active);
    TEST_ASSERT_EQUAL_FLOAT(BASE_POS[0], out.position[0]);
    TEST_ASSERT_EQUAL_FLOAT(BASE_POS[1], out.position[1]);
    TEST_ASSERT_EQUAL_FLOAT(BASE_POS[2], out.position[2]);

    jce_scene_destroy(s);
}

/* Core property: after add_trauma the resolved position differs from baseline
 * by a NONZERO amount BOUNDED by the per-axis max amplitude — and after enough
 * decay time the offset returns to ~0. */
static void test_trauma_shakes_then_decays(void)
{
    JceScene *s = make_scene_with_vcam();
    jce_vcam_system_reset();

    const float dt = 1.0f / 60.0f;

    /* Baseline pose (offset ~0). */
    JceVcamOutput base;
    bool has_active = false;
    memset(&base, 0, sizeof base);
    jce_vcam_system_evaluate(s, dt, &base, &has_active);
    TEST_ASSERT_TRUE(has_active);
    TEST_ASSERT_FLOAT_WITHIN(BOUND_EPS, 0.0f, dist3(base.position, BASE_POS));

    /* Add full trauma and evaluate a few frames; capture the max displacement
     * and verify the per-axis bound holds every frame.  A couple of frames
     * guards against the (vanishingly unlikely) case of all three noise axes
     * sampling near zero on a single frame. */
    jce_vcam_system_add_trauma(1.0f);

    float max_disp = 0.0f;
    for (int i = 0; i < 4; ++i) {
        JceVcamOutput out;
        memset(&out, 0, sizeof out);
        jce_vcam_system_evaluate(s, dt, &out, &has_active);
        TEST_ASSERT_TRUE(has_active);

        /* Per-axis offset must stay within the configured envelope. */
        for (int a = 0; a < 3; ++a) {
            float off = fabsf(out.position[a] - BASE_POS[a]);
            TEST_ASSERT_TRUE(off <= SHAKE_MAX_POS + BOUND_EPS);
        }
        float d = dist3(out.position, BASE_POS);
        if (d > max_disp) max_disp = d;
    }

    /* It actually MOVED (nonzero shake). */
    TEST_ASSERT_TRUE(max_disp > BOUND_EPS);
    /* And the total displacement is bounded by the 3-axis envelope. */
    float bound = sqrtf(3.0f) * SHAKE_MAX_POS + BOUND_EPS;
    TEST_ASSERT_TRUE(max_disp <= bound);

    /* Let it decay: default decay is 1.5 trauma/sec, so ~1s of dt drives
     * trauma (and thus the offset) back to zero. */
    JceVcamOutput out;
    for (int i = 0; i < 120; ++i) {
        memset(&out, 0, sizeof out);
        jce_vcam_system_evaluate(s, dt, &out, &has_active);
    }
    TEST_ASSERT_TRUE(has_active);
    TEST_ASSERT_FLOAT_WITHIN(BOUND_EPS, 0.0f, dist3(out.position, BASE_POS));

    jce_scene_destroy(s);
}

/* reset() must clear residual trauma so a fresh session starts perfectly
 * still even if the previous one ended mid-shake. */
static void test_reset_clears_residual_trauma(void)
{
    JceScene *s = make_scene_with_vcam();

    jce_vcam_system_reset();
    jce_vcam_system_add_trauma(1.0f);

    const float dt = 1.0f / 60.0f;
    JceVcamOutput out;
    bool has_active = false;
    memset(&out, 0, sizeof out);
    jce_vcam_system_evaluate(s, dt, &out, &has_active);
    /* Mid-shake right now. */
    TEST_ASSERT_TRUE(dist3(out.position, BASE_POS) > BOUND_EPS);

    /* Reset must wipe trauma; the very next evaluate is shake-free. */
    jce_vcam_system_reset();
    memset(&out, 0, sizeof out);
    jce_vcam_system_evaluate(s, dt, &out, &has_active);
    TEST_ASSERT_TRUE(has_active);
    TEST_ASSERT_FLOAT_WITHIN(BOUND_EPS, 0.0f, dist3(out.position, BASE_POS));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_baseline_pose_has_no_shake);
    RUN_TEST(test_trauma_shakes_then_decays);
    RUN_TEST(test_reset_clears_residual_trauma);
    return UNITY_END();
}
