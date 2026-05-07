/*
 * jce_vcam_system.h  Cinemachine-style VCam ECS system.
 *
 * Walks all entities with a JceVirtualCameraComponent, picks the highest
 * priority active one, resolves follow / look-at targets via Transform,
 * and produces a damped JceVcamOutput each frame.
 *
 * Rendering layer (Game View) checks `out_has_active` and overrides the
 * live camera position / target / FOV when true. When no VCam is active,
 * the live camera retains free-fly / player-snap behaviour.
 */

#ifndef JCE_VCAM_SYSTEM_H
#define JCE_VCAM_SYSTEM_H

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/scene/jce_virtual_camera.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene JceScene;

/* Reset internal damping state. Call when scene is unloaded or when the
 * active VCam should snap to its target on the next evaluate. */
JCE_API void JCE_CALL jce_vcam_system_reset(void);

/* Evaluate one frame.
 *  - Returns true via *out_has_active if any active VCam exists in the
 *    scene; in that case `out` is populated with the damped pose.
 *  - Returns false otherwise; `out` is left untouched.
 */
JCE_API void JCE_CALL jce_vcam_system_evaluate(JceScene      *scene,
                                                float          dt,
                                                JceVcamOutput *out,
                                                bool          *out_has_active);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_SYSTEM_H */
