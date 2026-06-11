/* jce_bundle_format.h
 *
 * Scene-Asset-Bundle format definitions (JSON contract metadata).
 *
 * A "bundle" is a JPAK v2 archive carrying:
 *   - the assets referenced by one scene (kind = "scene"), or
 *   - assets shared across multiple scenes (kind = "shared"), or
 *   - a delta payload between two catalog versions (kind = "patch").
 *
 * Each bundle stores a bundled manifest at the well-known virtual path
 * `__bundle__/manifest.json` so the archive is self-describing.  A
 * sidecar `<bundle-file>.jbundle.json` (identical contents) is also
 * emitted so external tooling/CDNs can list bundles without opening
 * the .jbundle archive.
 *
 * A top-level `bundle_catalog.json` lists every bundle in a build:
 * filename, dependency edges, kind, version.  At runtime, the loader
 * opens the catalog, resolves the dependency closure of a requested
 * scene, and mounts every needed bundle (refcounted) onto the
 * JceFileSystem before scene load.
 *
 * The on-disk container is plain JPAK v2.  The optional capability bit
 * `JPAK_CAP_OPT_BUNDLE` is set in JpakHeader.flags so JPAK tooling can
 * recognise bundles without parsing the manifest.
 *
 * Layer: Resource (Layer 3).  Pure C, FFI-safe.
 */
#ifndef JCE_BUNDLE_FORMAT_H
#define JCE_BUNDLE_FORMAT_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Capability bit — set in JpakHeader.flags for bundle archives        */
/* ================================================================== */

/* Optional capability: archive is a JCE bundle (manifest at
 * `__bundle__/manifest.json`).  Loaders that don't understand this bit
 * still load the archive normally — bundle features just stay dormant. */
#define JPAK_CAP_OPT_BUNDLE 0x00010000u

/* ================================================================== */
/* Well-known virtual paths inside a bundle                            */
/* ================================================================== */

#define JCE_BUNDLE_MANIFEST_VPATH "__bundle__/manifest.json"

/* Sidecar file extension (next to the .jbundle on disk). */
#define JCE_BUNDLE_FILE_EXT       ".jbundle"
#define JCE_BUNDLE_MANIFEST_EXT   ".jbundle.json"
#define JCE_BUNDLE_CATALOG_NAME   "bundle_catalog.json"
#define JCE_BUNDLE_DIFF_EXT       ".jdiff"

/* ================================================================== */
/* JSON contract identifiers                                           */
/* ================================================================== */

/* Per-bundle manifest. */
#define JCE_BUNDLE_MANIFEST_CONTRACT_NAME  "jce.bundle"
#define JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR 1u
#define JCE_BUNDLE_MANIFEST_CONTRACT_MINOR 0u

/* Top-level catalog. */
#define JCE_BUNDLE_CATALOG_CONTRACT_NAME   "jce.bundle.catalog"
#define JCE_BUNDLE_CATALOG_CONTRACT_MAJOR  1u
#define JCE_BUNDLE_CATALOG_CONTRACT_MINOR  0u

/* Diff/patch document. */
#define JCE_BUNDLE_DIFF_CONTRACT_NAME      "jce.bundle.diff"
#define JCE_BUNDLE_DIFF_CONTRACT_MAJOR     1u
#define JCE_BUNDLE_DIFF_CONTRACT_MINOR     0u

/* ================================================================== */
/* Bundle kinds                                                        */
/* ================================================================== */

#define JCE_BUNDLE_KIND_SCENE   "scene"   /* assets owned by one scene  */
#define JCE_BUNDLE_KIND_SHARED  "shared"  /* assets used by 2+ scenes   */
#define JCE_BUNDLE_KIND_PATCH   "patch"   /* delta between two catalogs */

/* ================================================================== */
/* JSON keys (manifest)                                                */
/* ================================================================== */

/* Top-level wrapper (matches scene_serial style). */
#define JCE_BUNDLE_KEY_CONTRACT      "contract"
#define JCE_BUNDLE_KEY_CONTRACT_NAME "name"
#define JCE_BUNDLE_KEY_CONTRACT_MAJOR "major"
#define JCE_BUNDLE_KEY_CONTRACT_MINOR "minor"

/* Manifest fields. */
#define JCE_BUNDLE_KEY_ID            "id"           /* string, unique within catalog */
#define JCE_BUNDLE_KEY_VERSION       "version"      /* uint, monotonic per id        */
#define JCE_BUNDLE_KEY_CONTENT_HASH  "content_hash" /* hex string, XXH3-128 of pak   */
#define JCE_BUNDLE_KIND_KEY          "kind"         /* JCE_BUNDLE_KIND_*             */
#define JCE_BUNDLE_KEY_SCENE_PATH    "scene_path"   /* string, scene JSON vpath      */
#define JCE_BUNDLE_KEY_DEPENDS_ON    "depends_on"   /* array<string> of bundle ids   */
#define JCE_BUNDLE_KEY_ASSETS        "assets"       /* array<{path,size,hash}>       */
#define JCE_BUNDLE_KEY_ASSET_PATH    "path"
#define JCE_BUNDLE_KEY_ASSET_SIZE    "size"
#define JCE_BUNDLE_KEY_ASSET_HASH    "hash"
#define JCE_BUNDLE_KEY_ENCRYPTED     "encrypted"    /* bool, payload ChaCha20-enc.   */

/* ================================================================== */
/* JSON keys (catalog)                                                 */
/* ================================================================== */

#define JCE_BUNDLE_CATALOG_KEY_VERSION  "version"   /* uint, monotonic catalog ver. */
#define JCE_BUNDLE_CATALOG_KEY_BUNDLES  "bundles"   /* object<id, entry>            */
#define JCE_BUNDLE_CATALOG_KEY_FILE     "file"      /* string, on-disk filename     */
#define JCE_BUNDLE_CATALOG_KEY_DEPS     "deps"      /* array<string>                */
#define JCE_BUNDLE_CATALOG_KEY_SIZE     "size"      /* uint, on-disk byte size      */
#define JCE_BUNDLE_CATALOG_KEY_KIND     "kind"      /* JCE_BUNDLE_KIND_*            */
#define JCE_BUNDLE_CATALOG_KEY_HASH     "content_hash"
#define JCE_BUNDLE_CATALOG_KEY_SCENE    "scene_path"

/* ================================================================== */
/* JSON keys (diff/patch)                                              */
/* ================================================================== */

#define JCE_BUNDLE_DIFF_KEY_FROM_VERSION "from_version"
#define JCE_BUNDLE_DIFF_KEY_TO_VERSION   "to_version"
#define JCE_BUNDLE_DIFF_KEY_ADDED        "added"    /* array<bundle entry>          */
#define JCE_BUNDLE_DIFF_KEY_UPDATED      "updated"  /* array<bundle entry>          */
#define JCE_BUNDLE_DIFF_KEY_REMOVED      "removed"  /* array<string>  bundle ids    */

/* ================================================================== */
/* Special bundle ids                                                  */
/* ================================================================== */

/* The default shared bundle id used when no override tag is supplied. */
#define JCE_BUNDLE_SHARED_DEFAULT_ID "_shared"

/* Reserved override values that may appear in scene/asset metadata
 * under the "bundle" key:
 *
 *   "force-shared"  → put this asset in the default shared bundle even
 *                     if it is referenced by only one scene.
 *   "force-local"   → keep this asset inside every referencing scene's
 *                     own bundle (duplicate it) instead of moving it to
 *                     a shared bundle.  Useful for tiny assets where the
 *                     extra mount cost outweighs the duplicated bytes.
 *   "<custom-id>"   → place asset in a custom shared bundle named id.
 */
#define JCE_BUNDLE_TAG_FORCE_SHARED "force-shared"
#define JCE_BUNDLE_TAG_FORCE_LOCAL  "force-local"
#define JCE_BUNDLE_TAG_KEY          "bundle"

/* ================================================================== */
/* Helpers (pure inline, header-only)                                  */
/* ================================================================== */

static inline bool jce_bundle_manifest_major_compatible(uint32_t major)
{
    return major == JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR;
}

static inline bool jce_bundle_catalog_major_compatible(uint32_t major)
{
    return major == JCE_BUNDLE_CATALOG_CONTRACT_MAJOR;
}

JCE_EXTERN_C_END

#endif /* JCE_BUNDLE_FORMAT_H */
