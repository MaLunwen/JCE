/* jce_bundle_loader.h
 *
 * Runtime API for loading scene-asset bundles into a JceFileSystem.
 *
 * Workflow:
 *   1. Open a catalog file (`bundle_catalog.json`) produced by
 *      `jce_bundle_pack`.  The catalog enumerates every bundle in a
 *      build, its on-disk filename relative to the catalog, declared
 *      bundle dependencies, and a content hash for verification.
 *   2. Call `jce_bundle_mount(cat, "scene_forest")` to mount that
 *      bundle on the engine's JceFileSystem.  The loader recursively
 *      mounts every declared dependency first.  Each mount bumps a
 *      per-bundle refcount.
 *   3. Read scene/asset files normally via `jce_fs_open()` — the
 *      multi-mount filesystem walks the priority list of mounted
 *      packs and serves the first hit.
 *   4. Call `jce_bundle_unmount(cat, "scene_forest")` when leaving
 *      the scene.  The loader decrements refcounts and unmounts any
 *      bundle whose refcount hits zero (including transitive deps no
 *      longer needed by anyone).
 *
 * Backward compatibility: opening a catalog is opt-in.  The legacy
 * monolithic `game_assets.pak` keeps working when no catalog is
 * provided.
 *
 * Thread safety: catalog open/close/mount/unmount are NOT thread-safe;
 * call them from the main thread.  The loader is intentionally simple
 * — async loading is the caller's responsibility (start a worker, do
 * mount() on the worker, then signal main).
 *
 * Layer: Resource (Layer 3).  Pure C, FFI-safe.
 */
#ifndef JCE_BUNDLE_LOADER_H
#define JCE_BUNDLE_LOADER_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

struct JceFileSystem;

typedef struct JceBundleCatalog JceBundleCatalog;

/* Open the catalog at `catalog_path`.  Bundle files are resolved
 * relative to the catalog's directory.  Returns NULL on failure (file
 * missing, JSON malformed, contract version incompatible). */
JCE_API JceBundleCatalog *jce_bundle_catalog_open(struct JceFileSystem *fs,
                                                  const char *catalog_path);

JCE_API void jce_bundle_catalog_close(JceBundleCatalog *cat);

/* Catalog inspection. */
JCE_API uint32_t    jce_bundle_catalog_version(const JceBundleCatalog *cat);
JCE_API uint32_t    jce_bundle_catalog_count  (const JceBundleCatalog *cat);
JCE_API const char *jce_bundle_catalog_id_at  (const JceBundleCatalog *cat,
                                               uint32_t idx);
JCE_API const char *jce_bundle_catalog_kind   (const JceBundleCatalog *cat,
                                               const char *bundle_id);
JCE_API const char *jce_bundle_catalog_file   (const JceBundleCatalog *cat,
                                               const char *bundle_id);
JCE_API const char *jce_bundle_catalog_scene_path(const JceBundleCatalog *cat,
                                                  const char *bundle_id);
JCE_API uint32_t    jce_bundle_catalog_dep_count(const JceBundleCatalog *cat,
                                                 const char *bundle_id);
JCE_API const char *jce_bundle_catalog_dep_at  (const JceBundleCatalog *cat,
                                                const char *bundle_id,
                                                uint32_t idx);
JCE_API uint64_t    jce_bundle_catalog_size_bytes(const JceBundleCatalog *cat,
                                                  const char *bundle_id);
JCE_API const char *jce_bundle_catalog_content_hash(const JceBundleCatalog *cat,
                                                    const char *bundle_id);

/* Mount a bundle (and all its declared dependencies) onto the
 * filesystem.  Each call bumps the bundle's refcount.  Returns
 * `true` if the bundle is mounted and ready (regardless of whether
 * this call performed the mount or simply incremented the refcount). */
JCE_API bool jce_bundle_mount  (JceBundleCatalog *cat, const char *bundle_id);

/* Unmount: decrement refcount; when the count hits 0, the underlying
 * pack is unmounted from the filesystem.  Transitive dependency
 * refcounts are decremented in the same call.  Safe to call on a
 * never-mounted bundle (no-op, returns false). */
JCE_API bool jce_bundle_unmount(JceBundleCatalog *cat, const char *bundle_id);

JCE_API bool     jce_bundle_is_mounted (const JceBundleCatalog *cat,
                                        const char *bundle_id);
JCE_API uint32_t jce_bundle_refcount   (const JceBundleCatalog *cat,
                                        const char *bundle_id);

/* Convenience: mount the bundle that owns the requested scene path
 * (matched against catalog `scene_path` field).  Returns NULL when
 * no bundle owns the scene; otherwise returns the bundle id (string
 * lifetime tied to the catalog). */
JCE_API const char *jce_bundle_mount_for_scene(JceBundleCatalog *cat,
                                               const char *scene_path);

/* ── Standalone bundle (no catalog) ──────────────────────────────────
 *
 * Mounts a single `.jbundle` file produced by jce_bundle_pack's
 * single-file mode (or any catalog-less bundle).  The runtime reads
 * `__bundle__/manifest.json` to discover the bundle id and scene
 * vpath, then mounts the underlying JPAK on `fs` under that id so
 * `jce_fs_open(scene_path)` works immediately.
 *
 * This is the "drag-and-drop a .jbundle and play" entry point used by
 * the editor's "Pack Current Scene" workflow and by minimal embedders
 * that have no project layout.  Refcounts apply per handle: a second
 * mount with the same handle pointer is a hard error; use
 * jce_bundle_file_close to release.
 *
 * Returns NULL on any failure (file missing, not a JPAK, missing
 * manifest, fs mount conflict).
 */
typedef struct JceBundleFile JceBundleFile;

JCE_API JceBundleFile *jce_bundle_file_open (struct JceFileSystem *fs,
                                             const char *jbundle_path,
                                             const char *mount_name_or_null);
JCE_API void           jce_bundle_file_close(JceBundleFile *bf);

/* Inspectors. */
JCE_API const char *jce_bundle_file_id        (const JceBundleFile *bf);
JCE_API const char *jce_bundle_file_scene_path(const JceBundleFile *bf);
JCE_API const char *jce_bundle_file_kind      (const JceBundleFile *bf);

JCE_EXTERN_C_END

#endif /* JCE_BUNDLE_LOADER_H */
