/*
 * jce_2d_lights.h  Unity URP 2D Renderer light equivalents.
 *
 * Three light kinds:
 *   - PointLight2D (radius, color, intensity, layer mask)
 *   - SpotLight2D  (radius + inner/outer cone angle)
 *   - GlobalLight2D (covers a sorting layer with constant colour)
 *
 * Components are POD; the renderer (eventual jce_sprite_batch wire)
 * culls + gathers them per draw via the gather helper below.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_2D_LIGHTS_H
#define JCE_2D_LIGHTS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_2D_LIGHTS_GATHER_MAX 64

typedef enum {
    JCE_2D_LIGHT_POINT     = 0,
    JCE_2D_LIGHT_SPOT      = 1,
    JCE_2D_LIGHT_GLOBAL    = 2,
    JCE_2D_LIGHT_FREEFORM  = 3,    /* polygon shape (data only) */
} JceLight2DKind;

typedef enum {
    JCE_2D_LIGHT_BLEND_ADDITIVE = 0,
    JCE_2D_LIGHT_BLEND_MULTIPLY = 1,
} JceLight2DBlend;

/* Per-light snapshot the renderer consumes per sprite draw. */
typedef struct {
    JceLight2DKind  kind;
    float           position[3];    /* world XY + Z for sort */
    float           color[4];
    float           intensity;      /* multiplier on color */
    float           outer_radius;
    float           inner_radius;   /* falloff begins here */
    float           inner_angle_deg; /* spot only */
    float           outer_angle_deg; /* spot only */
    uint32_t        target_layer_mask; /* sorting layers the light hits */
    JceLight2DBlend blend;
    bool            volumetric;     /* whether to emit a volumetric quad */
} JceLight2DInstance;

/* Component PODs (JcePointLight2DComponent / JceSpotLight2DComponent
 * / JceGlobalLight2DComponent) live in jce_scene.h for scene-graph
 * ownership; callers translate them into JceLight2DInstance entries
 * each frame and push to the gather pool below. */

/* AABB in world XY used for gather culling. */
typedef struct {
    float min_x, min_y;
    float max_x, max_y;
} JceLight2DAabb;

/* Process-global gather buffer.  Populated by callers (typically by
 * walking the scene's light entities + filling instances).  The
 * renderer then reads jce_2d_lights_gather to iterate visible ones
 * within a view rect. */
JCE_API void     jce_2d_lights_clear(void);
JCE_API bool     jce_2d_lights_push(const JceLight2DInstance *inst);
JCE_API uint32_t jce_2d_lights_count(void);
JCE_API const JceLight2DInstance *jce_2d_lights_at(uint32_t idx);

/* Filter by view AABB + layer mask; fills `out` with up to `cap`
 * matching instances and returns the count.  Sort order: outer_radius
 * descending (bigger lights first). */
JCE_API uint32_t jce_2d_lights_gather(JceLight2DAabb           view,
                                       uint32_t                  layer_mask,
                                       JceLight2DInstance      *out,
                                       uint32_t                  cap);

JCE_EXTERN_C_END

#endif /* JCE_2D_LIGHTS_H */
