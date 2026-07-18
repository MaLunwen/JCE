/*
 * jce_editor_project_state.h  Per-project, machine-local editor state.
 *
 * Third scope in the settings architecture (2026-07-04 audit):
 *
 *   preferences  ~/.jce/editor-preferences.json   user x machine
 *   session      ~/.jce/editor-session.json       user x machine, last-state
 *   PROJECT      <root>/.jce/editor-state.json    user x machine x PROJECT
 *
 * Holds view/panel chrome that must not leak across projects (the audit's
 * scope errors: a debug view mode left on in project A used to greet you
 * in project B): per-project view state, per-scene camera poses and
 * bookmarks, asset-browser location, hierarchy filters, game-view module.
 * Machine-local — projects should git-ignore ".jce/" (jce_project.json's
 * scaffold already does).
 *
 * Anchored at the OPEN project's root via the same explicit-root pattern
 * as jce_project_settings / jce_pak_key.  While no project is open the
 * store is INERT: reads return fallbacks, writes are dropped — never a
 * stray ".jce" in the launch directory (audit risk #9's pre-warm bug).
 *
 * Same crash-safety model as the user config: values live on an in-memory
 * table, writes are debounced (~0.5s quiet) and flushed atomically; a
 * project-root switch flushes the outgoing project's state first.
 *
 * Per-scene keys: pass the scene's project-relative path as `scene`.
 * Scene sections are LRU-capped (most recent 24 scenes) so the file can't
 * grow without bound in a many-scene project.
 */

#ifndef JCE_EDITOR_PROJECT_STATE_H
#define JCE_EDITOR_PROJECT_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project-level keys. */
int   jce_editor_pstate_get_int(const char *key, int fallback);
void  jce_editor_pstate_set_int(const char *key, int value);
float jce_editor_pstate_get_float(const char *key, float fallback);
void  jce_editor_pstate_set_float(const char *key, float value);
bool  jce_editor_pstate_get_str(const char *key, char *out, size_t cap);
void  jce_editor_pstate_set_str(const char *key, const char *value);

/* Per-scene keys (scene = project-relative scene path; normalized
 * internally).  Sections beyond the 24 most recently touched scenes are
 * evicted at flush time. */
int   jce_editor_pstate_scene_get_int(const char *scene, const char *key,
                                      int fallback);
void  jce_editor_pstate_scene_set_int(const char *scene, const char *key,
                                      int value);
float jce_editor_pstate_scene_get_float(const char *scene, const char *key,
                                        float fallback);
void  jce_editor_pstate_scene_set_float(const char *scene, const char *key,
                                        float value);
bool  jce_editor_pstate_scene_get_str(const char *scene, const char *key,
                                      char *out, size_t cap);
void  jce_editor_pstate_scene_set_str(const char *scene, const char *key,
                                      const char *value);

/* True when a project root is known (reads/writes are live). */
bool  jce_editor_pstate_active(void);

/* Debounce pump + forced flush — mirror jce_editor_config_flush_*.
 * flush_tick also detects project-root changes (flush old, reload new). */
void  jce_editor_pstate_flush_tick(float dt_sec);
void  jce_editor_pstate_flush_now(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PROJECT_STATE_H */
