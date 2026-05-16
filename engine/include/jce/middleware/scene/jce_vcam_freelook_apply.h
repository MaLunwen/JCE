/*
 * jce_vcam_freelook_apply.h  Cinemachine FreeLook + Composer + Hard
 * Lookat evaluator (data-layer, no bgfx).
 *
 * Consumes a JceVirtualCamera whose `rig` is FREELOOK or
 * THIRD_PERSON_AIM and produces a target position + look-at point
 * for the live camera each frame.  Composer applies a screen-space
 * framing rule when lookat_mode == DAMPED; HARD short-circuits to
 * the raw look-at point.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VCAM_FREELOOK_APPLY_H
#define JCE_VCAM_FREELOOK_APPLY_H

#include <jce/middleware/scene/jce_virtual_camera.h>

JCE_EXTERN_C_BEGIN

/* Input deltas from the platform layer (typically mouse Δx / Δy +
 * a `dt`).  Speeds are baked into the rig itself. */
typedef struct {
    float yaw_input;        /* horizontal mouse delta or stick.x */
    float pitch_input;      /* vertical   mouse delta or stick.y */
    float dt;
} JceVcamFreeLookInput;

/* Output: position + look-at point + an effective FOV (composer may
 * widen it slightly to keep target in frame at the soft-zone edge). */
typedef struct {
    float position[3];
    float target[3];
    float fov_deg;
} JceVcamFreeLookOutput;

/* Advance the FreeLook rig + apply composer framing.  When the rig
 * type is BASIC, no-op (caller falls back to jce_vcam_evaluate).
 *
 * Mutates `vcam->freelook.horizontal_axis` / `vertical_axis` in
 * place so the next call sees accumulated state. */
JCE_API void jce_vcam_freelook_apply(JceVirtualCamera           *vcam,
                                       const JceVcamFreeLookInput *input,
                                       JceVcamFreeLookOutput      *out);

/* Composer-only helper: nudge `look_at` toward `target_world` so the
 * resulting view keeps the target inside the composer's screen
 * rect.  Returns the corrected look-at point.  `cam_pos` is the
 * current camera position; `cam_forward` is its current forward
 * vector (unit).  When composer.lookat_mode == HARD, copies
 * target_world straight through. */
JCE_API void jce_vcam_composer_apply(const JceVcamComposer *composer,
                                       const float cam_pos[3],
                                       const float cam_forward[3],
                                       const float target_world[3],
                                       float       out_lookat[3]);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_FREELOOK_APPLY_H */
