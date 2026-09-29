/*
 * jce_asset_ref_rewrite.h — repoint references when an asset is renamed.
 *
 * WHY THIS EXISTS.  Every asset reference in this engine is a raw path string:
 * JceMeshRenderer.material_path, JceAudioSourceComponent.clip_path, a
 * material's texture paths, a scene's prefab paths -- 256-byte char arrays,
 * with no GUID and no .meta sidecar anywhere in the tree.  Renaming a file in
 * the Asset Browser called jce_fs_host_rename and logged success.  Nothing
 * else happened.  Every scene, prefab and material that pointed at the old
 * name silently went to a missing asset, and the engine's mitigation -- the
 * fuzzy basename index in editor/src/scene/jce_asset_path_index.* -- cannot
 * help, because a RENAME is exactly the case where the basename changed.
 *
 * jce_assetdb already knows who points at what (jce_assetdb_find_references),
 * and that function had ZERO CALLERS: its own header lists three consumers and
 * none of them called it, so the reverse map it builds lazily was built by
 * nobody.  This is the missing half.
 *
 * THE REWRITE IS TEXTUAL, AND DELIBERATELY CONSERVATIVE.  jce_assetdb decides
 * a file references an asset if the file's bytes contain the asset's
 * project-relative path OR its bare basename.  The basename half is the
 * dangerous one to rewrite: "wood.png" can occur inside "darkwood.png", inside
 * a comment, or inside an unrelated word.  So a basename is only rewritten
 * when it is DELIMITED -- preceded by '/', '\\' or '"' and followed by '"' --
 * which is what a path inside JSON looks like and what prose does not.
 *
 * That conservatism means some references CANNOT be rewritten, and the caller
 * is told which: a file the index flagged and this function did not change is
 * reported, not swallowed.  A rename that silently half-repairs is the defect
 * this fixes, relocated.
 *
 * Pure: no filesystem, no globals, no ImGui.  jce_assetdb_rename_asset does
 * the I/O around it.  Kept dependency-free (jce_core only) so it unit-tests
 * standalone, the same reason jce_assetdb.cpp is.
 */
#ifndef JCE_ASSET_REF_REWRITE_H
#define JCE_ASSET_REF_REWRITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rewrite references to one renamed asset inside a text buffer.
 *
 *   text/len       the file's bytes (not required to be NUL-terminated)
 *   old_rel/new_rel   project-relative paths, forward slashes, may be NULL
 *   old_base/new_base bare filenames, may be NULL
 *   out            receives a NUL-terminated malloc'd buffer the caller frees
 *                  with jce_free; only written when the return value is > 0
 *   out_len        receives the new length (excluding the NUL); may be NULL
 *
 * Returns the number of substitutions made, or 0 when nothing matched (and
 * then *out is left untouched, so the caller writes nothing back).
 *
 * Rel paths are matched exactly, and with '\\' accepted in the buffer where
 * the rel path has '/', because a scene saved on Windows can carry either.
 * Basenames are matched only when delimited as described in the file header.
 */
int jce_asset_ref_rewrite(const char *text, size_t len,
                          const char *old_rel,  const char *new_rel,
                          const char *old_base, const char *new_base,
                          char **out, size_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JCE_ASSET_REF_REWRITE_H */
