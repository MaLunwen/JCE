/*
 * jce_collider_cook.h  Build per-object compound colliders from model
 *                      geometry.
 *
 * The core idea: a model file that contains several separated objects
 * must NOT collapse into one fat box / hull that fills the empty space
 * between them.  Instead each object (node / mesh) becomes its own child
 * collider and the children are combined into a single compound.
 *
 *   ruins.glb { wall_left, wall_right, pillar, stone }
 *      → 4 child colliders → 1 compound (holes between them stay empty)
 *
 * Static geometry cooks to exact triangle meshes; dynamic geometry to
 * convex hulls or, for concave parts, a VHACD convex decomposition.
 *
 * This is a build-time / load-time helper.  The cooked result is a small
 * POD tree that can be serialized (see jce_archive cook / save) and then
 * instantiated into a physics body via jce_collider_instantiate().
 */

#ifndef JCE_COLLIDER_COOK_H
#define JCE_COLLIDER_COOK_H

#include <stdbool.h>
#include <stdint.h>

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/physics/jce_physics_types.h>

JCE_EXTERN_C_BEGIN

/* ------------------------------------------------------------------ */
/* Cook configuration                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    JCE_COLLIDER_MODE_AUTO          = 0, /* static→trimesh, dynamic→convex hull */
    JCE_COLLIDER_MODE_BOX           = 1, /* tight AABB per part                 */
    JCE_COLLIDER_MODE_SPHERE        = 2, /* bounding sphere per part            */
    JCE_COLLIDER_MODE_CAPSULE       = 3, /* AABB-fit capsule per part           */
    JCE_COLLIDER_MODE_CONVEX_HULL   = 4, /* one convex hull per part            */
    JCE_COLLIDER_MODE_CONVEX_DECOMP = 5, /* VHACD: many hulls per concave part  */
    JCE_COLLIDER_MODE_TRIANGLE_MESH = 6  /* exact triangles (static only)       */
} JceColliderMode;

typedef enum {
    JCE_COLLIDER_SPLIT_BY_PART = 0, /* one child per input part (default)      */
    JCE_COLLIDER_SPLIT_WHOLE   = 1  /* merge all parts, then cook one unit     */
} JceColliderSplitMode;

typedef struct {
    JceColliderMode      mode;
    JceColliderSplitMode split;
    bool                 is_static;     /* affects AUTO; trimesh needs static */
    bool                 detect_naming; /* honor COL_/UCX_/UBX_/USP_/UCP_/TRI_ */

    /* VHACD tuning (convex decomposition).  0 ⇒ library defaults. */
    uint32_t vhacd_resolution;          /* voxel resolution (e.g. 100000)     */
    uint32_t vhacd_max_hulls;           /* max hulls per part (e.g. 32)       */
    uint32_t vhacd_max_verts_per_hull;  /* hull vertex cap (e.g. 64)          */
} JceColliderCookConfig;

/* Sensible defaults: AUTO mode, per-part split, static, naming on. */
JCE_API JceColliderCookConfig jce_collider_cook_config_default(void);

/* ------------------------------------------------------------------ */
/* Cook input                                                          */
/* ------------------------------------------------------------------ */

/*
 * One input object/part from a model.  Geometry is in part-LOCAL space;
 * `transform` (column-major 4x4) places it into model space.  The cooker
 * bakes the transform into the output so a model with arbitrary node
 * transforms still produces correctly-placed children.
 */
typedef struct {
    const char     *name;          /* node / mesh name (naming conventions)  */
    const float    *vertices;      /* xyz triplets, part-local               */
    uint32_t        vertex_count;
    const uint32_t *indices;       /* 3 per triangle                         */
    uint32_t        index_count;
    float           transform[16]; /* part-local → model space (column-major)*/
} JceColliderPart;

/* ------------------------------------------------------------------ */
/* Cooked output                                                       */
/* ------------------------------------------------------------------ */

/*
 * A cooked child collider.  Primitive children (BOX/SPHERE/CAPSULE) use
 * position/rotation/half_extents.  Geometry children (CONVEX_HULL /
 * TRIANGLE_MESH) have their model-space geometry baked into vertices /
 * indices and use an identity position/rotation.
 */
typedef struct {
    uint8_t   shape;          /* JceShapeType */
    float     position[3];    /* child local origin (model space)            */
    float     rotation[4];    /* child local rotation quat (x,y,z,w)         */
    float     half_extents[3];/* box/sphere/capsule params                   */
    float    *vertices;       /* owned; xyz triplets (hull / trimesh)        */
    uint32_t  vertex_count;
    uint32_t *indices;        /* owned; trimesh only                         */
    uint32_t  index_count;
} JceCookedChild;

typedef struct {
    JceCookedChild *children;
    uint32_t        child_count;
} JceCookedCollider;

/* Cook `part_count` parts into `out` according to `cfg`.  Returns false
   on bad input or if no usable geometry was found.  Free with
   jce_collider_cooked_free(). */
JCE_API bool jce_collider_cook(const JceColliderPart       *parts,
                               uint32_t                     part_count,
                               const JceColliderCookConfig *cfg,
                               JceCookedCollider           *out);

JCE_API void jce_collider_cooked_free(JceCookedCollider *c);

JCE_EXTERN_C_END

#endif /* JCE_COLLIDER_COOK_H */
