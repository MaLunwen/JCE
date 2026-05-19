/*
 * jce_vcam_dolly_group.h  DollyCart path follower + Group Composer.
 *
 * Two independent helpers sharing this TU:
 *
 *   - jce_vcam_dolly_apply: advance position_along_path by speed*dt,
 *     resample the camera position from the polyline waypoint list
 *     using piecewise-linear interpolation by accumulated arc length.
 *
 *   - jce_vcam_group_apply: compute weighted centroid of the group's
 *     targets + a bounding circle; outputs a look-at point (centroid)
 *     and optionally a recommended camera distance (when
 *     adjust_distance is true).  Caller decides how to apply the
 *     distance — typically pulls the camera back along its current
 *     view direction.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VCAM_DOLLY_GROUP_H
#define JCE_VCAM_DOLLY_GROUP_H

#include <jce/middleware/scene/jce_virtual_camera.h>

JCE_EXTERN_C_BEGIN

/* Advance + sample dolly cart.  When `auto_advance` is set, this
 * fn updates position_along_path internally; otherwise the caller
 * is expected to drive it (e.g. tied to a gameplay value).
 * Outputs the sampled world position. */
JCE_API void jce_vcam_dolly_apply(JceVcamDollyCart *dolly, float dt,
                                    float out_position[3]);

/* Group composer outputs the look-at centroid + an optional
 * recommended camera distance.  When `out_distance` is NULL or
 * `group.adjust_distance` is false, distance is ignored. */
JCE_API void jce_vcam_group_apply(const JceVcamGroupComposer *group,
                                    float vertical_fov_deg,
                                    float aspect,
                                    float out_lookat[3],
                                    float *out_distance);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_DOLLY_GROUP_H */
