/*
 * ck_engine_smoke.h — Caged Kingdom acts as the integration test harness
 * for the JCE engine. This module exercises every Stage 17–26 module at
 * game startup (create → exercise → destroy + invariants), logging any
 * failures. The game continues to run regardless — failures are observed
 * via the log, never block launch.
 *
 * Hooked from ck_app's demo_init() before ck_app_create() returns.
 */
#ifndef CK_ENGINE_SMOKE_H
#define CK_ENGINE_SMOKE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns number of failed test cases (0 == all green). */
int ck_engine_smoke_run(void);

#ifdef __cplusplus
}
#endif

#endif /* CK_ENGINE_SMOKE_H */
