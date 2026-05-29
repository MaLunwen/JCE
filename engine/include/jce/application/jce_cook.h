/*
 * jce_cook.h  Layer 6 — Asset cook orchestrator.
 *
 * Transforms editor-author sources (under JceProject::source_assets)
 * into runtime-ready artefacts (under JceProject::cooked_assets).  The
 * cook output is the tree the build pipeline feeds to PAK embedding —
 * keep it deterministic and self-contained.
 *
 * The v1 cooker is intentionally minimal and rule-based:
 *
 *   .scene / .scene.json   → copy verbatim (binary cook later)
 *   .json / .toml / .csv   → copy verbatim
 *   .png / .jpg / .ktx2    → copy verbatim
 *   .ogg / .opus / .wav    → copy verbatim
 *   .ttf / .otf            → copy verbatim
 *   everything else        → copy verbatim (no-op)
 *
 * Future work (S2+):
 *   - .scene → packed binary scene (via flecs serializer)
 *   - .glsl/.hlsl/.sc → bgfx shaderc per backend
 *   - .png → KTX2/BC7 via texconv pipeline
 *
 * Up-to-date detection: a cooked file is considered fresh when it
 * exists and its mtime ≥ the source mtime.  Mismatched / missing
 * outputs are rebuilt; everything else is skipped.
 *
 * The cooker mutates the filesystem only beneath `cooked_assets`.  It
 * never touches `source_assets` (read-only by contract).
 */

#ifndef JCE_COOK_H
#define JCE_COOK_H

#include <jce/os/core/jce_defs.h>
#include <jce/application/jce_project.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceCookStats {
	int total;       /* files visited under source_assets       */
	int cooked;      /* files actually (re)written this run     */
	int skipped;     /* up-to-date and skipped                  */
	int failed;      /* I/O or transform failures               */
} JceCookStats;

/* Per-file progress callback.  Return false to abort the cook early.
 *  `rel_path` is the path relative to source_assets (forward slashes).
 *  `did_write` is true when the file was actually (re)written. */
typedef bool (*JceCookProgressFn)(const char *rel_path,
                                  bool        did_write,
                                  void       *user);

/* Run the full cook for `p`.  Creates `cooked_assets` if missing.
 * `progress` may be NULL.  On any fatal setup error returns false
 * (per-file failures count in stats but do not stop the run unless
 * `progress` returns false).  Thread-safe to call from a worker. */
JCE_API bool JCE_CALL jce_cook_run_all(const JceProject  *p,
                                       JceCookProgressFn  progress,
                                       void              *user,
                                       JceCookStats      *out_stats);

/* Cook a single file (path relative to source_assets).  Useful for
 * editor save-hooks ("just saved foo.scene → re-cook foo.scene"). */
JCE_API bool JCE_CALL jce_cook_run_one(const JceProject *p,
                                       const char       *rel_path);

/* Returns true when the cooked tree is fresh w.r.t. the sources — i.e.
 * a follow-up jce_cook_run_all() would write zero files.  Cheap to call
 * (single mtime sweep). */
JCE_API bool JCE_CALL jce_cook_is_up_to_date(const JceProject *p);

/* Wipe `cooked_assets` (recursive).  Useful when bumping cook version
 * or when the user wants a clean rebuild. */
JCE_API bool JCE_CALL jce_cook_clean(const JceProject *p);

JCE_EXTERN_C_END

#endif /* JCE_COOK_H */
