/*
 * jce_audio_ecs.h -- flecs ECS adapter for the audio middleware.
 *
 * Bridges JceReverbZones / JceAudioOcclusionTracker / JceAudioMixer
 * into a flecs world so callers can attach audio behaviour to
 * entities and drive everything from per-frame queries instead of
 * managing module instances by hand.
 *
 * The adapter does NOT own the underlying audio modules — the caller
 * creates them and passes them in.  Lifetime: the JceAudioEcs handle
 * must outlive any system tick using its components.
 *
 * Generic — works with any flecs world; no scene/middleware coupling.
 *
 * Thread-safety: a JceAudioEcs handle is single-threaded (the flecs
 * world it wraps is single-threaded by default).  Tick from the same
 * thread that drives ecs_progress().
 *
 * Example:
 *   ecs_world_t *w = (ecs_world_t *)jce_scene_get_world(scene);
 *   JceAudioEcs *aecs = jce_audio_ecs_create(w, reverb, occ, mixer);
 *
 *   // attach a reverb zone to an entity:
 *   JceReverbZoneEcs z;
 *   z.desc.shape          = JCE_REVERB_SHAPE_AABB;
 *   z.desc.center         = (jce_vec3){10, 1, 5};
 *   z.desc.extents        = (jce_vec3){4, 2, 4};
 *   z.desc.falloff_radius = 1.5f;
 *   z.desc.priority       = 1;
 *   z.desc.preset         = jce_reverb_preset_hall();
 *   z.zone_id             = 0;        // assigned by sync()
 *   ecs_set_ptr(w, e, JceReverbZoneEcs, &z);
 *
 *   // each frame:
 *   jce_audio_ecs_sync_zones(aecs);
 *   JceReverbPreset blended;
 *   jce_audio_ecs_sample_reverb(aecs, listener_pos, &blended);
 *   jce_audio_ecs_solve_occlusion(aecs, listener_pos, my_raycast, world);
 *   jce_audio_ecs_apply_mixer(aecs);
 *
 *   jce_audio_ecs_destroy(aecs);
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_AUDIO_ECS_H
#define JCE_AUDIO_ECS_H

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* The caller-owned ECS world is passed as an opaque void* (from
 * jce_scene_get_world()); the concrete flecs type is private to the ABI. */

/* ------------------------------------------------------------------ *
 *  Components
 * ------------------------------------------------------------------ */

/* Reverb zone attached to an entity.  zone_id is assigned by the
 * adapter on first sync; user fills `desc` only. */
typedef struct JceReverbZoneEcs {
    JceReverbZoneDesc desc;
    JceReverbZoneId   zone_id;   /* 0 = unassigned */
    bool              dirty;     /* set by user to push desc updates */
} JceReverbZoneEcs;

/* Voice that wants run-time occlusion.  source_position is read each
 * tick; lowpass_hz / attenuation are written back. */
typedef struct JceOcclusionSourceEcs {
    uint64_t  voice_id;
    jce_vec3  source_position;
    /* outputs (read by caller after solve_occlusion): */
    float     occlusion;
    float     lowpass_hz;
    float     attenuation;
} JceOcclusionSourceEcs;

/* Voice → bus assignment.  apply_mixer() pushes any (re)assignment
 * into JceAudioMixer; user mutates `bus` to retarget. */
typedef struct JceMixerBusEcs {
    uint64_t      voice_id;
    JceAudioBusId bus;
    JceAudioBusId last_applied_bus;  /* internal; user leaves alone */
} JceMixerBusEcs;

/* ------------------------------------------------------------------ *
 *  Lifetime
 * ------------------------------------------------------------------ */

typedef struct JceAudioEcs JceAudioEcs;

/* Register components in `world` and bind to caller-owned modules.
 * Any of `rz`/`occ`/`mx` may be NULL — the corresponding tick
 * function is then a no-op (graceful degradation). */
JCE_API JceAudioEcs *jce_audio_ecs_create(void                     *world,
                                          JceReverbZones           *rz,
                                          JceAudioOcclusionTracker *occ,
                                          JceAudioMixer            *mx);

JCE_API void JCE_CALL jce_audio_ecs_destroy(JceAudioEcs *a);

/* ------------------------------------------------------------------ *
 *  Per-frame ticks (call in any order; all three are independent)
 * ------------------------------------------------------------------ */

/* Mirror JceReverbZoneEcs components into the underlying
 * JceReverbZones store.  New components get a zone_id; ones with
 * dirty=true get their desc re-pushed. */
JCE_API void JCE_CALL jce_audio_ecs_sync_zones(JceAudioEcs *a);

/* Sample the active reverb mixture at `listener_pos`. */
JCE_API void JCE_CALL jce_audio_ecs_sample_reverb(JceAudioEcs *a,
                                                  jce_vec3 listener_pos,
                                                  JceReverbPreset *out);

/* Batch-solve occlusion for every JceOcclusionSourceEcs entity.
 * Writes occlusion / lowpass_hz / attenuation back into each. */
JCE_API void JCE_CALL jce_audio_ecs_solve_occlusion(JceAudioEcs *a,
                                                    jce_vec3 listener_pos,
                                                    JceAudioOcclusionRaycastFn raycast,
                                                    void *ud);

/* Push voice→bus assignments that changed since last tick. */
JCE_API void JCE_CALL jce_audio_ecs_apply_mixer(JceAudioEcs *a);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_ECS_H */
