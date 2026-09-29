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
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene JceScene;
typedef uint64_t JceEntity;

/* Reset internal damping state. Call when scene is unloaded or when the
 * active VCam should snap to its target on the next evaluate. Also re-seeds
 * the trauma-shake generator to zero, so a fresh scene/Play session starts
 * perfectly still. */
JCE_API void JCE_CALL jce_vcam_system_reset(void);

/* Add trauma to the camera-shake generator (gap 6.5).  `amount` is added to
 * the current trauma and clamped to [0,1]; the shake intensity is trauma² so
 * it eases out, and it decays back to zero over time.  While trauma is active,
 * jce_vcam_system_evaluate adds a bounded positional offset (max amplitude is
 * the generator's configured envelope) onto the resolved active-VCam pose, so
 * the live camera visibly shakes on a hit / explosion and settles back.  No-op
 * when there is no active VCam to apply it to. */
JCE_API void JCE_CALL jce_vcam_system_add_trauma(float amount);

/* Read the CURRENT-frame camera-shake displacement (gap 6.5) WITHOUT needing an
 * active VCam.  Writes the bounded positional offset (decaying trauma -> noise *
 * trauma^2) into out_pos[3]; exactly zero when there is no trauma.  READ-ONLY —
 * does NOT advance the shake clock (jce_vcam_system_evaluate advances it once
 * per frame), so a player-snap / free-fly camera can add this offset and shake
 * even when no VirtualCamera is active.  Returns true if the shake is active. */
JCE_API bool JCE_CALL jce_vcam_system_get_shake_offset(float out_pos[3]);

/* Evaluate one frame.
 *  - Returns true via *out_has_active if any active VCam exists in the
 *    scene; in that case `out` is populated with the damped pose.
 *  - Returns false otherwise; `out` is left untouched.
 */
JCE_API void JCE_CALL jce_vcam_system_evaluate(JceScene      *scene,
                                                float          dt,
                                                JceVcamOutput *out,
                                                bool          *out_has_active);

/*
 * ── Naming a shot ──────────────────────────────────────────────────────
 *
 * JceVirtualCameraComponent.vcam_name was authored, serialised and shown in
 * the VCam Manager, and NOTHING under engine/src ever looked at it: selection
 * is by priority, so the name was a label in a panel.  "Cut to the camera
 * called BossIntro" -- the one thing a cutscene, a trigger or a scene authored
 * from the SDK actually wants to say -- could not be expressed at all.
 */

/* The entity carrying the virtual camera named `name`, or 0 when there is
 * none.  Matching is exact and case-sensitive, like every other name lookup
 * in this scene API. */
JCE_API JceEntity JCE_CALL jce_vcam_find_by_name(JceScene *scene,
                                                 const char *name);

/*
 * Make the named vcam the live one, ahead of priority.
 *
 * IT DOES NOT TOUCH THE AUTHORED COMPONENTS, and that is the whole design.
 * The obvious implementation -- raise this one's `priority` above the others
 * -- writes gameplay state into the authored scene: the editor would mark it
 * dirty, Ctrl+S would bake a cutscene's camera choice into the level, and
 * ending the cutscene would need the old numbers remembered from somewhere.
 * So the override lives in the system, beside the damping state that is
 * already there, and the scene file is what the author wrote.
 *
 * A named camera that does not exist, is inactive, or has a disabled
 * component does NOT win: selection falls back to priority, which is what the
 * scene would have done anyway.  Returns true when the name currently
 * resolves to such a camera -- so a caller can tell "cut taken" from "no such
 * shot" -- but the override is recorded either way, because an entity that
 * has not streamed in yet must not silently turn into "whatever priority
 * says" the moment it appears.
 *
 * Pass NULL or "" to clear it and hand the decision back to priority.
 */
JCE_API bool JCE_CALL jce_vcam_system_set_active_by_name(JceScene   *scene,
                                                         const char *name);

/* The name currently overriding priority, or "" when none.  Never NULL. */
JCE_API const char *JCE_CALL jce_vcam_system_get_active_name(void);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_SYSTEM_H */
