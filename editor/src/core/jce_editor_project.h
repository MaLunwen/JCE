/*
 * jce_editor_project.h  Editor-side loaded-project state.
 *
 * Wraps the engine's JceProject (jce_project_load) for the editor.
 * Other panels (build profiles, run manager, etc.) consult this to
 * pre-fill UI from jce_project.json when a project is open.
 *
 * Lifecycle: rebuilt every time set_current_project_root() is called.
 * When the open root has no jce_project.json the project pointer is
 * NULL — callers must handle that as "engine workspace mode" or
 * "asset folder only" depending on context.
 */

#ifndef JCE_EDITOR_PROJECT_H
#define JCE_EDITOR_PROJECT_H

#include <jce/application/jce_project.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by set_current_project_root() (dialog_project.cpp).  Reloads
 * jce_project.json under the new root.  Safe to call with NULL/"". */
void jce_editor_project_set_root(const char *root);

/* Returns the cached JceProject for the current root, or NULL if no
 * jce_project.json was found.  The pointer is owned by this module —
 * do NOT free it. */
const JceProject *jce_editor_project_get(void);

/* True if the current root looks like the JCE engine source tree
 * (contains engine/include/jce/api.h).  Used by the build profile
 * panel to keep backward-compatible "engine dev" behaviour. */
bool jce_editor_project_is_engine_workspace(void);

/* Persist a single string-typed field to <project_root>/jce_project.json,
 * then refresh the in-memory cache so subsequent jce_editor_project_get()
 * calls observe the new value.  `field` accepts the same names as
 * jce_project_set_field().  Returns false if no project is open or the
 * save fails. */
bool jce_editor_project_update_field(const char *field, const char *value);

/* Replace the project's bundles[] array wholesale.  Persists to
 * jce_project.json and reloads the cache.  Pass NULL/0 to clear. */
bool jce_editor_project_set_bundles(const char *const *paths, int count);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PROJECT_H */
