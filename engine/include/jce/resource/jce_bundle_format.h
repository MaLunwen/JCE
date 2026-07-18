/* jce_bundle_format.h
 *
 * Scene-Asset-Bundle format definitions (JSON contract metadata).
 *
 * A "bundle" is a JPAK v1 archive (the portable jce_archive format) carrying:
 *   - the assets referenced by one scene (kind = "scene"), or
 *   - assets shared across multiple scenes (kind = "shared"), or
 *   - a delta payload between two catalog versions (kind = "patch").
 *
 * Each bundle stores a bundled manifest at the well-known virtual path
 * `__bundle__/manifest.json` so the archive is self-describing.  A
 * sidecar `<bundle-file>.jbundle.json` mirrors the manifest and adds the
 * completed archive's size/content hash, so tooling/CDNs can verify and list
 * bundles without opening the .jbundle archive.
 *
 * A top-level `bundle_catalog.json` lists every bundle in a build:
 * filename, dependency edges, kind, version.  At runtime, the loader
 * opens the catalog, resolves the dependency closure of a requested
 * scene, and mounts every needed bundle (refcounted) onto the
 * JceFileSystem before scene load.
 *
 * The on-disk container is plain JPAK v1 (jce_archive_format.h — a 64-byte
 * header + explicit little-endian byte I/O read/written at fixed offsets, so
 * it is portable across arch/OS/endianness; the retired struct-packed v2
 * container is NOT used).  The `JPAK_CAP_OPT_BUNDLE` capability bit below is
 * LEGACY/UNUSED — the v1 writer never sets it; a bundle is recognised at
 * runtime by the presence of its `__bundle__/manifest.json` virtual path.
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
#define JCE_BUNDLE_GRAPH_NAME     "bundle_graph.json"
#define JCE_BUNDLE_DIFF_EXT       ".jdiff"

/* ================================================================== */
/* JSON contract identifiers                                           */
/* ================================================================== */

/* Per-bundle manifest. */
#define JCE_BUNDLE_MANIFEST_CONTRACT_NAME  "jce.bundle"
#define JCE_BUNDLE_MANIFEST_CONTRACT_MAJOR 1u
#define JCE_BUNDLE_MANIFEST_CONTRACT_MINOR 1u

/* Top-level catalog. */
#define JCE_BUNDLE_CATALOG_CONTRACT_NAME   "jce.bundle.catalog"
#define JCE_BUNDLE_CATALOG_CONTRACT_MAJOR  1u
#define JCE_BUNDLE_CATALOG_CONTRACT_MINOR  1u

/* Diff/patch document. */
#define JCE_BUNDLE_DIFF_CONTRACT_NAME      "jce.bundle.diff"
#define JCE_BUNDLE_DIFF_CONTRACT_MAJOR     1u
#define JCE_BUNDLE_DIFF_CONTRACT_MINOR     0u

/* Explicit asset dependency document (`<asset>.deps.json` and the
 * project-level `bundle_roots.json`). */
#define JCE_BUNDLE_DEPS_CONTRACT_NAME       "jce.bundle.dependencies"
#define JCE_BUNDLE_DEPS_CONTRACT_MAJOR      1u
#define JCE_BUNDLE_DEPS_CONTRACT_MINOR      0u

/* Container-independent cooked graph snapshot consumed by Bundle/Dist. */
#define JCE_BUNDLE_GRAPH_CONTRACT_NAME       "jce.bundle.graph"
#define JCE_BUNDLE_GRAPH_CONTRACT_MAJOR      1u
#define JCE_BUNDLE_GRAPH_CONTRACT_MINOR      0u

/* Runtime representation contract written by Bundle 1.1 producers. */
#define JCE_BUNDLE_CONTENT_ABI               "jce-content-1"
#define JCE_BUNDLE_COOK_VERSION              1u

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
#define JCE_BUNDLE_KEY_CONTENT_HASH  "content_hash" /* hex string, XXH3-64 of pak    */
#define JCE_BUNDLE_KEY_BUILD_HASH    "build_hash"   /* XXH3-64 of source graph       */
#define JCE_BUNDLE_KIND_KEY          "kind"         /* JCE_BUNDLE_KIND_*             */
#define JCE_BUNDLE_KEY_SCENE_PATH    "scene_path"   /* string, scene JSON vpath      */
#define JCE_BUNDLE_KEY_DEPENDS_ON    "depends_on"   /* array<string> of bundle ids   */
#define JCE_BUNDLE_KEY_ASSETS        "assets"       /* array of typed asset records  */
#define JCE_BUNDLE_KEY_ASSET_PATH    "path"
#define JCE_BUNDLE_KEY_ASSET_SIZE    "size"
#define JCE_BUNDLE_KEY_ASSET_HASH    "hash"
#define JCE_BUNDLE_KEY_ASSET_ID      "asset_id"       /* hash of canonical address */
#define JCE_BUNDLE_KEY_CONTENT_ID    "content_id"     /* hash of cooked bytes       */
#define JCE_BUNDLE_KEY_ASSET_TYPE    "type"           /* semantic asset type        */
#define JCE_BUNDLE_KEY_REPRESENTATION "representation" /* concrete payload contract */
#define JCE_BUNDLE_KEY_TARGET_PROFILE "target_profile"
#define JCE_BUNDLE_KEY_CONTENT_ABI   "content_abi"
#define JCE_BUNDLE_KEY_COOK_VERSION  "cook_version"
#define JCE_BUNDLE_KEY_MIN_ENGINE_VERSION "minimum_engine_version"
#define JCE_BUNDLE_KEY_GRAPH_ID      "graph_id"
#define JCE_BUNDLE_KEY_ASSET_ADDRESS "address"
#define JCE_BUNDLE_KEY_DEPENDENCIES  "dependencies" /* array<{address,asset_id,origin}> */
#define JCE_BUNDLE_KEY_ORIGIN        "origin"       /* graph-edge discovery source  */
#define JCE_BUNDLE_KEY_ARCHIVE_SIZE  "archive_size" /* final .jbundle bytes */
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
#define JCE_BUNDLE_CATALOG_KEY_BUILD_HASH "build_hash"
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
