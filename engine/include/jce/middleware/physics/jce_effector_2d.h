/*
 * jce_effector_2d.h  Unity 2D physics effectors.
 *
 * Each effector applies a continuous force on bodies whose 2D
 * collider overlaps its trigger volume.  Four kinds match Unity:
 *   - PointEffector2D     : radial push/pull from a centre point
 *   - AreaEffector2D      : constant directional force inside volume
 *   - BuoyancyEffector2D  : Archimedes-style upward force below
 *                          surface line, plus linear/angular drag
 *   - PlatformEffector2D  : one-way platform (passes from below);
 *                          edge angle tolerance for normal filtering
 *
 * Components are POD attached to entities; the physics middleware
 * walks them each step and calls `jce_effector_2d_apply_to_body`
 * for every body inside.
 *
 * Layer: physics (Layer 4) — public.
 */

#ifndef JCE_EFFECTOR_2D_H
#define JCE_EFFECTOR_2D_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_EFFECTOR_FORCE_CONSTANT     = 0,
    JCE_EFFECTOR_FORCE_INV_LINEAR   = 1,   /* 1/d falloff */
    JCE_EFFECTOR_FORCE_INV_SQUARE   = 2,   /* 1/d² falloff */
} JceEffector2DFalloff;

typedef struct {
    float                force_magnitude;
    float                distance_scale;     /* clamp denominator */
    JceEffector2DFalloff falloff;
    /* Drag applied to bodies inside the effector. */
    float                drag;
    float                angular_drag;
    /* When true, force is attractive (toward centre); else repulsive. */
    bool                 attract;
} JcePointEffector2DComponent;

typedef struct {
    float force_angle_deg;    /* direction in world XY plane */
    float force_magnitude;
    float force_variation;    /* ±range randomness */
    float drag;
    float angular_drag;
    /* When true, force is applied at body centre (no torque). */
    bool  use_global_angle;
} JceAreaEffector2DComponent;

typedef struct {
    float surface_level_y;    /* world Y of fluid surface */
    float density;            /* fluid density, kg/m^3 */
    float linear_drag;
    float angular_drag;
    float flow_angle_deg;     /* current flow direction */
    float flow_magnitude;
    float flow_variation;
} JceBuoyancyEffector2DComponent;

typedef struct {
    /* Surface arc — top half-angle from up vector that counts as
     * "land on top" (0..180 degrees).  Outside this arc, the
     * collider is ignored for one-way pass-through. */
    float surface_arc_deg;
    bool  use_one_way;        /* false = solid both sides */
    bool  use_side_friction;
    bool  use_side_bounce;
    /* Optional: angle tolerance for stepped pass-through. */
    float side_arc_deg;
} JcePlatformEffector2DComponent;

/* ── Sampler ─────────────────────────────────────────────────── */

/* Apply a point effector to a body at `body_pos` (XY) with current
 * linear velocity `body_vel`.  Writes the resulting force into
 * `out_force` (additive — caller initialises to zero). */
JCE_API void jce_effector2d_apply_point(const JcePointEffector2DComponent *e,
                                          const float effector_pos[2],
                                          const float body_pos[2],
                                          const float body_vel[2],
                                          float       out_force[2]);

JCE_API void jce_effector2d_apply_area(const JceAreaEffector2DComponent *e,
                                         const float body_vel[2],
                                         float       out_force[2]);

JCE_API void jce_effector2d_apply_buoyancy(const JceBuoyancyEffector2DComponent *e,
                                             const float body_pos[2],
                                             float       body_volume,
                                             float       gravity_y,
                                             const float body_vel[2],
                                             float       out_force[2]);

/* Returns true when a contact at `contact_normal` (unit vec2)
 * should be honoured given the platform's surface arc; false means
 * pass-through. */
JCE_API bool jce_effector2d_platform_should_collide(
    const JcePlatformEffector2DComponent *e,
    const float contact_normal[2]);

JCE_EXTERN_C_END

#endif /* JCE_EFFECTOR_2D_H */
