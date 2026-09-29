/*
 * jce_bundle_pack_cook.h -- the in-process asset cook, split out of
 * jce_bundle_pack.c when that file crossed 3000 lines.
 *
 * The seam is not arbitrary.  Packing is two jobs: work out WHICH assets
 * travel (the dependency closure, the bundle association, the catalog) and
 * turn each one into its shipped BYTES (texture block-encode, model import,
 * audio transcode).  The second is where every slow second of a build goes,
 * it is the only part that fans out onto the job pool, and it reaches the
 * first through exactly two names -- which is what makes it a seam rather
 * than a cut.
 */
#ifndef JCE_BUNDLE_PACK_COOK_H
#define JCE_BUNDLE_PACK_COOK_H

#include <jce/resource/jce_bundle_pack.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward: the external-asset map lives in jce_bundle_pack.c and is READ-ONLY
 * while cooking, which is what lets the workers share it without a lock. */
typedef struct ExternalMap ExternalMap;

/* The caller's vpath -> host-path resolver.  It lived as a file-local typedef
 * in jce_bundle_pack.c; both translation units need it now, so it lives with
 * the seam rather than being spelled twice. */
typedef bool (*PackResolveFn)(const char *vpath, char *out, size_t outsz, void *u);

/* Per-job cook outcome, written on the worker and replayed by the driver.
 * A worker must never touch the log sink (the editor's is not thread-safe) or
 * the thread-local pack context, so the outcome travels as DATA and is
 * narrated afterwards, in deterministic order. */
typedef struct {
    bool failed;       /* a cook was attempted but failed -> shipped raw     */
    char err[96];      /* short failure detail for the driver-side warning   */
} CookStatus;

typedef struct {
    size_t      slot;       /* entries[] index this asset writes              */
    const char *vpath;      /* borrowed                                       */
    uint8_t    *buf;        /* in: rewritten raw; out: cooked (worker writes) */
    size_t      in_size;    /* original size (for the reconstructed log line) */
    size_t      out_size;   /* cooked size (worker writes)                    */
    CookStatus  status;     /* worker-written outcome, driver-replayed        */
} CookJob;

typedef struct {
    CookJob          *jobs;
    const char       *resource_root;
    PackResolveFn     resolve_fn;
    void             *resolve_user;
    const ExternalMap *emap;
    int               target_platform;
} CookCtx;

/* Cook one asset.  Returns the buffer to ship (possibly `raw` itself when the
 * asset is kept as authored) and never longjmps -- see CookStatus. */
uint8_t *jce_bundle_cook_asset(const char *vpath, uint8_t *raw, size_t raw_size,
                               const char *resource_root,
                               PackResolveFn resolve_fn, void *resolve_user,
                               const ExternalMap *emap,
                               int target_platform,
                               size_t *out_size, CookStatus *out_status);

/* jce_thread_pool_parallel_for body over CookCtx::jobs. */
void jce_bundle_cook_jobs_range(uint32_t begin, uint32_t end, void *user);

/* ── What the cook borrows back from the driver ────────────────────
 *
 * THREE names, and the list is short on purpose: it is the measure of whether
 * this is a seam or a cut.  Anything longer would mean the cook is not
 * actually separable from the graph walk beside it.
 *
 * pack_log_info is called ONLY on the driver thread -- a worker must never
 * touch it (see CookStatus). */
void pack_log_info(const char *fmt, ...);

/* Read one asset's bytes through the caller's resolver and the external map.
 * Runs on the DRIVER before dispatch: it longjmps on OOM, and a worker has no
 * jmp_buf of its own. */
uint8_t *read_asset(const char *vpath, const char *resource_root,
                    PackResolveFn resolve_fn, void *resolve_user,
                    const ExternalMap *emap, size_t *out_size);

/* Case-insensitive suffix test, used by both halves to classify a path. */
int ends_with_ci(const char *s, const char *suffix);

#endif /* JCE_BUNDLE_PACK_COOK_H */
