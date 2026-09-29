/* test_jce_camera_shake.c
 *
 * Unit tests for the trauma-based camera shake (jce_camera_shake):
 *   - add_trauma clamps to [0,1]; amount == trauma²
 *   - update decays trauma to zero and advances time
 *   - offset is exactly zero with no trauma; bounded by amplitude otherwise
 *   - deterministic for the same (state, time)
 */

#include <jce/os/core/jce_camera_shake.h>

#include "unity.h"

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_trauma_clamp_and_amount(void)
{
    JceCameraShake s;
    jce_camera_shake_init(&s, 1.0f, 10.0f, 42u);
    TEST_ASSERT_FALSE(jce_camera_shake_active(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_camera_shake_amount(&s));

    jce_camera_shake_add_trauma(&s, 0.5f);
    TEST_ASSERT_TRUE(jce_camera_shake_active(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, jce_camera_shake_amount(&s));   /* 0.5² */

    jce_camera_shake_add_trauma(&s, 0.8f);                         /* clamps to 1 */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_camera_shake_amount(&s));
}

static void test_decay_to_zero(void)
{
    JceCameraShake s;
    jce_camera_shake_init(&s, 2.0f, 10.0f, 7u);   /* 2 trauma/sec */
    jce_camera_shake_add_trauma(&s, 1.0f);

    jce_camera_shake_update(&s, 0.25f);            /* -0.5 -> 0.5 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, s.trauma);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, s.time);

    for (int i = 0; i < 10; ++i) jce_camera_shake_update(&s, 0.1f);
    TEST_ASSERT_FALSE(jce_camera_shake_active(&s));  /* fully decayed */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_camera_shake_amount(&s));
}

static void test_offset_zero_without_trauma(void)
{
    JceCameraShake s;
    jce_camera_shake_init(&s, 1.0f, 10.0f, 99u);
    jce_camera_shake_update(&s, 1.0f);             /* time advances, trauma stays 0 */
    float pos[3], rot[3];
    jce_camera_shake_offset(&s, pos, rot);
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, pos[i]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, rot[i]);
    }
}

static void test_offset_bounded_by_amplitude(void)
{
    JceCameraShake s;
    jce_camera_shake_init(&s, 0.01f, 13.0f, 5u);
    float mp[3] = { 0.5f, 0.5f, 0.5f };
    float mr[3] = { 10.0f, 10.0f, 10.0f };
    jce_camera_shake_set_amplitude(&s, mp, mr);
    jce_camera_shake_add_trauma(&s, 1.0f);

    /* Sample across time; every component must stay within its max. */
    for (int k = 0; k < 200; ++k) {
        jce_camera_shake_update(&s, 0.016f);
        jce_camera_shake_add_trauma(&s, 0.02f);    /* keep trauma high */
        float pos[3], rot[3];
        jce_camera_shake_offset(&s, pos, rot);
        for (int i = 0; i < 3; ++i) {
            TEST_ASSERT_TRUE(fabsf(pos[i]) <= mp[i] + 1e-4f);
            TEST_ASSERT_TRUE(fabsf(rot[i]) <= mr[i] + 1e-3f);
        }
    }
}

static void test_deterministic(void)
{
    JceCameraShake a, b;
    jce_camera_shake_init(&a, 1.0f, 10.0f, 123u);
    jce_camera_shake_init(&b, 1.0f, 10.0f, 123u);
    jce_camera_shake_add_trauma(&a, 0.7f);
    jce_camera_shake_add_trauma(&b, 0.7f);
    for (int k = 0; k < 5; ++k) {
        jce_camera_shake_update(&a, 0.05f);
        jce_camera_shake_update(&b, 0.05f);
    }
    float pa[3], ra[3], pb[3], rb[3];
    jce_camera_shake_offset(&a, pa, ra);
    jce_camera_shake_offset(&b, pb, rb);
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(pa[i], pb[i]);
        TEST_ASSERT_EQUAL_FLOAT(ra[i], rb[i]);
    }
    /* Different seed → different shake (at least one component differs). */
    JceCameraShake c;
    jce_camera_shake_init(&c, 1.0f, 10.0f, 999u);
    jce_camera_shake_add_trauma(&c, 0.7f);
    for (int k = 0; k < 5; ++k) jce_camera_shake_update(&c, 0.05f);
    float pc[3], rc[3];
    jce_camera_shake_offset(&c, pc, rc);
    bool differs = false;
    for (int i = 0; i < 3; ++i)
        if (pc[i] != pa[i] || rc[i] != ra[i]) differs = true;
    TEST_ASSERT_TRUE(differs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trauma_clamp_and_amount);
    RUN_TEST(test_decay_to_zero);
    RUN_TEST(test_offset_zero_without_trauma);
    RUN_TEST(test_offset_bounded_by_amplitude);
    RUN_TEST(test_deterministic);
    return UNITY_END();
}
