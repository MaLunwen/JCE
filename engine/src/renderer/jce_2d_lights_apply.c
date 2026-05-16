/*
 * jce_2d_lights_apply.c  Scene → gather pool conversion stub.
 *
 * Iterating ECS components needs flecs; the heavy walking helper is
 * scene-internal and lives in jce_scene.c.  Here we expose a
 * fixed-shape stub that the eventual scene-side walker calls
 * once per visible 2D-light entity to translate the per-kind POD
 * into a JceLight2DInstance and push it.
 *
 * Until the scene walker lands in the bgfx host build, callers can
 * use jce_2d_lights_apply_push_point / _spot / _global directly
 * with whatever transform + component data they have.
 */

#include <jce/renderer/jce_2d_lights_apply.h>
#include <jce/middleware/scene/jce_scene.h>

#include <string.h>

static void push_with_position(JceLight2DInstance *inst,
                                 const float position[3])
{
    inst->position[0] = position[0];
    inst->position[1] = position[1];
    inst->position[2] = position[2];
}

void jce_2d_lights_apply_push_point(const float position[3],
                                      const JcePointLight2DComponent *p)
{
    if (!p) return;
    JceLight2DInstance inst;
    memset(&inst, 0, sizeof(inst));
    inst.kind = JCE_2D_LIGHT_POINT;
    push_with_position(&inst, position);
    memcpy(inst.color, p->color, sizeof(inst.color));
    inst.intensity         = p->intensity;
    inst.outer_radius      = p->outer_radius;
    inst.inner_radius      = p->inner_radius;
    inst.target_layer_mask = p->target_layer_mask;
    inst.blend             = (JceLight2DBlend)p->blend;
    inst.volumetric        = p->volumetric;
    jce_2d_lights_push(&inst);
}

void jce_2d_lights_apply_push_spot(const float position[3],
                                     const JceSpotLight2DComponent *p)
{
    if (!p) return;
    JceLight2DInstance inst;
    memset(&inst, 0, sizeof(inst));
    inst.kind = JCE_2D_LIGHT_SPOT;
    push_with_position(&inst, position);
    memcpy(inst.color, p->color, sizeof(inst.color));
    inst.intensity         = p->intensity;
    inst.outer_radius      = p->outer_radius;
    inst.inner_radius      = p->inner_radius;
    inst.inner_angle_deg   = p->inner_angle_deg;
    inst.outer_angle_deg   = p->outer_angle_deg;
    inst.target_layer_mask = p->target_layer_mask;
    inst.blend             = (JceLight2DBlend)p->blend;
    inst.volumetric        = p->volumetric;
    jce_2d_lights_push(&inst);
}

void jce_2d_lights_apply_push_global(const JceGlobalLight2DComponent *p)
{
    if (!p) return;
    JceLight2DInstance inst;
    memset(&inst, 0, sizeof(inst));
    inst.kind = JCE_2D_LIGHT_GLOBAL;
    /* Global = position irrelevant. */
    memcpy(inst.color, p->color, sizeof(inst.color));
    inst.intensity         = p->intensity;
    inst.target_layer_mask = p->target_layer_mask;
    inst.blend             = (JceLight2DBlend)p->blend;
    jce_2d_lights_push(&inst);
}

uint32_t jce_2d_lights_apply_from_scene(JceScene *scene)
{
    (void)scene;
    /* Stub: scene walker lives in host build (needs flecs).  Callers
     * use the per-light helpers above + their own iteration until
     * then. */
    return 0;
}
