/*
 * jce_navmesh_scene_bake.h  Build a Recast navmesh directly from a JceScene.
 *
 * Walks the scene's collider components (Box / Sphere / Capsule —
 * MeshCollider is left to a future pass), tessellates each into a
 * triangle soup in world space, and feeds the result into
 * jce_recast_build().  This is the runtime equivalent of clicking
 * "Bake" in Unity's Navigation window.
 *
 * Off-mesh links can be specified via JceOffMeshLink entries to
 * connect disjoint islands (jumps, ladders, teleports).
 *
 * Layer: AI / Navigation (Layer 3) — public.
 */

#ifndef JCE_NAVMESH_SCENE_BAKE_H
#define JCE_NAVMESH_SCENE_BAKE_H

#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene JceScene;

/* Off-mesh link description.  Both endpoints are world-space; a
 * one-directional link (start → end) is used unless `bidirectional`
 * is true. */
typedef struct {
    float    start[3];
    float    end[3];
    float    radius;       /* agent radius this link can carry (m) */
    bool     bidirectional;
    uint8_t  area_id;      /* 0 = default; ≥1 user-tagged areas    */
} JceOffMeshLink;

typedef struct {
    JceRecastConfig recast;          /* Recast voxelisation params */
    uint16_t        collision_mask;  /* which collider groups to include (0xFFFF = all) */

    /* Optional off-mesh links applied after the main bake.  Caller-owned. */
    const JceOffMeshLink *links;
    uint32_t              link_count;

    /* Tessellation density for spheres / capsules (latitude steps).
     * Default = 12 if zero. */
    uint32_t        sphere_segments;
} JceNavmeshSceneBakeDesc;

/* Build a navmesh from a scene.  Returns NULL on failure (logged).
 * Caller owns the returned object — destroy with jce_recast_destroy(). */
JCE_API JceRecastNavMesh *jce_navmesh_bake_scene(JceScene *scene,
                                                  const JceNavmeshSceneBakeDesc *desc);

JCE_EXTERN_C_END

#endif /* JCE_NAVMESH_SCENE_BAKE_H */
