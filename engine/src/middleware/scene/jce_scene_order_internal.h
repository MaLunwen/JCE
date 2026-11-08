/* Private scene-authored order; never derive painter/layout order from a
 * recycled ECS handle or its generation. No public component/ABI is added. */
#ifndef JCE_SCENE_ORDER_INTERNAL_H
#define JCE_SCENE_ORDER_INTERNAL_H

#include <jce/middleware/scene/jce_scene.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Scene-local creation ordinal, unchanged by table migration/reparenting.
 * Zero means dead/unmanaged entity; only public scene creation authors it. */
uint64_t jce_scene_internal_authored_order(const JceScene *scene, JceEntity entity);

/* In-place, allocation-free O(n log n) sort; no global comparator context.
 * Reject dead/unmanaged entities and changed rosters, with no handle fallback. */
bool jce_scene_internal_sort_entities(const JceScene *scene,
                                      JceEntity *entities, size_t count);

/* Snapshot handles before visitors; preserve canonical authored array order.
 * Refuse OOM, changed roster, dead/unmanaged entity, not an unordered save.
 * The existing one-shot/streaming loader already creates in array order. */
bool jce_scene_internal_each_authored(JceScene *scene,
                                      JceEntityCallback visitor, void *user);

/* Root-first depth-first traversal, authored sibling order, no recursive stack.
 * Used by canonical prefab save; false makes the caller discard its tree. */
bool jce_scene_internal_each_subtree_authored(JceScene *scene, JceEntity root,
                                              JceEntityCallback visitor, void *user);

JCE_EXTERN_C_END
#endif
