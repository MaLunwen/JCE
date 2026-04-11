/*
 * jce_streaming.h  Resource streaming for large worlds.
 *
 * Manages background loading and unloading of scene chunks,
 * texture mip levels, and mesh LODs based on camera proximity.
 * Designed for both open-world (distance rings) and RTS
 * (rectangular viewport) streaming strategies.
 *
 * Layer: Resource Streaming (Layer 3).
 *
 * STATUS: Architecture stub — API surface defined, implementation pending.
 */

#ifndef JCE_STREAMING_H
#define JCE_STREAMING_H

#include <stdbool.h>
#include <stdint.h>
#include <jce/core/jce_math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceStreamingSystem JceStreamingSystem;

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
} JceStreamingConfig;

/* ================================================================== */
/* Chunk descriptor                                                    */
/* ================================================================== */

typedef struct {
    uint32_t chunk_id;
    jce_vec3 center;
    float    radius;
    const char *asset_path;   /* path to the chunk asset (scene file) */
} JceStreamChunk;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config);
void                jce_streaming_destroy(JceStreamingSystem *sys);

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
 * Queues loads/unloads as needed.  Call once per frame. */
void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Number of chunks currently loaded in memory. */
uint32_t jce_streaming_loaded_count(const JceStreamingSystem *sys);

/* Number of chunks currently being loaded (in-flight). */
uint32_t jce_streaming_pending_count(const JceStreamingSystem *sys);

/* Current memory usage in bytes. */
uint64_t jce_streaming_memory_used(const JceStreamingSystem *sys);

/* Check if a specific chunk is loaded. */
bool jce_streaming_chunk_loaded(const JceStreamingSystem *sys,
                                 uint32_t chunk_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_STREAMING_H */
