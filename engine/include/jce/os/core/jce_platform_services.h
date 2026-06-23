/*
 * jce_platform_services.h -- Platform / online-service abstraction.
 *
 * A vendor-neutral facade for the "online platform" features a shipped game
 * needs from a store/runtime SDK: a stable user id, achievements, stats,
 * leaderboards, and cloud-save slots.  The engine and gameplay code talk only
 * to this interface; the concrete provider (Steam, Epic Online Services,
 * console SDKs, ...) sits behind a function-pointer backend (JcePlatformBackend)
 * and is selected at init.
 *
 * This header ships exactly ONE backend: a dependency-free LOCAL backend that
 * persists everything to JSON files under a caller-supplied data directory.
 * It is the offline / null-platform default and the headless-test target.
 * Future SDK backends (Steam/EOS) implement the same vtable and live in their
 * own optional translation units — NO third-party SDK is referenced here.
 *
 * Determinism: the LOCAL backend never reads wall-clock or random state in a
 * way that affects observable output; leaderboard ordering is a stable sort by
 * (score desc, insertion order).  Point it at a temp directory and it is fully
 * reproducible and unit-testable.
 *
 * Process-global handle is NOT implied: callers own a JcePlatformServices*.
 * Single-thread per handle (drive from the main / gameplay thread).
 *
 * Layer: OS / Core (Layer 1) — foundational; depends only on jce_core.
 */

#ifndef JCE_PLATFORM_SERVICES_H
#define JCE_PLATFORM_SERVICES_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque service handle, valid until jce_platform_shutdown(). */
typedef struct JcePlatformServices JcePlatformServices;

/* ── Backend identification ──────────────────────────────────────────────── */

typedef enum {
    JCE_PLATFORM_BACKEND_LOCAL = 0,   /* file-backed, dependency-free (default) */
    JCE_PLATFORM_BACKEND_STEAM,       /* reserved — future SDK backend          */
    JCE_PLATFORM_BACKEND_EOS,         /* reserved — future SDK backend          */
    JCE_PLATFORM_BACKEND_CUSTOM       /* a caller-supplied vtable               */
} JcePlatformBackendKind;

/* ── Backend interface (vtable) ──────────────────────────────────────────────
 * A provider implements these and registers them via jce_platform_init_backend.
 * `self` is the backend's own opaque state (returned by create()).  All string
 * outputs are written into caller-provided buffers; all functions are required
 * to be NULL-safe on their pointer arguments.  Unsupported operations should
 * return false / 0 rather than crash. */
typedef struct JcePlatformBackend {
    JcePlatformBackendKind kind;
    const char            *name;       /* human-readable, e.g. "local" */

    /* Lifecycle.  create() returns backend state or NULL on failure.
     * `config` is backend-specific (the LOCAL backend takes the data dir). */
    void *(*create)(const char *config);
    void  (*destroy)(void *self);

    /* Stable per-installation user id (NUL-terminated into out/cap). */
    bool  (*user_id)(void *self, char *out, int cap);

    /* Achievements. */
    bool  (*ach_define)(void *self, const char *id, const char *name, int target);
    bool  (*ach_unlock)(void *self, const char *id);          /* idempotent */
    bool  (*ach_is_unlocked)(void *self, const char *id);
    bool  (*ach_set_progress)(void *self, const char *id, int current); /* auto-unlocks at target */
    int   (*ach_progress)(void *self, const char *id);

    /* Stats (named integer counters). */
    bool  (*stat_set)(void *self, const char *id, int64_t value);
    int64_t (*stat_get)(void *self, const char *id, int64_t def);

    /* Leaderboards.  submit() keeps the BEST (highest) score per user;
     * query() fills out_scores/out_users (parallel arrays, may be NULL) with
     * the top `cap` entries sorted by score desc, returning the count. */
    bool  (*lb_submit)(void *self, const char *board, const char *user, int64_t score);
    int   (*lb_query_top)(void *self, const char *board, int cap,
                          int64_t *out_scores, char *out_users, int user_stride);

    /* Cloud-save slots (opaque byte blobs keyed by slot name). */
    bool  (*save_write)(void *self, const char *slot, const void *data, uint32_t size);
    /* Reads slot into `out` (cap bytes); writes the full size to *out_size.
     * Returns false if absent or `out` too small (still sets *out_size). */
    bool  (*save_read)(void *self, const char *slot, void *out, uint32_t cap, uint32_t *out_size);
    bool  (*save_exists)(void *self, const char *slot);
    bool  (*save_delete)(void *self, const char *slot);
    /* Enumerate slots: fills `out_names` (a flat char[count][stride]) and
     * returns the slot count.  Pass out_names=NULL to just count. */
    int   (*save_list)(void *self, char *out_names, int cap, int name_stride);

    /* Flush any in-memory state to durable storage (LOCAL: rewrite JSON). */
    bool  (*flush)(void *self);
} JcePlatformBackend;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Initialise with the built-in LOCAL backend, persisting under `data_dir`
 * (created if missing).  Returns NULL on failure (bad dir / OOM).  Reloads any
 * previously persisted state found in `data_dir`. */
JCE_API JcePlatformServices *jce_platform_init_local(const char *data_dir);

/* Initialise with a caller-supplied backend vtable (Steam/EOS/etc.).  `config`
 * is passed verbatim to backend->create.  The vtable is copied; the caller may
 * free it after the call.  Returns NULL if create() fails. */
JCE_API JcePlatformServices *jce_platform_init_backend(const JcePlatformBackend *backend,
                                                       const char *config);

/* Flush and tear down.  NULL is a no-op. */
JCE_API void JCE_CALL jce_platform_shutdown(JcePlatformServices *svc);

/* Persist current state to durable storage immediately.  Returns false on I/O
 * error.  Shutdown also flushes; call this for mid-session checkpoints. */
JCE_API bool JCE_CALL jce_platform_flush(JcePlatformServices *svc);

/* The active backend kind / name (for diagnostics / UI). */
JCE_API JcePlatformBackendKind JCE_CALL jce_platform_backend_kind(const JcePlatformServices *svc);
JCE_API const char * JCE_CALL          jce_platform_backend_name(const JcePlatformServices *svc);

/* ── Identity ────────────────────────────────────────────────────────────── */

/* Write the stable user id into `out` (NUL-terminated, truncated to cap).
 * Returns false on bad args. */
JCE_API bool JCE_CALL jce_platform_user_id(JcePlatformServices *svc, char *out, int cap);

/* ── Achievements ────────────────────────────────────────────────────────── */

/* Define an achievement.  `target` is the progress goal (use 1 for a simple
 * boolean achievement).  Idempotent: redefining keeps prior unlock/progress.
 * Returns false on bad args. */
JCE_API bool JCE_CALL jce_platform_ach_define(JcePlatformServices *svc,
                                              const char *id, const char *name, int target);

/* Unlock an achievement (idempotent — a second unlock is a no-op success). */
JCE_API bool JCE_CALL jce_platform_ach_unlock(JcePlatformServices *svc, const char *id);

/* True iff the achievement exists and is unlocked. */
JCE_API bool JCE_CALL jce_platform_ach_is_unlocked(JcePlatformServices *svc, const char *id);

/* Set absolute progress (clamped to [0, target]); reaching target unlocks.
 * Returns false if the achievement is undefined or args are bad. */
JCE_API bool JCE_CALL jce_platform_ach_set_progress(JcePlatformServices *svc,
                                                    const char *id, int current);

/* Current progress (0 if undefined). */
JCE_API int  JCE_CALL jce_platform_ach_progress(JcePlatformServices *svc, const char *id);

/* ── Stats ───────────────────────────────────────────────────────────────── */

JCE_API bool    JCE_CALL jce_platform_stat_set(JcePlatformServices *svc,
                                               const char *id, int64_t value);
JCE_API int64_t JCE_CALL jce_platform_stat_get(JcePlatformServices *svc,
                                               const char *id, int64_t def);

/* ── Leaderboards ────────────────────────────────────────────────────────── */

/* Submit `score` for `user` on `board`.  Keeps the highest score per user.
 * Auto-creates the board.  Returns false on bad args. */
JCE_API bool JCE_CALL jce_platform_lb_submit(JcePlatformServices *svc,
                                             const char *board, const char *user, int64_t score);

/* Query the top `cap` entries of `board`, score-descending (ties resolved by
 * submission order).  `out_scores` and `out_users` are optional parallel output
 * arrays; `out_users` is a flat char[cap][user_stride] block.  Returns the
 * number of entries written (<= cap; 0 if the board is empty/missing). */
JCE_API int JCE_CALL jce_platform_lb_query_top(JcePlatformServices *svc,
                                               const char *board, int cap,
                                               int64_t *out_scores,
                                               char *out_users, int user_stride);

/* ── Cloud-save slots ────────────────────────────────────────────────────── */

JCE_API bool JCE_CALL jce_platform_save_write(JcePlatformServices *svc,
                                              const char *slot, const void *data, uint32_t size);

/* Read `slot` into `out` (cap bytes).  *out_size receives the stored size even
 * when `out` is too small (then returns false).  Pass out=NULL,cap=0 to query
 * size only.  Returns false if the slot is absent. */
JCE_API bool JCE_CALL jce_platform_save_read(JcePlatformServices *svc,
                                             const char *slot, void *out, uint32_t cap,
                                             uint32_t *out_size);

JCE_API bool JCE_CALL jce_platform_save_exists(JcePlatformServices *svc, const char *slot);
JCE_API bool JCE_CALL jce_platform_save_delete(JcePlatformServices *svc, const char *slot);

/* List slot names into a flat char[cap][name_stride] block.  Pass
 * out_names=NULL to just count.  Returns the number of slots. */
JCE_API int  JCE_CALL jce_platform_save_list(JcePlatformServices *svc,
                                             char *out_names, int cap, int name_stride);

JCE_EXTERN_C_END

#endif /* JCE_PLATFORM_SERVICES_H */
