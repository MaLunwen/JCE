/*
 * jce_prefab.h  Prefab system (P3-32).
 *
 * A "prefab" is a JSON scene file (jce.scene contract) that the editor
 * or runtime can instantiate into a live scene as a parented subtree.
 * Instances are tagged via JceEditorMeta.prefab_instance + prefab_path
 * so the editor can later show the link, jump-to-source, or apply
 * variant overrides.
 *
 * Convention: store under .prefab.json (e.g.
 * `assets/prefabs/enemy_grunt.prefab.json`).
 *
 * On-disk formats — the loader accepts BOTH:
 *   1. Flat scene form  {contract, scene:{version, entities:[{id, parentId,
 *      components}, ...]}} — what jce_prefab_save_subtree() below emits;
 *      identical to a full scene file, so loaders treat it identically.
 *   2. Nested node-tree form  {contract, prefab:{version, root:{name,
 *      components, children:[...]}}} — what the editor's "Save as Prefab"
 *      writes, where parenthood is structural and nodes carry no id.
 * Both land on the same component parsers and produce the same entities;
 * only the way parenthood is expressed differs.
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

JCE_EXTERN_C_END

#endif /* JCE_PREFAB_H */
