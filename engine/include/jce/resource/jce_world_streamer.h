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

#include <jce/resource/jce_streaming.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceWorldStreamer JceWorldStreamer;
typedef struct JceScene         JceScene;
typedef struct JceFileSystem    JceFileSystem;
typedef struct JceThreadPool    JceThreadPool;
typedef struct JceSceneStreamingSettings JceSceneStreamingSettings;

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
 * thread_pool is retained for source compatibility; streaming now owns a
 * structured executor and does not borrow the frame-job pool. */
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

/* Register every chunk authored in a scene's streaming settings
 * (jce_scene_get_streaming_settings).  Entries with an empty path are
 * skipped.  Convenience used by both the editor preview streamer and
 * the runtime's default_main so the two stay in lockstep. */
JCE_API void jce_world_streamer_register_from_scene_settings(
    JceWorldStreamer                *ws,
    const JceSceneStreamingSettings *settings);

/* ================================================================== */
/* Entity spawn/despawn notifications (editor hierarchy integration)    */
/* ================================================================== */

/* Invoked on the main thread when the streamer spawns the entities of a
 * freshly-loaded chunk (on_spawn) and just BEFORE it destroys a chunk's
 * entities on unload (on_despawn), so the entity ids are still valid in the
 * despawn callback.  `entity_ids` are JceEntity values (uint64_t); `count` is
 * the number for that chunk.  The editor registers these to mirror streamed
 * entities into its hierarchy/selection so they are first-class objects; the
 * standalone runtime sets none (NULL = no-op).  Cheap: fired per chunk
 * load/unload, never per frame. */
typedef void (*JceWorldStreamerEntityCb)(const uint64_t *entity_ids,
                                         uint32_t count, void *user);

JCE_API void jce_world_streamer_set_entity_callbacks(
    JceWorldStreamer         *ws,
    JceWorldStreamerEntityCb  on_spawn,
    JceWorldStreamerEntityCb  on_despawn,
    void                     *user);

/* Invoked on the main thread when a CHUNK becomes resident (loaded == true,
 * fired just after its entities are spawned) and when it is evicted
 * (loaded == false, fired as the chunk's entities are removed).  Unlike the
 * per-entity callbacks above this carries the chunk id, so a caller can map a
 * chunk to its own per-chunk resource (e.g. an HLOD far-skyline proxy that
 * must be hidden while the detailed chunk is resident and shown once it
 * unloads) without reverse-mapping entity ids.  Cheap: fired per chunk
 * load/unload, never per frame.  NULL = no-op (the runtime sets none). */
typedef void (*JceWorldStreamerChunkCb)(uint32_t chunk_id, bool loaded,
                                        void *user);

JCE_API void jce_world_streamer_set_chunk_callback(
    JceWorldStreamer        *ws,
    JceWorldStreamerChunkCb  on_chunk_state,
    void                    *user);

/* ================================================================== */
/* Real residency reporting (VRAM ceiling)                             */
/* ================================================================== */

/* Optional callback: given the entity ids a chunk spawned, return the REAL GPU
 * footprint (bytes) of the resources they reference — typically the sum of each
 * entity's resolved model VB+IB+texture bytes (jce_scene_renderer_model_path_
 * vram_bytes).  The streamer feeds THIS to jce_streaming_set_chunk_residency so
 * the memory budget + LRU evict against real VRAM instead of a flat per-entity
 * estimate (so a 256 MiB budget actually bounds VRAM).  When unset, or when it
 * returns 0 (e.g. a chunk's async model decodes haven't landed yet), the
 * streamer falls back to its per-entity estimate; it re-reports each update as
 * models finish loading so the number converges to real bytes.  Fired per chunk
 * load, never per frame.  Set NULL to clear. */
typedef uint64_t (*JceWorldStreamerResidencyFn)(const uint64_t *entity_ids,
                                                uint32_t count, void *user);

JCE_API void jce_world_streamer_set_residency_query(
    JceWorldStreamer            *ws,
    JceWorldStreamerResidencyFn  query,
    void                        *user);

/* ================================================================== */
/* HLOD far-skyline proxy coordination                                 */
/* ================================================================== */

/* Wire the always-resident per-chunk HLOD "massing" proxies to the streamer so
 * a chunk's cheap far-proxy is HIDDEN once its detailed geometry streams in and
 * SHOWN again the moment the chunk unloads — eliminating the double-draw /
 * z-fight of the proxy box over the real streamed buildings.
 *
 * The proxies are baked (tools/worldgen/gen_hlod.py) into the MASTER scene as
 * always-resident entities named "HLOD_<gx>_<gz>", one per streamable cell.
 * This builds the chunk-id -> proxy-entity map from the scene's authored
 * streaming table (chunk fragment path "…/cell_<gx>_<gz>.scene.json" -> proxy
 * name "HLOD_<gx>_<gz>", resolved against each entity's EditorMeta name, which the
 * shared scene loader populates in BOTH the editor and the cooked runtime
 * scene) and installs the streamer chunk callback to toggle the proxy's
 * MeshRenderer.  It then chains to `extra_cb` (with `extra_ud`) if non-NULL, so
 * a caller can layer extra per-chunk work on the same single callback slot
 * (the editor uses this for its hierarchy chunk-grouping).
 *
 * This is the ONE implementation shared by the editor (scene-view preview +
 * Play) and the standalone runtime (default_main): both call this so the
 * shipped exe hides proxies exactly like the editor does.  All proxies are
 * reset to VISIBLE at attach (baseline before any chunk is resident).  Safe
 * no-op when no HLOD proxies were baked (the map ends up empty; the chunk
 * callback is still installed so `extra_cb` keeps firing).  Call after creating
 * + registering the streamer. */
JCE_API void jce_world_streamer_attach_hlod(JceWorldStreamer        *ws,
                                            JceScene                *scene,
                                            JceWorldStreamerChunkCb  extra_cb,
                                            void                    *extra_ud);

/* ================================================================== */
/* Per-frame update                                                    */
/* ================================================================== */

/* Drive streaming decisions and apply any pending chunk loads/unloads
   to the scene.  Call once per frame from the main thread. */
JCE_API void jce_world_streamer_update(JceWorldStreamer *ws, jce_vec3 camera_pos);

/* ================================================================== */
/* Preview / authoring load-override                                   */
/* ================================================================== */

/* Forward a preview load-override to the wrapped streaming system
 * (jce_streaming_set_preview): RADIUS = current distance-ring behaviour
 * (default, exact existing behaviour); ALL = every registered chunk stays
 * resident (the editor's "Full World" preview — zooming/orbiting out no
 * longer pops far chunks); FILTER = only the supplied chunk ids stay
 * resident (debug a subset, synced with the hierarchy).  filter_ids is
 * COPIED and only consulted in FILTER mode (pass NULL/0 otherwise).
 * The per-frame load budget + async ordering are unchanged, so a large
 * world streams in over a few seconds.  Safe with NULL ws. */
JCE_API void jce_world_streamer_set_preview_load(JceWorldStreamer    *ws,
                                                 JceStreamPreviewMode mode,
                                                 const uint32_t      *filter_ids,
                                                 uint32_t             count);

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

/* The config this streamer was created with (copied at create time —
 * JceWorldStreamConfig has no setters; recreate the streamer to change
 * it).  Returns the defaults for NULL. */
JCE_API JceWorldStreamConfig jce_world_streamer_get_config(
    const JceWorldStreamer *ws);

/* Passthroughs to the wrapped JceStreamingSystem's pressure / refusal /
 * per-chunk-state queries (the wrapped system itself is private). */
JCE_API JceStreamingPressure jce_world_streamer_pressure(
    const JceWorldStreamer *ws);
JCE_API JceStreamingPressure jce_world_streamer_pressure_high_water(
    const JceWorldStreamer *ws);
JCE_API uint32_t jce_world_streamer_refused_loads(const JceWorldStreamer *ws);
JCE_API JceChunkState jce_world_streamer_chunk_state(
    const JceWorldStreamer *ws, uint32_t chunk_id);

JCE_EXTERN_C_END

#endif /* JCE_WORLD_STREAMER_H */
