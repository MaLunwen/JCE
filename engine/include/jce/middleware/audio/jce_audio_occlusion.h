/*
 * jce_audio_occlusion.h -- generic audio occlusion / obstruction.
 *
 * Pure-CPU.  Given a listener and a set of source positions, computes
 * per-source occlusion factor (0 = clear path, 1 = fully occluded) and
 * a low-pass cutoff frequency the caller can apply to the corresponding
 * voice's filter bus.
 *
 * Engine-agnostic — physics is supplied via a raycast callback.  The
 * callback is called from the same thread as jce_audio_occlusion_solve
 * (typically the game thread).
 *
 * Thread-safety: stateless _solve() is reentrant — safe in parallel
 * over disjoint query arrays.  A JceAudioOcclusionTracker instance is
 * single-threaded (use one tracker per worker).
 *
 * Example:
 *   JceAudioOcclusionTracker *tr =
 *       jce_audio_occlusion_tracker_create(256);
 *   JceAudioOcclusionQuery q[N];
 *   for (uint32_t i = 0; i < N; ++i) q[i].source_position = sources[i];
 *   uint64_t ids[N]; for (uint32_t i = 0; i < N; ++i) ids[i] = voices[i];
 *   jce_audio_occlusion_tracker_solve(tr, listener_pos, ids, q, N,
 *                                     my_physics_raycast, physics_world);
 *   for (uint32_t i = 0; i < N; ++i) {
 *       jce_audio_voice_set_volume(audio, voices[i],
 *                                  base_vol[i] * q[i].attenuation);
 *       set_voice_lowpass(voices[i], q[i].lowpass_hz);
 *   }
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_AUDIO_OCCLUSION_H
#define JCE_AUDIO_OCCLUSION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-supplied raycast.  Returns the fraction of the segment
 * (origin -> origin + dir * max_distance) at which the ray first hits
 * a blocker.  Returns 1.0f if the ray is unobstructed.  `material_db`
 * (0..1) is filled with the blocker's acoustic absorption: 0 = thin
 * curtain, 1 = thick concrete; if the implementation has no material
 * info, set to 0.5f.
 *
 * `ud` is the user pointer passed to jce_audio_occlusion_solve().
 */
typedef float (*JceAudioOcclusionRaycastFn)(void          *ud,
                                            jce_vec3       origin,
                                            jce_vec3       dir,
                                            float          max_distance,
                                            float         *out_material_absorption);

typedef struct {
    jce_vec3 source_position;
    /* Output: full occlusion factor in [0,1] = (1-hit) * absorption.   */
    float    occlusion;
    /* Output: low-pass cutoff Hz.  22050 = bypass, 200 = heavy.        */
    float    lowpass_hz;
    /* Output: attenuation multiplier for direct path in [0,1].         */
    float    attenuation;
} JceAudioOcclusionQuery;

typedef struct {
    /* When occlusion=1, lowpass cutoff drops to this floor (Hz). */
    float min_lowpass_hz;     /* default 400  */
    /* When occlusion=0, lowpass cutoff ceiling (Hz). */
    float max_lowpass_hz;     /* default 22050 */
    /* When occlusion=1, direct path drops to this volume in [0,1]. */
    float min_direct_volume;  /* default 0.15 */
    /* Smoothing coefficient applied to per-source state in [0,1].
     *   0 = no smoothing (raw raycast values),
     *   1 = never update (frozen).
     * Authored as the retention per update at 60 Hz.  The tracker rescales it
     * by the measured solve-to-solve interval, so the perceived fade takes the
     * same wall-clock time at any frame rate; it used to be applied once per
     * update, which made sources duck faster on faster machines. */
    float smoothing;          /* default 0.85 */
    /* Maximum raycast distance in metres. */
    float max_raycast_dist;   /* default 200 */
} JceAudioOcclusionParams;

JCE_API JceAudioOcclusionParams jce_audio_occlusion_default_params(void);

/* Rescale an authored per-update retention coefficient to the elapsed time of
 * one update, so the same wall-clock fade results at any frame rate.  Exposed
 * for tests: the property that matters (two half-steps equal one whole step)
 * cannot be observed through the tracker without controlling its clock. */
JCE_API float jce_audio_occlusion_retention_for_dt(float authored,
                                                   float dt_seconds);

/* Stateless one-shot solver.  No history smoothing.                    *
 * `out_queries[i].occlusion / lowpass_hz / attenuation` are written.   */
JCE_API void jce_audio_occlusion_solve(const JceAudioOcclusionParams *params,
                                       jce_vec3                       listener,
                                       JceAudioOcclusionQuery        *queries,
                                       uint32_t                       count,
                                       JceAudioOcclusionRaycastFn     raycast,
                                       void                          *raycast_ud);

/* Stateful tracker (per-source temporal smoothing).                    */

typedef struct JceAudioOcclusionTracker JceAudioOcclusionTracker;

JCE_API JceAudioOcclusionTracker *jce_audio_occlusion_tracker_create(uint32_t initial_capacity);
JCE_API void                      jce_audio_occlusion_tracker_destroy(JceAudioOcclusionTracker *t);
JCE_API void                      jce_audio_occlusion_tracker_set_params(JceAudioOcclusionTracker *t, const JceAudioOcclusionParams *p);

/* Solve with per-source temporal smoothing.  `voice_ids` is an opaque
 * stable identifier the caller uses to associate state across frames
 * (e.g. JceVoice value cast to uint64_t, or any unique per-source id). */
JCE_API void jce_audio_occlusion_tracker_solve(JceAudioOcclusionTracker  *t,
                                               jce_vec3                   listener,
                                               const uint64_t            *voice_ids,
                                               JceAudioOcclusionQuery    *queries,
                                               uint32_t                   count,
                                               JceAudioOcclusionRaycastFn raycast,
                                               void                      *raycast_ud);

/* Drop tracker entries for voices no longer alive.  Caller passes a
 * sorted-or-unsorted array of active voice_ids; everything else gets
 * evicted.  Cheap O(state_count + active_count) hash sweep. */
JCE_API void jce_audio_occlusion_tracker_gc(JceAudioOcclusionTracker *t,
                                            const uint64_t           *active_voice_ids,
                                            uint32_t                  active_count);

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_OCCLUSION_H */
