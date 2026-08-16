/*
 * jce_cook_manager.h  Editor-side wrapper around jce_cook_*.
 *
 * Submits the engine cook as structured background work, reports progress
 * through the Console + status line, and exposes
 * a small state machine so the toolbar "▶ Play" button can chain
 * Cook → Build → Run.
 *
 * Layer: Editor (depends on jce_application's jce_cook + jce_async).
 * One cook at a time.  Re-entrancy returns false from start().
 */

#ifndef JCE_COOK_MANAGER_H
#define JCE_COOK_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_COOK_IDLE      = 0,
    JCE_COOK_RUNNING   = 1,
    JCE_COOK_SUCCEEDED = 2,
    JCE_COOK_FAILED    = 3,
} JceCookMgrState;

typedef struct {
    JceCookMgrState state;
    int             total;
    int             cooked;
    int             skipped;
    int             failed;
    char            last_error[256];
    char            project_root[1024];
} JceCookMgrStatus;

void jce_cook_manager_init(void);
void jce_cook_manager_shutdown(void);

/* Compatibility pump hook. Structured completion is driven by the engine's
 * default executor, so this is cheap and has no private thread to join. */
void jce_cook_manager_poll(void);

/* Kick a full cook for `project_root`.  Returns false if a cook is
 * already running or the project cannot be loaded. */
bool jce_cook_manager_start(const char *project_root);

bool jce_cook_manager_is_running(void);
void jce_cook_manager_get_status(JceCookMgrStatus *out);

/* Synchronous freshness probe.  Cheap (single mtime sweep).  Returns
 * true when a follow-up cook would do nothing.  Safe to call from the
 * UI thread. */
bool jce_cook_manager_is_up_to_date(const char *project_root);

#ifdef __cplusplus
}
#endif

#endif /* JCE_COOK_MANAGER_H */
