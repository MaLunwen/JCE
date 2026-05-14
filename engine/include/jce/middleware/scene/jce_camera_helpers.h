/*
 * jce_camera_helpers.h  Cinemachine-style camera math.
 *
 * Pure-function helpers for the common scripted-camera patterns:
 * look-at-with-offset (orbit characters at a fixed lens offset),
 * orbit (yaw/pitch rotation around a target), and dolly-along-path
 * (interpolated camera positions driven by an AnimationCurve t-param).
 *
 * Helpers compute world-space (eye, target, up) tuples that the
 * caller composes into a final view matrix via the engine's existing
 * `jce_m4_look_at`.  Decoupled from any specific camera component so
 * editor previews, gameplay code, and replay tools can all use the
 * same primitives.
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_CAMERA_HELPERS_H
#define JCE_CAMERA_HELPERS_H

#include <jce/middleware/animation/jce_anim_curve.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    jce_vec3 eye;
    jce_vec3 target;
    jce_vec3 up;
} JceCameraFrame;

/* Look at `target` from `eye + offset_local`.  `offset_local` is in
 * the camera's local space (right, up, forward) — applied AFTER the
 * look-at orientation is computed from (eye, target, world_up).  Use
 * this for fixed-shoulder follow cams. */
JCE_API JceCameraFrame jce_cam_look_at_with_offset(jce_vec3 eye,
                                                    jce_vec3 target,
                                                    jce_vec3 world_up,
                                                    jce_vec3 offset_local);

/* Orbit a target at the given distance with yaw / pitch (radians).
 * Yaw rotates around world_up, pitch tilts above/below.  Useful for
 * arc-rotate inspection cams.  Output `up` is world_up. */
JCE_API JceCameraFrame jce_cam_orbit(jce_vec3 target,
                                      float     yaw_rad,
                                      float     pitch_rad,
                                      float     distance,
                                      jce_vec3  world_up);

/* Sample three position curves at parameter `t` to derive an eye
 * position; the caller supplies its own target via `look_target` and
 * world up.  Useful for dolly tracks where the curves were authored
 * in the Animation Curve editor (B7.3 + B9.1).  Any of the three
 * curves may be NULL → its component is treated as 0. */
JCE_API JceCameraFrame jce_cam_dolly_along_path(const JceAnimCurve *cx,
                                                 const JceAnimCurve *cy,
                                                 const JceAnimCurve *cz,
                                                 float               t,
                                                 jce_vec3            look_target,
                                                 jce_vec3            world_up);

/* Smooth-damp interpolation toward a target frame.  `dt` is the
 * frame delta; `smooth_time` controls how aggressively the
 * interpolation snaps (smaller → snappier).  Returns the new frame
 * to apply this tick.  Mirrors Unity's Vector3.SmoothDamp for camera
 * follow without spawn jitter. */
JCE_API JceCameraFrame jce_cam_smooth_damp(JceCameraFrame current,
                                            JceCameraFrame target,
                                            float          smooth_time,
                                            float          dt);

JCE_EXTERN_C_END

#endif /* JCE_CAMERA_HELPERS_H */
