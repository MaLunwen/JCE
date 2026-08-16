/*
 * jce_component_registry.h  Dense component-type registry (public surface).
 *
 * Industry-standard replacement for the exhausted 64-bit JCE_COMP_FLAG_*
 * space (the TypeManager pattern: Unity DOTS / EnTT / flecs all key
 * components by a dense registered type id, never by a fixed bitmask).
 *
 * A component is identified by a small dense `comp_id` — its row in the
 * registry, assigned in registration order at first scene creation.
 * Presence is answered by the ECS itself (flecs archetypes) through the
 * row's accessor, so there is no per-entity mirror mask to keep in sync
 * and no upper limit beyond JCE_COMP_MAX.
 *
 * The legacy `uint64_t` flag surface (JCE_COMP_FLAG_*) remains valid:
 * every pre-existing component row carries its legacy bit, and the old
 * flag-based queries (jce_scene_get_component_flags /
 * jce_scene_component_enabled) are shims over this registry.  New
 * components get `legacy_flag == 0` and are reachable only by id/name —
 * the synthetic editor-slot workaround is superseded by this registry.
 *
 * Registration happens once, engine-side (jce_scene_create), from the
 * component serializer module — the registry row carries the JSON type
 * name (and accepted aliases), so scene serialization, the editor's
 * Add-Component surface and presence queries all share one table.
 */

#ifndef JCE_COMPONENT_REGISTRY_H
#define JCE_COMPONENT_REGISTRY_H

#include <jce/middleware/scene/jce_scene.h>

JCE_EXTERN_C_BEGIN

#define JCE_COMP_ID_INVALID (-1)
/* Hard cap on registered component types (4 enable-state words). */
#define JCE_COMP_MAX 256

/* ── Registry queries ─────────────────────────────────────────────── */

/* Number of registered component types (0 before the first scene). */
JCE_API int         jce_component_count(void);

/* Canonical JSON type name of a row ("MeshRenderer", "NavAgent", ...).
 * NULL for an invalid id. */
JCE_API const char *jce_component_name(int comp_id);

/* The row's legacy JCE_COMP_FLAG_* bit, or 0 for components registered
 * after the 64-bit space filled. */
JCE_API uint64_t    jce_component_legacy_flag(int comp_id);

/* Look a row up by canonical name OR any registered alias spelling.
 * Returns JCE_COMP_ID_INVALID when unknown. */
JCE_API int         jce_component_find(const char *name);

/* Map a single legacy flag bit to its row (JCE_COMP_ID_INVALID if the
 * bit is unassigned/retired). */
JCE_API int         jce_component_from_legacy_flag(uint64_t single_bit_flag);

/* ── Per-entity queries (presence from flecs, no mirror state) ────── */

JCE_API bool jce_scene_has_comp(const JceScene *s, JceEntity e, int comp_id);
JCE_API void jce_scene_remove_comp(JceScene *s, JceEntity e, int comp_id);

/* Type-erased component access through the registry row's accessors.
 * jce_scene_get_comp returns the live component pointer and writes its
 * struct size to *out_size (when non-NULL); returns NULL (size 0) when
 * the entity lacks the component or the row has no direct accessor
 * (unified Light, entity-level Tag/Layer).  jce_scene_set_comp copies
 * the row's struct_size bytes from `data` into the entity's component,
 * adding it when absent; returns false when the row has no setter. */
JCE_API void *jce_scene_get_comp(JceScene *s, JceEntity e, int comp_id,
                                 uint32_t *out_size);
JCE_API bool  jce_scene_set_comp(JceScene *s, JceEntity e, int comp_id,
                                 const void *data);

/* Per-component enable toggle, id-keyed — works for EVERY registered
 * component (unlike the legacy 64-bit disabled mask).  Default enabled. */
JCE_API bool jce_scene_comp_enabled(const JceScene *s, JceEntity e, int comp_id);

/* True when ANY entity in the scene carries a per-component disable.
 *
 * jce_scene_comp_enabled already short-circuits on this, but only AFTER an
 * ecs_is_alive() -- a flecs sparse-set lookup that every call pays even when
 * the answer is "nothing in this scene is disabled".  At 25.7k visible entities
 * that was 71 cycles each, 12.9% of the whole submit loop, to re-derive one
 * scene-wide fact.
 *
 * Hot loops hoist this out and skip the per-entity call entirely.  Doing so is
 * exact, not approximate: with no disable rows the per-entity function can only
 * return false for a DEAD entity, and every caller that matters already drops
 * dead entities because their component pointer comes back NULL. */
JCE_API bool jce_scene_has_component_disables(const JceScene *s);

/* Same answer as jce_scene_comp_enabled, minus the liveness check.
 *
 * That check is an ecs_is_alive() -- a flecs sparse-set lookup -- and it runs
 * before every short-circuit, so it is what the query actually costs: 71 cycles
 * per visible entity in the 200k bench, 11% of the whole submit loop, and
 * neither the scene-wide "nothing is disabled" flag nor the disable-id set
 * moved it, which is how it was identified.
 *
 * THE CALLER MUST HAVE ESTABLISHED THAT `e` IS ALIVE. Holding a non-NULL
 * component pointer for it is proof; being in a list built this frame is not,
 * because a script can delete an entity mid-frame. */
JCE_API bool jce_scene_comp_enabled_alive(const JceScene *s, JceEntity e,
                                          int comp_id);
/* Legacy-flag form, mirroring jce_scene_component_enabled. */
JCE_API bool jce_scene_component_enabled_alive(const JceScene *s, JceEntity e,
                                               uint64_t flag);
/* How many entities carry a disable row (the set the hot path must consult). */
JCE_API int32_t jce_scene_component_disable_count(const JceScene *s);
JCE_API void jce_scene_set_comp_enabled(JceScene *s, JceEntity e, int comp_id,
                                        bool enabled);

/* Authoring attribution for the toggle above.  A script that re-asserts a
 * component's enable state every frame silently overwrites anything the user
 * does in the Inspector, which reads as a broken checkbox rather than as
 * "something else owns this".  The script bridge marks what it writes; the
 * editor asks, and says so.  Run-scoped: never serialized, dropped when the
 * scene is restored.  Marking is idempotent and costs nothing after the
 * first write, so the per-frame re-assert path stays cheap. */
JCE_API void jce_scene_mark_comp_script_driven(JceScene *s, JceEntity e,
                                               int comp_id);
JCE_API bool jce_scene_comp_script_driven(const JceScene *s, JceEntity e,
                                          int comp_id);

JCE_EXTERN_C_END

#endif /* JCE_COMPONENT_REGISTRY_H */
