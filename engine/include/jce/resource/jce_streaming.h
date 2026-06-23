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

/* Update a LOADED chunk's accounted residency in bytes.  The streaming system
 * only knows the raw loaded payload size; the consumer (e.g. the world
 * streamer) knows the true cost of what that payload spawned — entities,
 * components, asset references — which dominates the memory budget.  Reporting
 * it here makes LRU eviction and the pressure signal actually engage instead
 * of counting tiny JSON byte sizes that never reach the budget (audit F3).
 * No-op if the chunk is not currently LOADED. */
void jce_streaming_set_chunk_residency(JceStreamingSystem *sys,
                                       uint32_t chunk_id, uint64_t bytes);

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

/* Total number of LRU-evicted chunks since system creation.  An eviction
   is triggered when memory_used would exceed budget_mb and at least one
   loaded chunk lies *outside* the load radius (least-recently-touched
   wins).  Use this stat to gauge whether your budget is too tight. */
JCE_API uint32_t jce_streaming_evicted_count(const JceStreamingSystem *sys);

/* ================================================================== */
/* Memory pressure                                                     */
/* ================================================================== */
/* Three-level back-pressure signal driven by `memory_used /
 * budget_bytes`.  Updated once per jce_streaming_update().
 *
 *   OK   (< 0.85)  : free to load anything
 *   SOFT (>= 0.85) : near budget; gameplay/streaming code SHOULD
 *                    downgrade mip selection or pause prefetch
 *   HARD (>= 1.00) : at/over budget; new loads are refused even if
 *                    LRU eviction failed to reclaim space
 *
 * Threshold for SOFT is tunable via a future config field; HARD is
 * always exactly the configured budget.  Callers can poll
 * jce_streaming_get_pressure() or register a callback to be notified
 * on level transitions only (no per-frame spam). */
typedef enum JceStreamingPressure {
    JCE_STREAM_PRESSURE_OK   = 0,
    JCE_STREAM_PRESSURE_SOFT = 1,
    JCE_STREAM_PRESSURE_HARD = 2,
} JceStreamingPressure;

/* Current level (cheap to call, no locks). */
JCE_API JceStreamingPressure jce_streaming_get_pressure(const JceStreamingSystem *sys);

/* Highest level seen since system creation — useful for tuning the
 * budget retroactively after a play session. */
JCE_API JceStreamingPressure jce_streaming_pressure_high_water(const JceStreamingSystem *sys);

/* String form for logs / profiler labels ("OK"/"SOFT"/"HARD"). */
JCE_API const char *jce_streaming_pressure_name(JceStreamingPressure p);

/* Optional callback fired ONCE on each pressure-level transition.
 * Useful to e.g. nudge mip-bias settings or page out far chunks.
 * `prev` and `curr` always differ.  Set fn=NULL to disable. */
typedef void (*JceStreamingPressureFn)(JceStreamingPressure prev,
                                        JceStreamingPressure curr,
                                        uint64_t              memory_used,
                                        uint64_t              budget_bytes,
                                        void                 *user_data);

JCE_API void jce_streaming_set_pressure_callback(JceStreamingSystem    *sys,
                                                  JceStreamingPressureFn fn,
                                                  void                  *user_data);

/*
 * Engine-internal pressure hook chain.
 *
 * The streaming subsystem invokes every registered hook (in registration
 * order) BEFORE firing the user callback set via
 * jce_streaming_set_pressure_callback().  Hooks let engine layers
 * (texture mip streaming, audio quality, etc.) react to back-pressure
 * without stomping on the single user slot.
 *
 * Returns true on success, false if the hook table is full or `fn` is
 * NULL.  At most JCE_STREAMING_MAX_PRESSURE_HOOKS hooks may be
 * registered per system.
 */
#define JCE_STREAMING_MAX_PRESSURE_HOOKS 4

JCE_API bool jce_streaming_add_pressure_hook(JceStreamingSystem    *sys,
                                              JceStreamingPressureFn fn,
                                              void                  *user_data);

/*
 * Force the streaming system to HARD pressure level immediately and
 * fire all registered hooks + the user callback.  Used by the
 * application lifecycle bridge to react to OS LOW_MEMORY signals
 * (P3-B.3): even though our own budget is not yet exceeded, the OS
 * has told us to release everything we can.  The actual pressure
 * level is then recomputed from used/budget on the next streaming
 * update, so callers do not need to "reset" it.
 *
 * Safe to call with NULL.  Main-thread only.
 */
JCE_API void jce_streaming_signal_low_memory(JceStreamingSystem *sys);

/*
 * Broadcast jce_streaming_signal_low_memory() to every live streaming
 * system created in this process.  Called from the engine's internal
 * lifecycle listener registered at jce_engine_create().
 */
JCE_API void jce_streaming_signal_low_memory_all(void);

/* Number of HARD-pressure load refusals since system creation.  Each
 * tick that a load was refused due to over-budget memory increments
 * this counter — a non-zero value proves the back-pressure path is
 * actually firing (and your budget is too tight). */
JCE_API uint32_t jce_streaming_refused_loads(const JceStreamingSystem *sys);

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
