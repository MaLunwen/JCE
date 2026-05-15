/*
 * jce_sensor.h  AI sensor / perception system.
 *
 * Each sensor is a query-time predicate that turns world state +
 * an observer's transform into a list of "stimuli" — perceived
 * entities, sounds, or events.  Mirrors Unreal's AIPerception and
 * Unity GameplayAbilities's perception module.
 *
 * Sensor kinds
 *   - Vision cone  (forward cone + max range + line-of-sight check)
 *   - Hearing      (sphere around observer; sounds register via
 *                   jce_sensor_report_sound)
 *   - Memory       (decay of previously-seen stimuli with a half-life)
 *
 * Storage is per-observer JceSensor + a process-global sound event
 * queue.  No physics coupling — line-of-sight is a caller-supplied
 * callback so consumers can pick whether to run a Bullet raycast or
 * use a cheaper visibility heuristic.
 *
 * Layer: middleware/ai (Layer 4) — public.
 */

#ifndef JCE_SENSOR_H
#define JCE_SENSOR_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SENSOR_MEMORY_MAX 32
#define JCE_SENSOR_SOUND_QUEUE 64

typedef enum {
    JCE_SENSOR_STIMULUS_NONE   = 0,
    JCE_SENSOR_STIMULUS_VISUAL = 1,
    JCE_SENSOR_STIMULUS_SOUND  = 2,
} JceSensorStimulusKind;

typedef struct {
    JceSensorStimulusKind kind;
    uint64_t              source_entity;   /* who/what produced the stimulus */
    float                 position[3];     /* world-space last seen / heard */
    float                 strength;        /* 0..1 (loudness / brightness) */
    float                 time_seconds;    /* world time at observation */
    /* Memory decay accumulator (1 → fresh, 0 → forgotten). */
    float                 confidence;
} JceSensorStimulus;

typedef struct {
    /* Observer transform. */
    float forward[3];
    float position[3];
    float up[3];

    /* Vision parameters. */
    float vision_fov_deg;      /* full cone angle in degrees */
    float vision_range;
    /* Hearing parameters. */
    float hearing_range;
    /* Memory half-life in seconds — 0 disables memory decay. */
    float memory_half_life;

    JceSensorStimulus memory[JCE_SENSOR_MEMORY_MAX];
    uint32_t          memory_count;
} JceSensor;

/* Optional line-of-sight callback.  Return true if the observer can
 * see the target.  `user` is passed through from query call. */
typedef bool (*JceSensorLosFn)(const float observer[3],
                                const float target[3], void *user);

/* Lifecycle. */
JCE_API void jce_sensor_init(JceSensor *s);

/* Update memory decay using `dt` seconds elapsed.  Stimuli below
 * `prune_threshold` (default 0.05) are dropped. */
JCE_API void jce_sensor_update_memory(JceSensor *s, float dt);

/* Query a list of candidate targets (positions + entity ids).  For
 * each that's within the vision cone + range AND `los_fn` returns
 * true, push a JCE_SENSOR_STIMULUS_VISUAL into memory at full
 * confidence. */
JCE_API uint32_t jce_sensor_query_visual(JceSensor      *s,
                                           const uint64_t *candidate_entities,
                                           const float    *candidate_positions, /* xyz triples */
                                           uint32_t        candidate_count,
                                           float           current_time_s,
                                           JceSensorLosFn  los_fn,
                                           void           *los_user);

/* ── Process-global sound event queue ────────────────────────── */

/* Report a sound that propagates to any nearby sensor.  Sources
 * should call this when firing weapons, footsteps land, etc. */
JCE_API bool jce_sensor_report_sound(uint64_t emitter_entity,
                                       const float position[3],
                                       float       loudness,
                                       float       current_time_s);

/* Drain pending sound events into a sensor's memory.  Caller passes
 * `dt` so memory decay is applied to existing entries too. */
JCE_API uint32_t jce_sensor_pull_sounds(JceSensor *s);

JCE_API void     jce_sensor_clear_sounds(void);

JCE_EXTERN_C_END

#endif /* JCE_SENSOR_H */
