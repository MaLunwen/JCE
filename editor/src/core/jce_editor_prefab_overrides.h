/*
 * jce_editor_prefab_overrides.h  Editor-side prefab override session.
 *
 * Owns a single JcePrefabOverrideSet representing the user's pending
 * changes against open prefab instances.  The Inspector reads this to
 * (a) show "(N overrides)" badges per entity, (b) wire Apply/Revert
 * buttons.  Full hook into scene save/load is a follow-up; this batch
 * just makes the data accessible from the editor UI layer.
 */

#ifndef JCE_EDITOR_PREFAB_OVERRIDES_H
#define JCE_EDITOR_PREFAB_OVERRIDES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePrefabOverrideSet JcePrefabOverrideSet;

/* Get the editor's session override set, lazily creating it on first
 * call.  Lifetime spans the editor process.  Never NULL. */
JcePrefabOverrideSet *jce_editor_prefab_overrides(void);

/* Convenience: count overrides recorded for `entity_path`. */
uint32_t jce_editor_prefab_overrides_count(const char *entity_path);

/* Apply all overrides under `entity_path` into the base prefab file
 * located at `base_path`.  Returns true on success.  On success the
 * applied entries are removed from the session set. */
bool jce_editor_prefab_overrides_apply(const char *base_path);

/* Drop every override recorded for `entity_path`.  Returns count
 * removed. */
uint32_t jce_editor_prefab_overrides_revert(const char *entity_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PREFAB_OVERRIDES_H */
