/*
 * jce_decal_projector.h  Persistent decal projector tied to an
 * entity's transform.
 *
 * Unity URP Decal Projector equivalent.  An ECS-attached component
 * spawns a decal that follows the entity's world transform and
 * lives until removed (vs. jce_decals' one-shot spawn API).
 *
 * Component payload tracks the underlying decal handle so the
 * scene system can update / despawn on transform change /
 * entity destruction.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_DECAL_PROJECTOR_H
#define JCE_DECAL_PROJECTOR_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_DECAL_PROJECTOR_PATH_LEN 160

typedef struct {
    char     sprite_path[JCE_DECAL_PROJECTOR_PATH_LEN];
    /* Decal box dimensions in world units (XY size + projection
     * depth Z).  Decal is projected -Z onto geometry. */
    float    size[3];
    /* Render layer mask (bitmask). */
    uint32_t target_layer_mask;
    /* Fade distance from camera. */
    float    fade_dist_start;
    float    fade_dist_end;
    /* Color tint applied. */
    float    color[4];
    /* Auto-orient flag: project onto surface normal instead of
     * fixed -Z. */
    bool     auto_orient;
    /* Runtime decal handle (filled by scene system; 0 = unspawned). */
    uint32_t runtime_handle;
} JceDecalProjectorComponent;

/* Initialise default values. */
JCE_API void jce_decal_projector_init(JceDecalProjectorComponent *p);

JCE_EXTERN_C_END

#endif /* JCE_DECAL_PROJECTOR_H */
