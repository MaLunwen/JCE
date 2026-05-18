/* jce_bundle_deps.h
 *
 * Scene → asset dependency scanner used by both the host packer
 * (`jce_bundle_pack`) and the editor's "Build Bundles" UI.
 *
 * Strategy: parse a scene JSON document and walk the entire tree,
 * harvesting any string value whose key is one of the well-known
 * asset-path keys ("meshPath", "materialPath", "texturePath",
 * "audioPath"/"clipPath", "fontPath", "hdrPath", "spritePath",
 * "scriptPath", "skeletonPath", "atlasPath", "sheetPath",
 * "terrainPath", "layerAlbedoPath0..3", "meshPath0..N", "prefabPath",
 * "animationPath").  Empty strings are ignored.
 *
 * Per-asset/per-component override tags ("bundle": "force-shared" |
 * "force-local" | "<custom>") are also collected so the packer can honor
 * them when partitioning into shared bundles.
 *
 * Layer: Resource (Layer 3).  Pure C; uses cJSON.
 */
#ifndef JCE_BUNDLE_DEPS_H
#define JCE_BUNDLE_DEPS_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One discovered asset reference.  `path` is heap-allocated; caller frees
 * the whole list via jce_bundle_deps_free(). */
typedef struct JceBundleDep {
    char *path;   /* normalised forward-slash relative path             */
    char *bundle; /* override tag (NULL if none): "force-shared" |
                   *  "force-local" | "<custom-bundle-id>"             */
} JceBundleDep;

typedef struct JceBundleDepList {
    JceBundleDep *items;
    uint32_t      count;
    uint32_t      capacity;
} JceBundleDepList;

/* Scan a scene JSON string and return the list of asset references.
 * `json_len` may be 0 to use strlen.  Returns true on parse success
 * (deps list may still be empty for pure-logic scenes). */
JCE_API bool jce_bundle_deps_scan(const char *json, size_t json_len,
                                  JceBundleDepList *out_list);

/* Convenience: read a scene JSON from a host file path and scan. */
JCE_API bool jce_bundle_deps_scan_file(const char *scene_path,
                                       JceBundleDepList *out_list);

/* Release every string + the items array.  Safe to call on a
 * zero-initialised list. */
JCE_API void jce_bundle_deps_free(JceBundleDepList *list);

/* Returns 1 iff `key` is a recognised asset-path JSON key.  Exposed so
 * the editor can highlight asset references in raw JSON views. */
JCE_API int jce_bundle_deps_is_asset_key(const char *key);

JCE_EXTERN_C_END

#endif /* JCE_BUNDLE_DEPS_H */
