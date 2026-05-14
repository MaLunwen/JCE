/*
 * jce_prefab_nested.h  Nested-prefab instantiation.
 *
 * The existing jce_prefab_instantiate_file() loads a flat subtree.
 * "Nested prefabs" are entities whose JceEditorMeta.prefab_path is
 * non-empty AND points to another prefab file — when those are
 * loaded, the leaf entity should be replaced by a freshly-instantiated
 * copy of the referenced prefab subtree.  Unity calls this a
 * "Nested Prefab".
 *
 * This module provides the recursive instantiator on top of the
 * existing flat loader.  Cycle detection caps recursion depth.
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_PREFAB_NESTED_H
#define JCE_PREFAB_NESTED_H

#include <jce/middleware/scene/jce_prefab.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Recursively instantiate the prefab at `virtual_path` into `scene`,
 * expanding any descendant entities that themselves carry a non-
 * empty prefab_path metadata field.  Maximum recursion depth caps at
 * `max_depth` (default 8 when 0 is passed) to defend against cycles.
 *
 * Returns the root entity of the outer prefab, or JCE_ENTITY_INVALID
 * on failure (file missing, malformed, recursion limit).
 *
 * `position_offset` may be NULL — same semantics as
 * jce_prefab_instantiate_file. */
JCE_API JceEntity jce_prefab_instantiate_nested(JceScene       *scene,
                                                 const char     *virtual_path,
                                                 const jce_vec3 *position_offset,
                                                 int             max_depth);

/* Recursive count: number of nested-prefab expansions a given top-
 * level prefab file performs (children-of-children with their own
 * prefab_path).  Useful for editor UI to surface "this prefab
 * contains 3 nested prefabs". */
JCE_API uint32_t jce_prefab_count_nested_refs(const JceScene *scene,
                                               JceEntity       root);

JCE_EXTERN_C_END

#endif /* JCE_PREFAB_NESTED_H */
