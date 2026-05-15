/*
 * jce_lightmapper_bake.h  Triangle-mesh AO + indirect-light baker.
 *
 * Existing jce_lightmapper.h supports box / sphere occluder proxies.
 * This module adds a real triangle-mesh path: build a BVH over scene
 * meshes, raytrace AO + sky-light contribution per lightmap texel.
 *
 * Multi-threaded via jce_jobs.  Embree integration is gated behind
 * the JCE_HAS_EMBREE compile flag (defined by `jce_lightmapper_embree.h`);
 * when off, the built-in median-split BVH does the work.
 *
 * Workflow
 *   1. Build a JceBakeMesh array — one entry per static mesh (verts +
 *      indices + UV2 lightmap channel).
 *   2. Call jce_lightmapper_bake_begin(scene, jobs, output_path).
 *   3. Poll jce_lightmapper_bake_poll() for progress 0..1.
 *   4. When done, the .lmap.png + .lmap.json sidecar are on disk.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHTMAPPER_BAKE_H
#define JCE_LIGHTMAPPER_BAKE_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_jobs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    const float    *positions;     /* xyz triples, vertex_count */
    const float    *normals;       /* xyz triples, vertex_count */
    const float    *uv2;           /* uv pairs, vertex_count (lightmap UVs) */
    uint32_t        vertex_count;
    const uint32_t *indices;       /* triangle_count * 3 */
    uint32_t        triangle_count;
    /* World transform — applied to positions/normals.  NULL = identity. */
    const float    *transform_4x4;
} JceBakeMesh;

typedef struct {
    /* Scene meshes (cast + receive shadows for each other). */
    const JceBakeMesh *meshes;
    uint32_t           mesh_count;

    /* Hemisphere sample count per texel (e.g. 64).  Higher = quieter
     * but slower; quality / time linear in this. */
    uint32_t           samples_per_texel;
    /* Maximum ray length in world units.  Beyond this, rays count as
     * "hit sky". */
    float              max_ray_length;
    /* Skylight colour multiplier for unoccluded ray directions. */
    float              sky_color[3];
    float              sky_intensity;

    /* Output lightmap resolution. */
    uint32_t           lightmap_width;
    uint32_t           lightmap_height;

    /* Output paths (PNG + .lmap.json sidecar). */
    const char        *output_png_path;
    const char        *output_json_path;
} JceLightmapBakeConfig;

typedef struct JceLightmapBaker JceLightmapBaker;

/* Begin a bake.  Returns NULL on bad config.  The baker dispatches
 * work onto the supplied job system (NULL = run single-threaded on
 * the calling thread). */
JCE_API JceLightmapBaker *jce_lightmapper_bake_begin(
    const JceLightmapBakeConfig *cfg,
    JceJobSystem                *jobs);

/* Progress in [0, 1].  Returns 1.0 when complete. */
JCE_API float jce_lightmapper_bake_poll(const JceLightmapBaker *b);

/* Returns true once the bake finished writing outputs.  Once true,
 * caller should jce_lightmapper_bake_free(). */
JCE_API bool  jce_lightmapper_bake_done(const JceLightmapBaker *b);

JCE_API void  jce_lightmapper_bake_free(JceLightmapBaker *b);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTMAPPER_BAKE_H */
