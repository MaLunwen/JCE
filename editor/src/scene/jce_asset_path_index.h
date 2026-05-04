/*
 * jce_asset_path_index.h  Project-wide asset path index.
 *
 * Builds a basename → absolute path map for all asset files under a
 * project root so scene loaders, drag-drop handlers, and material
 * resolvers can look up references in O(1) instead of recursively
 * walking the filesystem on every miss.  Mirrors Unity's import-time
 * GUID/path table at a much simpler granularity (basename only).
 *
 * Lookup order applied internally:
 *   1. exact lowercase basename match
 *   2. alphanum-only lowercase match (ignores - vs _ vs PascalCase)
 *   3. prefix-stripped match (SM_/SKM_/T_/M_/... removed from disk stem)
 *
 * Multiple files sharing a basename are stored; the *shortest* absolute
 * path wins on lookup (heuristic: shorter = closer to project root).
 */

#ifndef JCE_ASSET_PATH_INDEX_H
#define JCE_ASSET_PATH_INDEX_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drop all entries.  Call before reindexing or on project close. */
void jce_asset_path_index_clear(void);

/* Recursively scan `root`, adding every regular file to the index.
 * Safe to call multiple times for several roots; entries accumulate.
 * Skips dot-prefixed directories, build/, .git/, node_modules/.
 * Returns number of files indexed in this call.
 */
int  jce_asset_path_index_rebuild(const char *root);

/* Look up `requested_path` (any form: basename, relative, broken
 * absolute) and write the best matching absolute path to out_buf.
 * Returns true on hit. */
bool jce_asset_path_index_lookup(const char *requested_path,
                                 char *out_buf, int out_size);

/* Diagnostic: number of indexed files. */
int  jce_asset_path_index_size(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_PATH_INDEX_H */
