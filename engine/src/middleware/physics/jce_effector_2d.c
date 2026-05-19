/*
 * jce_effector_2d.c  Force samplers for Unity 2D effectors.
 *
 * Each sampler is additive — caller zero-inits `out_force` and runs
 * every effector in the body's overlap list against it.
 *
 * Platform pass-through: collider normal is compared to world up;
 * inside the surface arc → collide, outside → pass.
 */

#include <jce/middleware/physics/jce_effector_2d.h>

#include <math.h>

static float falloff_factor(JceEffector2DFalloff f, float d, float scale)
{
    float denom = d + (scale > 0.0f ? scale : 0.001f);
    switch (f) {
    case JCE_EFFECTOR_FORCE_INV_LINEAR: return 1.0f / denom;
    case JCE_EFFECTOR_FORCE_INV_SQUARE: return 1.0f / (denom * denom);
    case JCE_EFFECTOR_FORCE_CONSTANT:
    default:                            return 1.0f;
    }
}

void jce_effector2d_apply_point(const JcePointEffector2DComponent *e,
                                  const float effector_pos[2],
                                  const float body_pos[2],
                                  const float body_vel[2],
                                  float       out_force[2])
{
    if (!e || !effector_pos || !body_pos || !out_force) return;
    float dx = body_pos[0] - effector_pos[0];
    float dy = body_pos[1] - effector_pos[1];
    float d  = sqrtf(dx*dx + dy*dy);
    if (d < 1e-5f) return;
    float k = falloff_factor(e->falloff, d, e->distance_scale)
              * e->force_magnitude;
    if (e->attract) k = -k;
    out_force[0] += (dx / d) * k;
    out_force[1] += (dy / d) * k;
    /* Drag — direct opposes velocity. */
    if (body_vel && e->drag > 0.0f) {
        out_force[0] -= body_vel[0] * e->drag;
        out_force[1] -= body_vel[1] * e->drag;
    }
}

void jce_effector2d_apply_area(const JceAreaEffector2DComponent *e,
                                 const float body_vel[2],
                                 float       out_force[2])
{
    if (!e || !out_force) return;
    float rad = e->force_angle_deg * 0.01745329f;
    float dirx = cosf(rad), diry = sinf(rad);
    out_force[0] += dirx * e->force_magnitude;
    out_force[1] += diry * e->force_magnitude;
    if (body_vel && e->drag > 0.0f) {
        out_force[0] -= body_vel[0] * e->drag;
        out_force[1] -= body_vel[1] * e->drag;
    }
}

void jce_effector2d_apply_buoyancy(const JceBuoyancyEffector2DComponent *e,
                                     const float body_pos[2],
                                     float       body_volume,
                                     float       gravity_y,
                                     const float body_vel[2],
                                     float       out_force[2])
{
    if (!e || !body_pos || !out_force) return;
    if (body_pos[1] >= e->surface_level_y) return; /* above fluid */
    /* Submerged fraction: assume body_volume is fully submerged.
     * Real depth-graded force needs the body's shape; here we apply
     * Archimedes assuming full submersion. */
    float force_y = -gravity_y * e->density * body_volume;
    out_force[1] += force_y;
    if (body_vel) {
        out_force[0] -= body_vel[0] * e->linear_drag;
        out_force[1] -= body_vel[1] * e->linear_drag;
    }
    /* Flow current. */
    float rad = e->flow_angle_deg * 0.01745329f;
    out_force[0] += cosf(rad) * e->flow_magnitude;
    out_force[1] += sinf(rad) * e->flow_magnitude;
}

bool jce_effector2d_platform_should_collide(
    const JcePlatformEffector2DComponent *e,
    const float contact_normal[2])
{
    if (!e || !contact_normal) return true;
    if (!e->use_one_way) return true;
    /* Normalise. */
    float L = sqrtf(contact_normal[0]*contact_normal[0] +
                     contact_normal[1]*contact_normal[1]);
    if (L < 1e-6f) return true;
    float ny = contact_normal[1] / L;
    /* Angle vs world up (0,1). */
    float angle_rad = acosf(ny);
    float arc_half  = (e->surface_arc_deg * 0.5f) * 0.01745329f;
    return angle_rad <= arc_half;
}
