/*
 * jce_world_partition.h  Engine-side world partitioning (Direction A1).
 *
 * Takes ONE authored scene and splits it into the SAME streaming roster +
 * per-cell fragment format the runtime already consumes (jce_streaming.c +
 * jce_world_streamer.c) — the engine equivalent of UE World Partition's
 * "author one world, the engine grids it", replacing the street_demo-specific
 * offline Python (build/gen_city_streaming.py).
 *
 * This is a PURE tree transform: it serialises the scene to JSON once, then
 * spatial-hashes each STREAMABLE entity (by its world-AABB centre) into a
 * grid cell, MOVES those entity nodes into per-cell fragment trees, leaves
 * RESIDENT entities in the master, and injects the computed `streaming` block
 * (the chunk roster) into the master.  It performs NO file I/O and does NOT
 * mutate the live scene — the caller (editor) computes the per-entity AABBs
 * (which needs mesh resolution) and writes the returned trees to disk.  This
 * keeps the algorithm headless-testable and reusable.
 */

#ifndef JCE_WORLD_PARTITION_H
#define JCE_WORLD_PARTITION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

struct JceScene;

/* One authored entity, as classified + measured by the caller. */
typedef struct {
    uint64_t entity_id;   /* matches the "id" field in the serialised scene  */
    float    center[3];   /* world-space AABB centre                          */
    float    radius;      /* world-space bounding radius (for chunk sphere)   */
    bool     streamable;  /* false = keep resident in the master scene        */
} JcePartitionEntity;

typedef struct {
    float       cell_size;          /* XZ grid cell size in metres (>0)        */
    float       load_radius;        /* streaming load radius written to roster */
    float       unload_radius;      /* streaming unload radius                  */
    uint32_t    id_base;            /* first chunk id (subsequent +1)          */
    const char *fragment_path_fmt;  /* printf fmt, two %d (gx,gz), VFS-relative
                                     * e.g. "scenes/chunks/cell_%d_%d.scene.json"
                                     * — MUST contain cell_<gx>_<gz> so the A2
                                     * HLOD attach can parse the coordinates.  */
} JcePartitionConfig;

typedef struct {
    int      gx, gz;         /* grid coordinates                              */
    uint32_t id;             /* chunk id (id_base + index)                    */
    float    center[3];      /* chunk bounding-sphere centre (entity centroid)*/
    float    radius;         /* chunk bounding-sphere radius                  */
    char     path[256];      /* VFS-relative fragment path (roster + writeout)*/
    JceJson *json;           /* fragment scene tree {"scene":{...}}; owned by
                              * the result, freed by jce_world_partition_free */
} JcePartitionChunk;

typedef struct {
    JceJson           *master_json;     /* master scene tree: residents + the
                                         * injected streaming roster (owned)   */
    JcePartitionChunk *chunks;          /* per-cell fragments (owned)          */
    uint32_t           chunk_count;
    uint32_t           resident_count;  /* entities kept in the master         */
    uint32_t           streamed_count;  /* entities moved into fragments        */
} JcePartitionResult;

/* Build the master + per-cell fragment trees from `scene` and the caller's
 * per-entity classification.  Returns false (and leaves *out zeroed) on error
 * — including when the cell count would exceed JCE_SCENE_MAX_STREAM_CHUNKS
 * (raise cell_size).  On success the caller writes the trees to disk and then
 * frees them with jce_world_partition_free(). */
JCE_API bool jce_world_partition_build(const struct JceScene     *scene,
                                       const JcePartitionEntity  *ents,
                                       uint32_t                   ent_count,
                                       const JcePartitionConfig  *cfg,
                                       JcePartitionResult        *out);

JCE_API void jce_world_partition_free(JcePartitionResult *res);

JCE_EXTERN_C_END

#endif /* JCE_WORLD_PARTITION_H */
