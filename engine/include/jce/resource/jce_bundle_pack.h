/* jce_bundle_pack.h
 *
 * Scene-bundle packer — in-process API.
 *
 * The same logic is exposed two ways:
 *   1. The host CLI tool `jce_bundle_pack` (tools/jce_bundle_pack.c) — for
 *      headless / CI use.
 *   2. The editor's "Build Bundles" dialog calls these functions directly
 *      so packaging stays inside a single executable (no subprocess /
 *      external toolchain needed).
 *
 * Layer: Resource (Layer 3).  Pure C; uses cJSON / xxhash / zstd.
 */
#ifndef JCE_BUNDLE_PACK_H
#define JCE_BUNDLE_PACK_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Diagnostic levels (mirror JceConsoleLevel ordering). */
typedef enum JceBundlePackLogLevel {
    JCE_BUNDLE_PACK_LOG_INFO    = 0,
    JCE_BUNDLE_PACK_LOG_WARNING = 1,
    JCE_BUNDLE_PACK_LOG_ERROR   = 2,
    JCE_BUNDLE_PACK_LOG_SUCCESS = 3
} JceBundlePackLogLevel;

/* Log sink.  Called from the same thread that invokes the run/diff
 * functions — so a worker-thread caller is responsible for marshaling
 * the message to its UI thread.  `msg` is NUL-terminated and does NOT
 * end with a newline. */
typedef void (*JceBundlePackLogFn)(JceBundlePackLogLevel level,
                                   const char *msg, void *user);

/* Inputs for jce_bundle_pack_run.  String fields are borrowed; the
 * caller must keep them alive for the duration of the call.  NULL or
 * empty strings select defaults where noted. */
typedef struct JceBundlePackOptions {
    /* Convenience: when non-empty AND the explicit fields below are
     * empty, every other path is auto-derived from the project root:
     *   scenes_dir    = <project_root>          (recursive *.scene/*.scene.json)
     *   resource_root = <project_root>          (asset paths relative)
     *   out_dir       = <project_root>/.bundles
     *   prev_catalog  = <project_root>/.bundles/.prev/bundle_catalog.json
     * This is the recommended one-button mode for the editor and CI. */
    const char *project_root;

    const char *scenes_dir;       /* required if project_root unset    */
    const char *resource_root;    /* required if project_root unset    */
    const char *out_dir;          /* required if project_root unset    */
    const char *prev_catalog;     /* optional: incremental cache       */
    const char *shared_id;        /* optional: default "_shared"       */
    int         shared_threshold; /* default 2; clamped to >=2         */
    int         zstd_level;       /* default 3; clamped to [1,22]      */
    uint32_t    catalog_version;  /* 0 = auto (prev+1 or 1)            */
    bool        quiet;            /* suppress INFO; ERR/WARN still go  */

    /* ── Standalone / selected-scenes mode (v1.1) ──────────────────────
     *
     * The fields above pack every scene reachable from a directory tree
     * (recursive scan of `*.scene` / `*.scene.json`).  The fields below let callers
     * package an explicit list of scene files instead — including a
     * single "floating" `.scene` or `.scene.json` that does not live inside any
     * project layout.  This mirrors Unity's BuildPipeline.BuildAssetBundles
     * `scenes[]` argument and Addressables' single-asset group flow.
     *
     * Backwards compatible: when `scene_files` is NULL (the default,
     * zero-initialised), behaviour is identical to v1.0.
     */

    /* Explicit array of `.scene` / `.scene.json` paths.  When non-NULL and
     * `scene_file_count > 0`, the packer skips `walk_scenes()` and
     * uses this list verbatim.  Paths must exist and end in
     * `.scene` or `.scene.json`; missing scene entries are logged and skipped.
     * Asset references inside accepted scenes are strict: any missing asset
     * fails the producing bundle/run instead of emitting a partial bundle. When
     * combined with `scenes_dir`, the dir is used only for id/vpath
     * derivation (paths inside it use the relative form; those outside
     * fall back to basename + 8-hex collision tag). */
    const char *const *scene_files;
    size_t             scene_file_count;

    /* When set, and `resource_root` is empty, derive the resource root
     * from the first scene file's directory by walking up until a
     * folder named `resources` or `assets` is found, or until the
     * filesystem root.  If nothing is found, the scene file's own
     * directory is used.  This makes single-scene packaging work
     * without any project-layout assumptions. */
    bool        auto_resource_root;

    /* Single-file mode: produce exactly one `.jbundle` (plus its
     * `.jbundle.json` sidecar), skipping `bundle_catalog.json`, the
     * shared-bundle split, the `.gitignore` seed and the `.prev/`
     * incremental cache.  The output file is named
     *   <out_dir>/<bundle_id>.<short-hash>.jbundle
     * Only meaningful when `scene_file_count == 1`. */
    bool        single_file_mode;

    /* Override the bundle id used in single-file mode.  When NULL or
     * empty, the id is derived from the scene file's basename minus
     * its scene suffix. */
    const char *single_bundle_id;

    /* ── Project-scope path resolver (v1.2) ────────────────────────────
     *
     * Final fallback when an asset's vpath cannot be located either via
     * the active VFS or via `<resource_root>/<vpath>`.  The editor wires
     * this to `jce_asset_path_index_lookup` so any indexed file under
     * the project (including arbitrary non-resource_root subtrees like
     * external test asset folders) still gets pulled into the bundle.
     *
     * Contract: write the resolved **absolute host path** into out_buf
     * (NUL-terminated) and return true.  Return false when the vpath
     * cannot be resolved; the packer will then emit "missing asset".
     */
    bool      (*resolve_fn)(const char *vpath, char *out_buf,
                            size_t out_size, void *user);
    void       *resolve_user;

    /* ── Asset cooking (P0-build-bundles-cook) ─────────────────────────
     *
     * When `cook_assets` is true, the packer cooks each gathered asset
     * before storing it in the bundle, instead of shipping the raw source:
     *   - textures (.png/.jpg/.jpeg/.tga/.bmp) → cooked .jceasset (GPU
     *     block format: BC on desktop, ASTC on mobile — see target_platform)
     *   - models   (.obj/.fbx/.dae/.gltf/.glb) → binary glTF (.glb) with a
     *     meshoptimizer dedup/vertex-cache pass
     *   - audio    (.wav/.ogg/.flac/.opus/...) → cooked .jceasset PCM
     * A sibling `<asset>.import.json` (written by the editor's Import
     * Presets) is consulted for per-asset overrides (target_format,
     * max_size, gen_mips, scale, gen_normals/tangents, flip_uv).
     * The cooked bytes keep the SAME vpath; the runtime loaders content-
     * sniff (texture/audio: .jceasset magic, mesh: cgltf GLB magic), so no
     * reference rewriting is needed. Cook failures fall back to raw bytes
     * (the asset still ships, just uncooked) and are logged as warnings.
     *
     * Default (zero-initialised) is false: behaviour is identical to before. */
    bool        cook_assets;

    /* Target platform for cook auto-format selection (texture BC vs ASTC).
     * Values mirror JceCookPlatform in jce_asset_cooker.h:
     *   0=Windows 1=Linux 2=macOS 3=Android 4=iOS 5=Web 6=Auto(RGBA8).
     * Default (0) selects Windows/desktop BC. Only meaningful when
     * cook_assets is true. */
    int         target_platform;

    /* ── Bundle protection (spec §9.2) ─────────────────────────────────
     * When `encrypt` is true and `encryption_key` points at 32 bytes,
     * every entry of every produced .jbundle (including the embedded
     * __bundle__/manifest.json) uses a keyed index, ChaCha20 payloads, and
     * a full-archive HMAC, with the bundle id as the key-derivation label.
     * Runtime mounts decrypt transparently once the process key is installed.
     * The key ships inside the client, so this raises extraction cost rather
     * than creating secrecy.  Toggling protection or changing the key
     * invalidates the incremental (.prev) cache. */
    bool           encrypt;
    const uint8_t *encryption_key;   /* 32 bytes, borrowed */

    /* ── Always-included roots (Unity's Always Included Shaders) ───────
     *
     * Asset paths, resource-root relative, that must travel even though no
     * scene, material or descriptor points at them.  Unity's setting exists
     * because a shader named only from a SCRIPT is reachable from no
     * material and gets stripped; the same is true here of a graph-compiled
     * .bin blob, or any asset a script names by string.
     *
     * MERGED with `bundle_roots.json`, not a replacement for it: a project
     * may reasonably have both, and a list that silently won over the other
     * would be a shipped build missing exactly what somebody thought they
     * had declared.  Each path is a dependency ROOT, so the packer's
     * fixed-point closure follows whatever it references.
     *
     * A path that resolves to nothing is an ERROR, not a warning: the whole
     * point of the list is that nothing else would have caught it.
     * APPENDED (ABI); NULL / 0 is the historical behaviour exactly. */
    const char *const *always_included;
    int                always_included_count;
} JceBundlePackOptions;

/* Build all bundles described by `opts`.  Returns 0 on success or a
 * non-zero error code.  Safe to call from any thread.  Each invocation
 * uses an independent setjmp scope, so a fatal `oom` / IO error inside
 * the packer cleanly aborts the call instead of exit()ing the process. */
JCE_API int jce_bundle_pack_run(const JceBundlePackOptions *opts,
                                JceBundlePackLogFn         log_fn,
                                void                      *log_user);

/* Emit a .jdiff document between two existing catalog JSON files. */
JCE_API int jce_bundle_pack_diff(const char *old_catalog_path,
                                 const char *new_catalog_path,
                                 const char *out_diff_path,
                                 JceBundlePackLogFn log_fn,
                                 void              *log_user);

JCE_EXTERN_C_END

#endif /* JCE_BUNDLE_PACK_H */
