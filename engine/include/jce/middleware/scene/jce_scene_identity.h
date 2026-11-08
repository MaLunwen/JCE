/*
 * jce_scene_identity.h  Opaque scene/entity identities and cache generations.
 *
 * Scene-owned cache observation and invalidation signals. jce_scene.h
 * re-exports this standalone header for existing scene consumers.
 */

#ifndef JCE_SCENE_IDENTITY_H
#define JCE_SCENE_IDENTITY_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── Entity handle ───────────────────────────────────────────────── */

typedef uint64_t JceEntity;
#define JCE_ENTITY_INVALID 0

/* ── Scene ───────────────────────────────────────────────────────── */

typedef struct JceScene JceScene;

/* Monotonic counter bumped only by jce_scene_invalidate_world_cache (i.e. by a
 * real structural edit), never by the per-frame drops.  The scene renderer keys
 * its cross-frame persistent static world-matrix + AABB cache on this. */
JCE_API uint64_t  jce_scene_get_structural_epoch(const JceScene *s);

/* Per-entity transform generation (dynamic-scene world-cache, large-world opt).
 * set_transform / set_pivot bump ONLY the moved entity's subtree's gen instead
 * of the global structural epoch, so a moving camera / script-animated entity no
 * longer drops the renderer's static world-cache for the whole scene.  The
 * renderer keys a static entity's cache validity on BOTH the structural epoch
 * (global edits) AND this per-entity gen (transform edits).  jce_scene_xgen_active
 * is false until the first per-entity bump, so a never-moved scene stays on the
 * cheap epoch-only fast path. */
JCE_API uint64_t  jce_scene_entity_xform_gen(const JceScene *s, JceEntity e);
JCE_API bool      jce_scene_xgen_active(const JceScene *s);
JCE_API void      jce_scene_invalidate_entity_world(JceScene *s, JceEntity e);

/* Frame-invariance generation counters (DOTS-floor slice 1).  Together with
 * structural_epoch they form the frozen-frame key a cross-frame consumer can
 * compare to prove "nothing observable changed since last frame":
 *  - roster_epoch:  entity create/destroy (flecs swap-remove silently reorders
 *    table rows, so ANY roster change invalidates positional index caches).
 *  - enable_gen:    any EditorMeta.enabled flip (routed through the editor's
 *    single jce_state_set_entity_enabled entry; bumped via
 *    jce_scene_bump_enable_gen because the flag is mutated in place).
 *  - xform_counter: bumped by every jce_scene_invalidate_entity_world call
 *    (set_transform/set_pivot subtree bumps) AND by the runtime physics
 *    write-back (jce_scene_notify_physics_writeback) which otherwise mutates
 *    Transforms in place with no signal — one integer compare answers "did
 *    ANY world matrix change" without probing 150k per-entity gens. */
JCE_API uint64_t  jce_scene_get_roster_epoch(const JceScene *s);
JCE_API uint64_t  jce_scene_get_enable_gen(const JceScene *s);
/* Bumped whenever any component's data is written through jce_scene_set_*.
 * Cache keys that memoize per-entity state derived from component FIELDS
 * must include this, or a runtime edit to that field will not take effect. */
JCE_API uint64_t  jce_scene_get_cull_data_gen(const JceScene *s);
JCE_API void      jce_scene_bump_cull_data_gen(JceScene *s);
JCE_API uint64_t  jce_scene_get_xform_counter(const JceScene *s);
JCE_API void      jce_scene_bump_enable_gen(JceScene *s);
JCE_API void      jce_scene_notify_physics_writeback(JceScene *s);
/* Per-entity variant: the writer KNOWS which entity it mutated in place —
 * ring-push it (plus descendants) so the renderer's L2 incremental repair
 * rebuilds just those slots instead of the whole frame.  Falls back to
 * dirty-overflow (== the blanket notify) when the ring/subtree bounds hit. */
JCE_API void      jce_scene_notify_physics_writeback_entity(JceScene *s, JceEntity e);

/* Drain the dirty-entity ring (DOTS-floor L2): every entity whose world was
 * invalidated since the last take (subtree-expanded).  out may be NULL to
 * discard.  *out_overflow is set when the ring overflowed OR an in-place
 * mutation with an unknown entity set occurred (physics write-back) — the
 * consumer must then treat the whole scene as dirty. */
JCE_API uint32_t  jce_scene_take_dirty_entities(JceScene *s, JceEntity *out,
                                                uint32_t cap, bool *out_overflow);
/* Read-only variant (does not clear) — for consumers earlier in the frame
 * than the taker (the collect membership check runs before the ecull
 * repair, which owns the take). */
JCE_API uint32_t  jce_scene_peek_dirty_entities(const JceScene *s, JceEntity *out,
                                                uint32_t cap, bool *out_overflow);

/* Per-entity MATERIAL generation (lever ③ persistent draw-cmd cache). Mirrors
 * xform_gen for material/mesh content: bumped by the MeshRenderer set path and
 * async texture/model pop-in via jce_scene_invalidate_entity_material. A cache
 * stores the mat_gen it saw and rebuilds the entity's draw command when it
 * differs. Shares the xgen slot table (jce_scene_xgen_active gates both). */
JCE_API uint64_t  jce_scene_entity_material_gen(const JceScene *s, JceEntity e);
JCE_API void      jce_scene_invalidate_entity_material(JceScene *s, JceEntity e);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_IDENTITY_H */
