/*
 * api_save.h  Save games / persistence.
 *
 * Slot management, versioned migration and the pluggable storage providers
 * behind them. AGENTS.md §4 promised the whole of middleware through
 * api_middleware.h; this subsystem was reachable from no umbrella at all
 * until 2026-08-31.
 */

#ifndef JCE_API_SAVE_H
#define JCE_API_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/save/jce_save_migration.h>
#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/save/jce_snapshot.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_SAVE_H */
