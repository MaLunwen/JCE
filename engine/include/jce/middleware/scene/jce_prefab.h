/*
 * jce_prefab.h  Prefab system (P3-32).
 *
 * A "prefab" is a JSON scene file (jce.scene contract) that the editor
 * or runtime can instantiate into a live scene as a parented subtree.
 * Instances are tagged via JceEditorMeta.prefab_instance + prefab_path
 * so the editor can later show the link, jump-to-source, or apply
 * variant overrides.
 *
 * Format reuse: prefab files ARE scene files. There is no separate
 * "prefab schema". Convention: store under .prefab.json (e.g.
 * `assets/prefabs/enemy_grunt.prefab.json`). The serializer emits
 * the same envelope used by full scenes; loaders treat it identically.
 *
 * Layer: Middleware/Scene (Layer 4) — depends on Resource/scene_serial.
 */

#ifndef JCE_PREFAB_H
#define JCE_PREFAB_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene      JceScene;
typedef struct JceFileSystem JceFileSystem;
typedef uint64_t             JceEntity;

/* ── Save ─────────────────────────────────────────────────────────── */

/* Serialize a subtree (root + all descendants in the parent/child
 * hierarchy) to disk via host-path I/O. Use this from the editor after
 * the user picks "Save as Prefab" on a hierarchy node.
 *
 * `root` MUST be a live entity in `scene`. `path` is a host path; the
 * caller is responsible for any VFS write-dir prefixing. Returns true
 * on success. */
JCE_API bool jce_prefab_save_subtree(const JceScene *scene,
                                      JceEntity       root,
                                      const char     *path);

/* ── Instantiate ──────────────────────────────────────────────────── */

/* Load the prefab JSON at `virtual_path` (resolved through `fs` —
 * supports PAK + mounted dirs) and instantiate it into `scene` as a
 * new subtree. The returned root entity is the FIRST entity in the
 * additive load result; all created entities receive a JceEditorMeta
 * with `prefab_instance = true` and `prefab_path = virtual_path` so
 * the link is preserved across save/load.
 *
 * If `position_offset` is non-NULL, the root entity's transform.position
 * is translated by that vector before the first frame.
 *
 * Returns 0 on failure (missing file, parse error, empty prefab). */
JCE_API JceEntity jce_prefab_instantiate(JceScene             *scene,
                                          const JceFileSystem  *fs,
                                          const char           *virtual_path,
                                          const jce_vec3       *position_offset);

/* Convenience: instantiate from a host path (no VFS), used by editor
 * tooling that operates on disk paths directly. */
JCE_API JceEntity jce_prefab_instantiate_file(JceScene       *scene,
                                               const char     *path,
                                               const jce_vec3 *position_offset);

/* ── Queries ──────────────────────────────────────────────────────── */

/* Count live instances of a given prefab in the scene by scanning
 * JceEditorMeta.prefab_path. O(N) over entities — intended for editor
 * inspector / "find references", not per-frame use. */
JCE_API uint32_t jce_prefab_count_instances(const JceScene *scene,
                                              const char     *virtual_path);

/* ── Variant support ──────────────────────────────────────────────── */

/* Tag every entity in the subtree rooted at `root` as a variant of
 * the prefab at `parent_virtual_path`.  Sets EditorMeta.variant_parent_path
 * on each so the link round-trips through scene serialization.
 * Returns the number of entities tagged. */
JCE_API uint32_t jce_prefab_mark_as_variant(JceScene   *scene,
                                              JceEntity   root,
                                              const char *parent_virtual_path);

/* Save a subtree as a variant of an existing prefab.  Identical to
 * jce_prefab_save_subtree() except the saved file's entities carry
 * variant_parent_path so reloading reconstructs the chain.  The base
 * prefab file at `parent_virtual_path` is NOT modified — variants
 * are independent files that just remember their parent. */
JCE_API bool jce_prefab_save_subtree_as_variant(JceScene   *scene,
                                                  JceEntity   root,
                                                  const char *output_path,
                                                  const char *parent_virtual_path);

JCE_EXTERN_C_END

#endif /* JCE_PREFAB_H */
