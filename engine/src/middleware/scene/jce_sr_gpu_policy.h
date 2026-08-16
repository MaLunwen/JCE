/*
 * jce_sr_gpu_policy.h
 *
 * Pure workload policy for the scene renderer's GPU Scene / MDI path.
 * Kept independent of bgfx so the fixed-cost crossover remains unit-testable.
 */

#ifndef JCE_SR_GPU_POLICY_H
#define JCE_SR_GPU_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct JceSrGpuPolicy {
    uint32_t min_records;
    uint32_t min_group_records;
    uint32_t min_records_per_draw;
} JceSrGpuPolicy;

static inline JceSrGpuPolicy jce_sr_gpu_policy_default(void)
{
    JceSrGpuPolicy policy;
    policy.min_records = 512u;
    policy.min_group_records = 8u;
    policy.min_records_per_draw = 4u;
    return policy;
}

static inline bool jce_sr_gpu_policy_group_eligible(
    const JceSrGpuPolicy *policy, uint32_t records)
{
    uint32_t minimum = policy && policy->min_group_records
        ? policy->min_group_records : 1u;
    return records >= minimum;
}

static inline bool jce_sr_gpu_policy_batch_eligible(
    const JceSrGpuPolicy *policy, uint32_t records,
    uint32_t draw_runs, bool force)
{
    uint32_t min_records;
    uint32_t min_per_draw;

    if (records == 0u || draw_runs == 0u)
        return false;
    if (force)
        return true;

    min_records = policy && policy->min_records
        ? policy->min_records : 1u;
    min_per_draw = policy && policy->min_records_per_draw
        ? policy->min_records_per_draw : 1u;

    return records >= min_records &&
           records / draw_runs >= min_per_draw;
}

#endif /* JCE_SR_GPU_POLICY_H */
