/*
 * jce_vcam_confiner.h  Bounds clamp for a virtual camera position.
 *
 * Unity Cinemachine Confiner3D analog (data layer).  A confiner is a
 * world-space AABB (or oriented box) that the live camera position
 * is clamped into.  Optionally a soft-zone shrinks the AABB by
 * `damping_radius` to give the camera a smooth pull-back as it
 * approaches the boundary.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VCAM_CONFINER_H
#define JCE_VCAM_CONFINER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    /* World-space AABB. */
    float min[3];
    float max[3];
    /* Soft-zone shrink, in world units.  Camera within `damping_radius`
     * of an edge is pulled away with quadratic falloff. */
    float damping_radius;
    /* Whether to clamp Y axis too (false = ground-locked level). */
    bool  confine_y;
    bool  active;
} JceVcamConfiner3D;

/* Clamp `pos` (xyz) into the confiner.  Writes the corrected position
 * back to `pos`.  Returns true when the position was modified. */
JCE_API bool jce_vcam_confiner3d_clamp(const JceVcamConfiner3D *c,
                                         float pos[3]);

/* Convenience: build a confiner from centre + half-extents. */
JCE_API void jce_vcam_confiner3d_from_center(JceVcamConfiner3D *out,
                                              const float center[3],
                                              const float half_extents[3],
                                              float damping_radius);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_CONFINER_H */
