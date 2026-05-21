/*
 * jce_build_manager.h  CMake-driven build orchestration.
 *
 * Spawns `cmake --preset <p>` (configure) and `cmake --build --preset <p>`
 * (compile) child processes, drains their stdout/stderr to the editor
 * console with a "[build]" prefix, and exposes a small status struct so
 * the Build Settings dialog can render a progress / log strip.
 *
 * One build at a time.  Re-entrancy returns false from start_*().  The
 * dialog UI is responsible for calling jce_build_manager_poll() each
 * frame so completion / log lines are picked up.
 *
 * Threading: all entry points are expected from the editor main thread.
 *
 * Layer: Editor (depends on jce_process from os/core).
 */

#ifndef JCE_BUILD_MANAGER_H
#define JCE_BUILD_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_BUILD_IDLE      = 0,
    JCE_BUILD_RUNNING   = 1, /* configure or compile in flight */
    JCE_BUILD_SUCCEEDED = 2,
    JCE_BUILD_FAILED    = 3
} JceBuildState;

typedef enum {
    JCE_BUILD_STAGE_NONE      = 0,
    JCE_BUILD_STAGE_CONFIGURE = 1,
    JCE_BUILD_STAGE_COMPILE   = 2
} JceBuildStage;

typedef struct {
    JceBuildState state;
    JceBuildStage stage;
    int           exit_code;
    /* The preset name in flight (or last finished). */
    char          preset[128];
    /* Last error text — populated when state == JCE_BUILD_FAILED. */
    char          last_error[256];
} JceBuildStatus;

void jce_build_manager_init(void);
void jce_build_manager_shutdown(void);

/* Drain pipes, detect exit.  Call once per editor frame. */
void jce_build_manager_poll(void);

/* Run `cmake --preset <preset>` (configure step).  Returns false if a
 * build is already running or the spawn fails. */
bool jce_build_manager_configure(const char *preset);

/* Run `cmake --build --preset <preset>` (compile step).  Returns false
 * if a build is already running or the spawn fails. */
bool jce_build_manager_build(const char *preset);

/* Run `cmake --build --preset <preset> --target PackGameAssets` —
 * targeted incremental repack used by the editor's save/launch hooks
 * to close the "designer edits → game sees change" loop without
 * rebuilding the whole project.  CMake-side mtime tracking keeps the
 * cost near-zero when no inputs changed.  Returns false if a build is
 * already running or the spawn fails. */
bool jce_build_manager_repack_game_assets(const char *preset);

/* Request graceful stop (SIGINT / Ctrl+C semantics).  Falls back to
 * force-kill after ~5 s if the child does not exit. */
void jce_build_manager_request_stop(void);

bool jce_build_manager_is_running(void);
void jce_build_manager_get_status(JceBuildStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* JCE_BUILD_MANAGER_H */
