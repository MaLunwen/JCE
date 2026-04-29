/*
 * jce_world_streamer.h  High-level open-world scene streaming.
 *
 * Wraps JceStreamingSystem to provide GTA-style chunk loading that
 * automatically instantiates / destroys scene entities as the camera
 * moves through the world.
 *
 * Each registered chunk maps to a scene-fragment JSON file (same format
 * as a full scene file, saved by the editor) that is loaded additively
 * into the live JceScene when the camera enters the load radius and
 * unloaded when it leaves the unload radius.
 *
 *   JceWorldStreamer *ws = jce_world_streamer_create(&cfg, scene, fs, pool);
 *   jce_world_streamer_register_chunk(ws, 0, center, 120.f, "chunks/c0.jscene");
 *   // per-frame:
 *   jce_world_streamer_update(ws, camera_pos);
 *   // teardown:
 *   jce_world_streamer_destroy(ws);
 *
 * Thread safety:
 *   jce_world_streamer_update() may be called from the main thread only.
 *   Entity creation and destruction happen synchronously on the main thread
 *   after the background I/O completes, so bgfx handles are always
 *   created/destroyed on the render thread.
 *
 * Layer: Resource (Layer 3).
 */

#ifndef JCE_WORLD_STREAMER_H
#define JCE_WORLD_STREAMER_H

#include <jce/middleware/streaming/jce_streaming.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceWorldStreamer JceWorldStreamer;
typedef struct JceScene         JceScene;
typedef struct JceFileSystem    JceFileSystem;
typedef struct JceThreadPool    JceThreadPool;

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

typedef struct {
    JceStreamMode mode;            /* JCE_STREAM_RADIAL or JCE_STREAM_RECTANGULAR */
    float         load_radius;     /* metres — start loading inside this distance */
    float         unload_radius;   /* metres — unload beyond this distance */
    uint32_t      max_pending;     /* max concurrent background loads */
    uint32_t      budget_mb;       /* memory budget in MiB (0 = unlimited) */
    bool          single_thread;   /* force cooperative mode (auto-detected on WASM) */
    float         frame_budget_ms; /* per-frame time budget in cooperative mode */
} JceWorldStreamConfig;

/* Sensible defaults for a GTA VC-scale open world. */
static inline JceWorldStreamConfig jce_world_stream_config_default(void)
{
    JceWorldStreamConfig c;
    c.mode            = JCE_STREAM_RADIAL;
    c.load_radius     = 150.0f;
    c.unload_radius   = 200.0f;
    c.max_pending     = 4;
    c.budget_mb       = 256;
    c.single_thread   = false;
    c.frame_budget_ms = 2.0f;
    return c;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Create a world streamer bound to an existing scene and file system.
   thread_pool may be NULL; if so, single-thread mode is used. */
JCE_API JceWorldStreamer *jce_world_streamer_create(
    const JceWorldStreamConfig *config,
    JceScene                   *scene,
    JceFileSystem              *fs,
    JceThreadPool              *thread_pool);

JCE_API void jce_world_streamer_destroy(JceWorldStreamer *ws);

/* ================================================================== */
/* Chunk registration                                                  */
/* ================================================================== */

/* Register a world chunk.
   chunk_id   — unique 32-bit identifier (e.g. grid cell index)
   center     — world-space position of the chunk centre
   radius     — spatial radius of the chunk (used for culling / distance test)
   asset_path — VFS path to the scene-fragment JSON file */
JCE_API void jce_world_streamer_register_chunk(JceWorldStreamer *ws,
                                                uint32_t         chunk_id,
                                                jce_vec3         center,
                                                float            radius,
                                                const char      *asset_path);

JCE_API void jce_world_streamer_unregister_chunk(JceWorldStreamer *ws,
                                                  uint32_t         chunk_id);

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* Drive streaming decisions and apply any pending chunk loads/unloads
   to the scene.  Call once per frame from the main thread. */
JCE_API void jce_world_streamer_update(JceWorldStreamer *ws, jce_vec3 camera_pos);

/* ================================================================== */
/* Stats / queries                                                     */
/* ================================================================== */

JCE_API uint32_t jce_world_streamer_loaded_count(const JceWorldStreamer *ws);
JCE_API uint32_t jce_world_streamer_pending_count(const JceWorldStreamer *ws);
JCE_API uint64_t jce_world_streamer_memory_used(const JceWorldStreamer *ws);
JCE_API uint32_t jce_world_streamer_evicted_count(const JceWorldStreamer *ws);

/* Total number of entities currently owned and managed by the streamer
   (sum of all loaded chunk entity counts). */
JCE_API uint32_t jce_world_streamer_entity_count(const JceWorldStreamer *ws);

/* Total number of registered chunks. */
JCE_API uint32_t jce_world_streamer_chunk_count(const JceWorldStreamer *ws);

JCE_EXTERN_C_END

#endif /* JCE_WORLD_STREAMER_H */
