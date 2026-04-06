/*
 * jce_async_pool.h  Fixed-size thread pool for async asset loading.
 *
 * Architecture:
 *   - N worker threads (configurable, default 2)
 *   - Lock-based FIFO request queue (SDL_Mutex + SDL_Condition)
 *   - Workers: decompress + decode (CPU-heavy, off main thread)
 *   - Completion list: polled by main thread each frame
 *
 * Internal module — not part of the public API.
 */

#ifndef JCE_ASYNC_POOL_H
#define JCE_ASYNC_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <SDL3/SDL_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Request & Result                                                    */
/* ================================================================== */

/* Forward declarations. */
typedef struct PakArchive    PakArchive;
typedef struct JceFileSystem JceFileSystem;
typedef struct JceAudio      JceAudio;

/* Async request types. */
typedef enum {
	JCE_ASYNC_TEXTURE,
	JCE_ASYNC_AUDIO,
	JCE_ASYNC_MESH,
	JCE_ASYNC_MODEL,
	JCE_ASYNC_RAW,
	JCE_ASYNC_FONT
} JceAsyncRequestType;

/* Load parameters (copied into request). */
typedef struct JceAsyncLoadInfo {
	int      texture_sampler_mode;
	float    font_size;
	uint32_t font_codepoints[256];
	int      font_codepoint_count;
} JceAsyncLoadInfo;

/*
 * An async work item. Allocated by the pool, freed after finalization.
 *
 * Lifecycle:
 *   1. Created by jce_pool_submit()
 *   2. Picked up by a worker thread
 *   3. Worker sets decoded_data + decoded_size, flips done flag
 *   4. Main thread calls finalize callback
 *   5. Pool frees the request
 */
typedef struct JceAsyncRequest {
	/* --- Input (read-only after submission) --- */
	JceAsyncRequestType type;
	uint16_t            slot_index;    /* target slot in asset manager */
	char                path[256];     /* virtual asset path */
	PakArchive         *pak;
	JceFileSystem      *fs;
	JceAsyncLoadInfo    info;

	/* --- Output (written by worker) --- */
	void               *decoded_data;  /* type-specific decoded result */
	size_t              decoded_size;
	bool                success;
	bool                is_cooked;     /* true if data came from .jceasset */
	SDL_AtomicInt       done;          /* 0=working, 1=complete */

	/* --- Internal linked list --- */
	struct JceAsyncRequest *next;
} JceAsyncRequest;

/* ================================================================== */
/* Pool                                                                */
/* ================================================================== */

typedef struct JceAsyncPool JceAsyncPool;

/*
 * Create thread pool with N worker threads.
 * num_workers=0 → default (2 on mobile, 3 on desktop).
 */
JceAsyncPool *jce_pool_create(uint32_t num_workers);

/* Shutdown: signals all workers to exit, joins threads, frees pool. */
void jce_pool_destroy(JceAsyncPool *pool);

/*
 * Submit a load request.
 * The request struct is allocated internally and returned for tracking.
 * Caller must NOT free the returned pointer.
 */
JceAsyncRequest *jce_pool_submit(JceAsyncPool *pool,
                                 JceAsyncRequestType type,
                                 uint16_t slot_index,
                                 const char *path,
                                 PakArchive *pak,
                                 JceFileSystem *fs,
                                 const JceAsyncLoadInfo *info);

/*
 * Drain completed requests.
 *
 * Returns a linked list of completed requests (via ->next).
 * Caller processes each, then calls jce_pool_free_request() on each.
 *
 * max_count: maximum number to drain (0 = all).
 */
JceAsyncRequest *jce_pool_drain(JceAsyncPool *pool, uint32_t max_count);

/* Free a completed request after finalization. */
void jce_pool_free_request(JceAsyncRequest *req);

/* Get number of pending (queued + in-flight) requests. */
uint32_t jce_pool_pending_count(const JceAsyncPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASYNC_POOL_H */
