/* jce_archive_loader.h
 *
 * Runtime loader for the JCE Archive format (JPAK, format_version 1) — the
 * behavioural contract of spec §13 plus the loading-performance machinery of
 * §14: a reference-counted, LRU-evicted resource cache layered over an open
 * JceArchive, with both a synchronous acquire path and an asynchronous,
 * frame-budgeted request path.
 *
 * Why this exists.  Reading a resource straight off the archive (jce_archive_
 * read) is the right primitive but it (a) blocks the calling thread for the
 * whole I/O + decompression and (b) reloads the same resource every time it is
 * asked for.  §14.1 forbids blocking the main thread, and §14.2 mandates a
 * load-once cache governed by reference counting with LRU eviction under memory
 * pressure.  This loader provides exactly that on top of the archive reader.
 *
 * Threading model (spec §13).  Lookups over the resident index are read-only
 * and safe to issue concurrently.  The shared zstd decode context inside the
 * archive is NOT concurrency-safe, so the loader serialises the actual
 * read+decompress with an internal I/O lock; this is irrelevant on the single-
 * core baseline (where there is at most one worker) and keeps multi-core decode
 * correct at the cost of serialising the heavy step — an acceptable, documented
 * trade since archive reads are largely I/O bound.
 *
 * Worker model.  When `worker_count` > 0 a small worker pool (jce_jobs) performs
 * I/O + decompression off the main thread; jce_archive_loader_tick() integrates
 * completed loads into the cache on the main thread.  When `worker_count` == 0
 * — the single-core / WebAssembly baseline where worker parallelism is
 * unavailable — requests are serviced inline by tick(), bounded to
 * `frame_budget_ms` per call so a heavy load spreads across several frames
 * rather than producing one long stall (§14.1, §15).
 */
#ifndef JCE_ARCHIVE_LOADER_H
#define JCE_ARCHIVE_LOADER_H

#include <jce/os/core/jce_defs.h>
#include <jce/resource/jce_archive.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque loader handle. */
typedef struct JceArchiveLoader JceArchiveLoader;

/* A reference-counted view of a loaded resource.  `data`/`size` stay valid
 * until the matching jce_archive_loader_release(); the loader will not evict a
 * resource while any reference is held.  For an uncompressed, unencrypted,
 * page-aligned entry under an active memory mapping, `data` points directly
 * into the mapped archive (zero-copy, spec §8.2 / §13); otherwise it points at
 * a decompressed buffer the loader owns. */
typedef struct JceArchiveResource {
    uint64_t    hash; /* XXH3-64 of the normalized virtual path */
    const void *data; /* resource bytes (decompressed or mapped)  */
    uint32_t    size; /* number of bytes at `data`                */
} JceArchiveResource;

/* Async request handle; 0 is never a valid id. */
typedef uint64_t JceArchiveRequestId;

typedef struct JceArchiveLoaderConfig {
    /* Worker threads for asynchronous loads.  0 selects the inline,
     * frame-budgeted single-core path (no worker threads created). */
    uint32_t worker_count;
    /* Soft cache budget in bytes for owned (decompressed) resources; the loader
     * evicts least-recently-used, unreferenced resources once exceeded.  0 =
     * unbounded (no eviction).  Zero-copy mapped resources cost no heap and are
     * not counted. */
    size_t   cache_budget_bytes;
    /* Per-call decompression budget for tick() on the inline path, in
     * milliseconds.  Overflow work is deferred to later ticks.  <= 0 selects
     * the §15 default (~2.0 ms). */
    double   frame_budget_ms;
    /* When non-zero, verify each resource's content CRC after decompression
     * (recommended for development builds, spec §9.1). */
    int      verify_crc;
} JceArchiveLoaderConfig;

/* Create a loader over an already-open archive.  The archive is borrowed and
 * MUST outlive the loader; the caller still owns and closes it.  `cfg` may be
 * NULL for defaults (no workers, unbounded cache, ~2 ms budget, no CRC).
 * Returns NULL on invalid arguments or allocation failure. */
JCE_API JceArchiveLoader *jce_archive_loader_create(JceArchive *archive,
                                                    const JceArchiveLoaderConfig *cfg);

/* Destroy the loader, waiting for any in-flight worker loads to finish and
 * freeing every cached resource.  Does NOT close the underlying archive.
 * Holding resource references past this call is a use-after-free. */
JCE_API void jce_archive_loader_destroy(JceArchiveLoader *loader);

/* ── Synchronous acquire (spec §13 read contract) ───────────────────────── */

/* Acquire `path`, loading it now if not already cached, and return a
 * reference-counted handle (refcount incremented).  Repeated acquires of the
 * same resource share one cached copy.  Returns NULL when the resource is not
 * present or a load error occurs.  Release with jce_archive_loader_release(). */
JCE_API const JceArchiveResource *jce_archive_loader_acquire(JceArchiveLoader *loader,
                                                             const char *path);

/* As above, keyed by a precomputed XXH3-64 path hash (jce_archive_hash_path). */
JCE_API const JceArchiveResource *jce_archive_loader_acquire_hash(JceArchiveLoader *loader,
                                                                  uint64_t hash);

/* Drop one reference previously taken by acquire / a ready poll.  When the last
 * reference is released the resource becomes eligible for LRU eviction. */
JCE_API void jce_archive_loader_release(JceArchiveLoader *loader,
                                        const JceArchiveResource *res);

/* ── Asynchronous loading (spec §14.1) ──────────────────────────────────── */

/* Enqueue an asynchronous load of `path` and return a request id, or 0 if the
 * resource is absent.  Never blocks.  Poll for completion with
 * jce_archive_loader_poll(); progress is driven by jce_archive_loader_tick(). */
JCE_API JceArchiveRequestId jce_archive_loader_request(JceArchiveLoader *loader,
                                                       const char *path);

/* Poll a request.  Returns 1 when ready (sets *out to a referenced handle the
 * caller must release, and consumes the request), 0 while still pending, and a
 * negative value on failure / not-found (also consuming the request).  *out is
 * only written on the ready result. */
JCE_API int jce_archive_loader_poll(JceArchiveLoader *loader, JceArchiveRequestId id,
                                    const JceArchiveResource **out);

/* Drive loading progress once per frame on the main thread: integrate completed
 * worker loads into the cache and, on the inline (worker_count == 0) path,
 * service queued loads bounded by the frame budget (spec §14.1). */
JCE_API void jce_archive_loader_tick(JceArchiveLoader *loader);

/* Predictively warm the cache (e.g. during a loading screen, spec §14.2):
 * enqueue `path` for loading without returning a handle.  No-op if absent or
 * already cached/queued. */
JCE_API void jce_archive_loader_preload(JceArchiveLoader *loader, const char *path);

/* ── Introspection ──────────────────────────────────────────────────────── */

/* Bytes currently held by owned (decompressed) cached resources. */
JCE_API size_t jce_archive_loader_cache_bytes(const JceArchiveLoader *loader);

/* Number of resources resident in the cache (ready or loading). */
JCE_API uint32_t jce_archive_loader_cache_count(const JceArchiveLoader *loader);

/* Number of loads not yet integrated into the cache (queued or in flight). */
JCE_API uint32_t jce_archive_loader_pending_count(const JceArchiveLoader *loader);

JCE_EXTERN_C_END

#endif /* JCE_ARCHIVE_LOADER_H */
