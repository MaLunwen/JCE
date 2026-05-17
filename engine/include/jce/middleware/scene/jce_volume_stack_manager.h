/*
 * jce_volume_stack_manager.h  Spatial blend of multiple PostFX
 * stacks.
 *
 * Unity HDRP/URP Volume framework analog.  Each volume is a registered
 * PostFX stack (`jce_postfx_stack.h`) tagged with a centre + radius +
 * priority.  When the camera enters a volume, its stack is blended
 * over the global default with distance-falloff weight.  Multiple
 * overlapping volumes blend by priority then radius.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VOLUME_STACK_MANAGER_H
#define JCE_VOLUME_STACK_MANAGER_H

#include <jce/middleware/scene/jce_postfx_stack.h>
#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_VOLUME_STACK_MAX 32

typedef enum {
    JCE_VOLUME_SHAPE_GLOBAL  = 0,
    JCE_VOLUME_SHAPE_SPHERE  = 1,
    JCE_VOLUME_SHAPE_BOX     = 2,
} JceVolumeShape;

typedef struct {
    JceVolumeShape          shape;
    float                   position[3];
    float                   radius;         /* sphere */
    float                   half_extents[3];/* box */
    float                   blend_distance; /* soft falloff zone */
    int32_t                 priority;       /* higher wins ties */
    JcePostFxStackComponent stack;
    bool                    active;
} JceVolumeStackEntry;

JCE_API void     jce_volume_stack_clear(void);
JCE_API uint32_t jce_volume_stack_register(const JceVolumeStackEntry *entry);
JCE_API bool     jce_volume_stack_remove(uint32_t idx);
JCE_API uint32_t jce_volume_stack_count(void);
JCE_API const JceVolumeStackEntry *jce_volume_stack_at(uint32_t idx);

/* Resolve the effective PostFX stack at `world_pos`.  Blends every
 * overlapping volume on top of `default_stack` with distance-
 * weighted priority.  Returns the count of contributing volumes. */
JCE_API uint32_t jce_volume_resolve_at(const float                    world_pos[3],
                                         const JcePostFxStackComponent *default_stack,
                                         JcePostFxStackComponent       *out_resolved);

JCE_EXTERN_C_END

#endif /* JCE_VOLUME_STACK_MANAGER_H */
