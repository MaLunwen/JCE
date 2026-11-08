/* Boot-only worker policy. Pure calculation; no scheduler/config mirror. */
#ifndef JCE_WORKER_BUDGET_INTERNAL_H
#define JCE_WORKER_BUDGET_INTERNAL_H
#include <stdbool.h>

static inline int jce_worker_budget_default(bool low_memory, int logical_cores)
{
    if (low_memory || logical_cores <= 2) return 1;
    return logical_cores > 9 ? 8 : logical_cores - 1;
}

/* Explicit INI pins keep the existing shared-frame 1..16 policy.
 * Async owners separately retain their existing maximum of eight workers. */
static inline int jce_worker_budget_requested(bool low_memory, int logical_cores,
                                               int configured_workers)
{
    if (configured_workers > 0)
        return configured_workers > 16 ? 16 : configured_workers;
    return jce_worker_budget_default(low_memory, logical_cores);
}
/* Private boot publication and readback use the existing shared-pool budget.
 * Public machine-class accessors retain authored values; no second state is
 * stored here and reading the budget does not create a scheduler. */
#ifdef __cplusplus
extern "C" {
#endif
void jce_config_publish_perf_resolved(int machine_class, int job_workers,
                                      bool low_memory);
int jce_thread_pool_shared_worker_budget(void);
#ifdef __cplusplus
}
#endif
#endif
