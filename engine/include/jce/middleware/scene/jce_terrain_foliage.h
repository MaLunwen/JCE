/*
 * jce_terrain_foliage.h  Foliage / detail-mesh instance pool.
 *
 * Complements jce_terrain.h (heightmap + splat) with a per-instance
 * mesh authoring layer: trees, grass clumps, rocks scattered across
 * the terrain.  Mirrors Unity TerrainData.treeInstances + detail
 * prototypes at the data layer.
 *
 * Each instance is one POD struct (position / scale / rotation /
 * prototype index).  Instances are bucketed by prototype so the
 * renderer can issue one instanced draw call per prototype.
 *
 * Density painting and prototype-based scatter helpers let the
 * editor terrain-paint tool stamp foliage with the same brush model
 * as height / splat sculpting.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_TERRAIN_FOLIAGE_H
#define JCE_TERRAIN_FOLIAGE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_FOLIAGE_MAX_PROTOTYPES  32
#define JCE_FOLIAGE_PROTO_PATH_LEN  128

typedef struct {
    char  mesh_path     [JCE_FOLIAGE_PROTO_PATH_LEN];
    char  material_path [JCE_FOLIAGE_PROTO_PATH_LEN];
    float bend_factor;          /* wind-influence multiplier (0..1) */
    float min_scale;
    float max_scale;
    /* Bounding sphere radius — used for distance culling. */
    float bounding_radius;
    bool  cast_shadow;
    bool  active;
} JceFoliagePrototype;

typedef struct {
    float    position[3];        /* world XYZ */
    float    rotation_y;         /* yaw in radians */
    float    scale;
    uint16_t prototype;
    /* Colour multiplier baked from terrain splat sample at spawn. */
    uint32_t color_rgba;
} JceFoliageInstance;

typedef struct {
    JceFoliagePrototype prototypes[JCE_FOLIAGE_MAX_PROTOTYPES];

    JceFoliageInstance *instances;
    uint32_t            instance_count;
    uint32_t            instance_capacity;
} JceFoliagePool;

/* Lifecycle. */
JCE_API void jce_foliage_init   (JceFoliagePool *p);
JCE_API void jce_foliage_dispose(JceFoliagePool *p);
JCE_API void jce_foliage_clear  (JceFoliagePool *p);

/* Register prototype N (returns false if N >= MAX or proto null). */
JCE_API bool jce_foliage_set_prototype(JceFoliagePool *p,
                                        uint16_t                idx,
                                        const JceFoliagePrototype *proto);

/* Append one instance.  Grows the array as needed.  Returns the new
 * instance index, or UINT32_MAX on OOM. */
JCE_API uint32_t jce_foliage_add_instance(JceFoliagePool *p,
                                            const JceFoliageInstance *inst);

/* Remove an instance by swap-and-pop. */
JCE_API bool jce_foliage_remove_instance(JceFoliagePool *p, uint32_t idx);

/* Count instances belonging to a given prototype. */
JCE_API uint32_t jce_foliage_count_for_prototype(const JceFoliagePool *p,
                                                   uint16_t prototype);

/* Brush helper — scatter `count` instances within `radius` around
 * `cx,cz` on the XZ plane.  Y position uses caller-supplied callback
 * (typically jce_terrain_sample_height bound through a closure
 * thunked via a context pointer).  Returns the number of instances
 * actually appended. */
typedef float (*JceFoliageHeightFn)(float x, float z, void *user_data);

JCE_API uint32_t jce_foliage_brush_scatter(JceFoliagePool *p,
                                            uint16_t prototype,
                                            float    cx,
                                            float    cz,
                                            float    radius,
                                            uint32_t count,
                                            JceFoliageHeightFn height_fn,
                                            void    *user_data,
                                            uint32_t random_seed);

JCE_EXTERN_C_END

#endif /* JCE_TERRAIN_FOLIAGE_H */
