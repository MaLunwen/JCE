/* test_jce_easing.c
 *
 * Unit tests for the easing library (jce_easing):
 *   - endpoints: every easing maps 0->~0 and 1->~1
 *   - t clamped to [0,1]
 *   - known reference values for the polynomial eases
 *   - InOut symmetry about 0.5
 *   - jce_ease_lerp maps a..b; names are non-empty and unique-ish
 */

#include <jce/os/core/jce_easing.h>

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_endpoints(void)
{
    for (int e = 0; e < JCE_EASE_COUNT; ++e) {
        float at0 = jce_ease((JceEaseType)e, 0.0f);
        float at1 = jce_ease((JceEaseType)e, 1.0f);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, at0);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, at1);
    }
}

static void test_clamp(void)
{
    /* Out-of-range t is clamped, so it matches the endpoints. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, jce_ease(JCE_EASE_CUBIC_IN, -5.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, jce_ease(JCE_EASE_CUBIC_IN,  5.0f));
}

static void test_known_values(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.5f,    jce_ease(JCE_EASE_LINEAR,   0.5f));
    TEST_ASSERT_EQUAL_FLOAT(0.25f,   jce_ease(JCE_EASE_QUAD_IN,  0.5f));   /* 0.5²   */
    TEST_ASSERT_EQUAL_FLOAT(0.125f,  jce_ease(JCE_EASE_CUBIC_IN, 0.5f));   /* 0.5³   */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, jce_ease(JCE_EASE_QUAD_OUT, 0.5f));
    /* InOut passes through 0.5 at t=0.5 (odd symmetry of the standard set). */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_ease(JCE_EASE_QUAD_INOUT,  0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_ease(JCE_EASE_CUBIC_INOUT, 0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, jce_ease(JCE_EASE_SINE_INOUT,  0.5f));
}

static void test_lerp(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, jce_ease_lerp(JCE_EASE_LINEAR, 10.0f, 20.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.0f, jce_ease_lerp(JCE_EASE_LINEAR, 10.0f, 20.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 12.5f, jce_ease_lerp(JCE_EASE_QUAD_IN, 10.0f, 20.0f, 0.5f)); /* 10+10*0.25 */
}

static void test_names(void)
{
    for (int e = 0; e < JCE_EASE_COUNT; ++e) {
        const char *n = jce_ease_name((JceEaseType)e);
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_TRUE(strlen(n) > 0);
    }
    TEST_ASSERT_EQUAL_STRING("Linear", jce_ease_name(JCE_EASE_LINEAR));
    TEST_ASSERT_EQUAL_STRING("BounceOut", jce_ease_name(JCE_EASE_BOUNCE_OUT));
    /* Out-of-range → "Linear" fallback. */
    TEST_ASSERT_EQUAL_STRING("Linear", jce_ease_name((JceEaseType)9999));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_endpoints);
    RUN_TEST(test_clamp);
    RUN_TEST(test_known_values);
    RUN_TEST(test_lerp);
    RUN_TEST(test_names);
    return UNITY_END();
}
