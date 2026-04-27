/*
 * jce_streaming.h  Resource streaming for large worlds.
 *
 * Manages background loading and unloading of scene chunks,
 * texture mip levels, and mesh LODs based on camera proximity.
 * Designed for both open-world (distance rings) and RTS
 * (rectangular viewport) streaming strategies.
 *
 * On Emscripten (WASM), threads are restricted.  When single_thread
 * is true (auto-detected on web), the system uses a cooperative
 * frame-budget approach: each jce_streaming_update() call processes
 * at most one pending load/unload within the given time budget.
 *
 * Layer: Resource Streaming (Layer 3).
 */

#ifndef JCE_STREAMING_H
#define JCE_STREAMING_H


#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceStreamingSystem JceStreamingSystem;
typedef struct JceFileSystem      JceFileSystem;
typedef struct JceThreadPool      JceThreadPool;

/* ================================================================== */
/* Streaming strategy                                                  */
/* ================================================================== */

typedef enum {
    JCE_STREAM_RADIAL,       /* distance rings around camera (open world) */
    JCE_STREAM_RECTANGULAR,  /* viewport-based rectangle (RTS / top-down) */
} JceStreamMode;

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

typedef struct {
    JceStreamMode mode;
    float         load_radius;      /* distance to start loading */
    float         unload_radius;    /* distance to start unloading */
    uint32_t      max_pending;      /* max concurrent async loads */
    uint32_t      budget_mb;        /* memory budget in MiB */

    /* Single-thread mode: when true, loading is done cooperatively
       within jce_streaming_update() using a per-frame time budget.
       Auto-detected on Emscripten if not explicitly set. */
    bool          single_thread;

    /* Per-frame time budget for single-thread mode, in milliseconds.
       Default: 2.0 ms (leaves headroom in a 16ms/60fps frame). */
    float         frame_budget_ms;
} JceStreamingConfig;

/* ================================================================== */
/* Chunk descriptor                                                    */
/* ================================================================== */

typedef enum {
    JCE_CHUNK_UNLOADED  = 0,
    JCE_CHUNK_LOADING   = 1,
    JCE_CHUNK_LOADED    = 2,
    JCE_CHUNK_UNLOADING = 3,
} JceChunkState;

typedef struct {
    uint32_t chunk_id;
    jce_vec3 center;
    float    radius;
    const char *asset_path;   /* path to the chunk asset (scene file) */
} JceStreamChunk;

/* ================================================================== */
/* Chunk load callback                                                 */
/* ================================================================== */

/* Called when a chunk finishes loading (success or failure).
 * data: loaded chunk data (NULL on failure)
 * size: data size in bytes
 * user_data: user-provided context
 */
typedef void (*JceChunkLoadedFn)(uint32_t chunk_id, void *data, size_t size,
                                  void *user_data);

/* Called when a chunk is about to be unloaded.
 * user_data: user-provided context
 */
typedef void (*JceChunkUnloadedFn)(uint32_t chunk_id, void *user_data);

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config);
JCE_API void                jce_streaming_destroy(JceStreamingSystem *sys);

/* Bind file system for loading. Must be called before streaming starts. */
JCE_API void jce_streaming_set_filesystem(JceStreamingSystem *sys, JceFileSystem *fs);

/* Bind thread pool for multi-threaded loading (optional).
 * If not set, single_thread mode is forced. */
JCE_API void jce_streaming_set_thread_pool(JceStreamingSystem *sys, JceThreadPool *pool);

/* Set callbacks for chunk load/unload events (optional). */
void jce_streaming_set_callbacks(JceStreamingSystem *sys,
                                  JceChunkLoadedFn on_loaded,
                                  JceChunkUnloadedFn on_unloaded,
                                  void *user_data);

/* ================================================================== */
/* Chunk registration                                                  */
/* ================================================================== */

/* Register a chunk that can be streamed in/out. */
void jce_streaming_register_chunk(JceStreamingSystem *sys,
                                   const JceStreamChunk *chunk);

/* Unregister a chunk. */
void jce_streaming_unregister_chunk(JceStreamingSystem *sys,
                                     uint32_t chunk_id);

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* Update streaming based on current camera position.
 * Queues loads/unloads as needed.  Call once per frame.
 *
 * In single-thread mode, this processes pending loads within
 * the configured frame_budget_ms time budget. */
JCE_API void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Number of chunks currently loaded in memory. */
JCE_API uint32_t jce_streaming_loaded_count(const JceStreamingSystem *sys);

/* Number of chunks currently being loaded (in-flight). */
JCE_API uint32_t jce_streaming_pending_count(const JceStreamingSystem *sys);

/* Current memory usage in bytes. */
JCE_API uint64_t jce_streaming_memory_used(const JceStreamingSystem *sys);

/* Check if a specific chunk is loaded. */
bool jce_streaming_chunk_loaded(const JceStreamingSystem *sys,
                                 uint32_t chunk_id);

/* Query the state of a specific chunk. */
JceChunkState jce_streaming_chunk_state(const JceStreamingSystem *sys,
                                        uint32_t chunk_id);

/* Get the loaded data for a chunk (NULL if not loaded). */
void *jce_streaming_chunk_data(const JceStreamingSystem *sys,
                                uint32_t chunk_id, size_t *out_size);

/* Return true if the streaming system is operating in single-thread mode. */
JCE_API bool jce_streaming_is_single_thread(const JceStreamingSystem *sys);

JCE_EXTERN_C_END

#endif /* JCE_STREAMING_H */
