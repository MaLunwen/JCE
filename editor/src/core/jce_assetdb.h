/*
 * jce_assetdb.h  Project-wide asset database with reverse references.
 *
 * Scans a project root, builds a path -> kind map and a reverse map of
 * asset_path -> [files referencing it].  Consumed by the inspector
 * ASSET_REF field, the asset browser, and validation tools.
 *
 * The database is rebuilt on demand (jce_assetdb_rescan); call this
 * after large file changes.  Initial path uses the asset browser's
 * project_root.
 */

#ifndef JCE_ASSETDB_H
#define JCE_ASSETDB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_ASSET_KIND_UNKNOWN = 0,
    JCE_ASSET_KIND_TEXTURE,
    JCE_ASSET_KIND_MODEL,
    JCE_ASSET_KIND_AUDIO,
    JCE_ASSET_KIND_MATERIAL,
    JCE_ASSET_KIND_SCENE,
    JCE_ASSET_KIND_SHADER,
    JCE_ASSET_KIND_SCRIPT,
    JCE_ASSET_KIND_PARTICLE,
    JCE_ASSET_KIND_DATA,
} JceAssetKind;

const char *jce_assetdb_kind_label(JceAssetKind kind);

/* Initialize / rescan against project_root.  NULL or empty resets. */
void jce_assetdb_set_root(const char *project_root);
void jce_assetdb_rescan(void);

/* Returns the current project root (absolute path) or "" if unset. */
const char *jce_assetdb_get_root(void);

/* Flat enumeration. */
int            jce_assetdb_count(void);
const char    *jce_assetdb_path_at(int idx);
const char    *jce_assetdb_rel_at(int idx);
JceAssetKind   jce_assetdb_kind_at(int idx);

/* Lookups (path is project-relative or absolute, both accepted). */
JceAssetKind   jce_assetdb_get_kind(const char *path);

/* Reverse references: which files reference this asset?
 * Writes up to max_out paths into out_paths (caller-owned char[][512]).
 * Returns total count (may exceed max_out). */
int jce_assetdb_find_references(const char *asset_path,
                                char (*out_paths)[512], int max_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JCE_ASSETDB_H */
