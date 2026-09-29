/*
 * test_jce_sr_gpu_policy.c
 *
 * Pure workload-policy tests for the scene renderer's GPU Scene / MDI path.
 * The policy must keep fixed compute/upload costs away from small or highly
 * fragmented batches while retaining an explicit force mode for diagnostics.
 */

#include "jce_sr_gpu_policy.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_small_batch_stays_on_cpu(void)
{
    JceSrGpuPolicy policy = jce_sr_gpu_policy_default();

    TEST_ASSERT_FALSE(jce_sr_gpu_policy_batch_eligible(
        &policy, 98u, 55u, false));
}

static void test_large_coherent_batch_uses_gpu_scene(void)
{
    JceSrGpuPolicy policy = jce_sr_gpu_policy_default();

    TEST_ASSERT_TRUE(jce_sr_gpu_policy_group_eligible(&policy, 4096u));
    TEST_ASSERT_TRUE(jce_sr_gpu_policy_batch_eligible(
        &policy, 4096u, 8u, false));
}

static void test_many_tiny_draws_stay_on_cpu(void)
{
    JceSrGpuPolicy policy = jce_sr_gpu_policy_default();

    TEST_ASSERT_FALSE(jce_sr_gpu_policy_group_eligible(&policy, 2u));
    TEST_ASSERT_FALSE(jce_sr_gpu_policy_batch_eligible(
        &policy, 4096u, 2048u, false));
}

static void test_force_mode_keeps_diagnostic_path_available(void)
{
    JceSrGpuPolicy policy = jce_sr_gpu_policy_default();

    TEST_ASSERT_TRUE(jce_sr_gpu_policy_batch_eligible(
        &policy, 1u, 1u, true));
    TEST_ASSERT_FALSE(jce_sr_gpu_policy_batch_eligible(
        &policy, 0u, 0u, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_small_batch_stays_on_cpu);
    RUN_TEST(test_large_coherent_batch_uses_gpu_scene);
    RUN_TEST(test_many_tiny_draws_stay_on_cpu);
    RUN_TEST(test_force_mode_keeps_diagnostic_path_available);
    return UNITY_END();
}
