/*
 * jce_prefab_overrides.h  Per-instance prefab override patches.
 *
 * Each prefab instance in a scene can carry a sparse list of property
 * overrides — "this entity's transform.position differs from the base
 * prefab's authored value".  The override patch lives in a sidecar
 * file alongside the scene (e.g. `level1.scene.json` →
 * `level1.scene.overrides.json`) so the base prefab JSON stays the
 * single source of truth and instances only record their deltas.
 *
 * Mirrors Unity's prefab override system at the data-layer.  Editor
 * UX (Apply / Revert buttons in the Inspector, change-tracking on
 * component writes) is layered on top via the editor — those are
 * Batch 5+ tasks; this module only owns the data structure and
 * load/save plumbing.
 *
 * Overrides are addressed by:
 *   entity_path  — slash-separated path of EditorMeta.name within the
 *                  prefab subtree, rooted at the prefab root entity.
 *                  e.g. "" = root, "Body/Head" = grand-child named "Head".
 *   component_id — JCE_COMP_FLAG_* bit (transform / mesh-renderer / ...).
 *   field_path   — JSON pointer-like path into the component, e.g.
 *                  "position.x", "baseColor", "albedoTex".
 *
 * Values are stored as JSON-string-typed blobs to keep the data model
 * uniform regardless of field type — runtime apply parses the string
 * back into the field according to component_id + field_path.
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_PREFAB_OVERRIDES_H
#define JCE_PREFAB_OVERRIDES_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePrefabOverrideSet JcePrefabOverrideSet;

/* A single override entry.  Strings are owned by the parent set —
 * obtained via jce_prefab_overrides_get_at and only valid until the
 * set is mutated. */
typedef struct {
    const char *entity_path;   /* "" for root; never NULL */
    uint64_t    component_id;  /* one of JCE_COMP_FLAG_* */
    const char *field_path;
    const char *value_json;    /* raw JSON literal, e.g. "1.5" or "[1,0,0]" */
} JcePrefabOverride;

/* Lifecycle. */
JCE_API JcePrefabOverrideSet *jce_prefab_overrides_create(void);
JCE_API void                  jce_prefab_overrides_destroy(JcePrefabOverrideSet *set);

/* Add or replace an override.  Returns false on OOM or capacity
 * exhaustion (fixed cap of 1024 per set in this first cut). */
JCE_API bool jce_prefab_overrides_set(JcePrefabOverrideSet *set,
                                      const char *entity_path,
                                      uint64_t    component_id,
                                      const char *field_path,
                                      const char *value_json);

/* Remove an override.  Returns true if found and removed. */
JCE_API bool jce_prefab_overrides_clear_one(JcePrefabOverrideSet *set,
                                            const char *entity_path,
                                            uint64_t    component_id,
                                            const char *field_path);

/* Drop every override. */
JCE_API void jce_prefab_overrides_clear_all(JcePrefabOverrideSet *set);

/* Iteration. */
JCE_API uint32_t                 jce_prefab_overrides_count(const JcePrefabOverrideSet *set);
JCE_API JcePrefabOverride        jce_prefab_overrides_get_at(const JcePrefabOverrideSet *set,
                                                              uint32_t index);

/* Persistence — writes a flat JSON file with shape:
 *
 *   { "overrides": [
 *       { "path": "", "component": "transform", "field": "position",
 *         "value": [1.0, 2.0, 3.0] },
 *       ...
 *   ] }
 *
 * Returns true on success. */
JCE_API bool jce_prefab_overrides_save_to_file(const JcePrefabOverrideSet *set,
                                                const char *path);
JCE_API bool jce_prefab_overrides_load_from_file(JcePrefabOverrideSet *set,
                                                  const char *path);

/* ── Apply / Revert helpers (Unity prefab Apply / Revert) ─────────── */

/* Number of overrides whose `entity_path` exactly matches.  Used by
 * editor UX to show "(N overrides)" badges on entity rows. */
JCE_API uint32_t jce_prefab_overrides_count_for_entity(const JcePrefabOverrideSet *set,
                                                       const char *entity_path);

/* Total number of overrides (alias for count) — convenience for UX
 * code that wants "Has overrides?" style decisions. */
JCE_API bool jce_prefab_overrides_has_any(const JcePrefabOverrideSet *set);

/* Drop every override whose `entity_path` matches.  Returns the
 * number of entries removed.  Mirrors Unity "Revert all on this
 * GameObject". */
JCE_API uint32_t jce_prefab_overrides_revert_entity(JcePrefabOverrideSet *set,
                                                    const char *entity_path);

/* Drop overrides whose entity_path AND component_id both match —
 * equivalent to Unity's per-component "Revert" button. */
JCE_API uint32_t jce_prefab_overrides_revert_component(JcePrefabOverrideSet *set,
                                                       const char *entity_path,
                                                       uint64_t    component_id);

/* Apply: rewrite the base prefab JSON file at `base_prefab_path` with
 * every override merged in, then clear the override set.  Returns
 * true on success.  This is destructive against the base prefab — the
 * caller is responsible for any backups / version-control safety.
 * Mirrors Unity "Apply All to Prefab". */
JCE_API bool jce_prefab_overrides_apply_to_base(JcePrefabOverrideSet *set,
                                                 const char *base_prefab_path);

JCE_EXTERN_C_END

#endif /* JCE_PREFAB_OVERRIDES_H */
