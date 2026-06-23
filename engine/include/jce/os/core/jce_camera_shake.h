/*
 * jce_camera_shake.h -- Trauma-based camera shake generator (gap 6.5).
 *
 * A self-contained, deterministic shake model (the "math for game programmers"
 * trauma approach): callers add `trauma` on events (a hit, an explosion);
 * trauma decays over time, and the per-frame shake amount is trauma² so it
 * eases out smoothly.  The generator produces a bounded positional + rotational
 * offset (seeded smooth noise) that the camera system composes onto the view.
 *
 * Pure math — no rendering, no camera type dependency (the caller applies the
 * returned offset to whatever camera it has), so this is fully unit-testable.
 *
 * Layer: OS / Core (Layer 1) — dependency-free utility.
 */

#ifndef JCE_CAMERA_SHAKE_H
#define JCE_CAMERA_SHAKE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    float    trauma;         /* current trauma in [0,1]                     */
    float    decay_per_sec;  /* trauma lost per second                      */
    float    frequency;      /* shake oscillation frequency (cycles/sec-ish)*/
    uint32_t seed;           /* per-instance noise seed (distinct shakes)   */
    float    time;           /* accumulated time, advanced by _update       */
    float    max_pos[3];     /* positional offset at full shake (world units)*/
    float    max_rot_deg[3]; /* rotational offset at full shake (Euler deg) */
} JceCameraShake;

/* Initialise with a decay rate, oscillation frequency, and noise seed.  Default
 * amplitude is a mild (0.3 unit / 5°) shake; override with set_amplitude. */
JCE_API void jce_camera_shake_init(JceCameraShake *s, float decay_per_sec,
                                   float frequency, uint32_t seed);

/* Set the max positional (world units) and rotational (Euler degrees) offsets
 * reached at full shake.  NULL leaves that channel unchanged. */
JCE_API void jce_camera_shake_set_amplitude(JceCameraShake *s,
                                            const float max_pos[3],
                                            const float max_rot_deg[3]);

/* Add trauma (clamped so the total stays within [0,1]).  Call on impact. */
JCE_API void jce_camera_shake_add_trauma(JceCameraShake *s, float amount);

/* Advance time by dt and decay trauma toward zero. */
JCE_API void jce_camera_shake_update(JceCameraShake *s, float dt);

/* Current shake factor = trauma² (the eased intensity in [0,1]). */
JCE_API float jce_camera_shake_amount(const JceCameraShake *s);

/* True while there is trauma left to shake. */
JCE_API bool jce_camera_shake_active(const JceCameraShake *s);

/* Compute the current offset.  Each written component is bounded by the
 * matching max amplitude (|out| <= max), and is exactly zero when trauma is 0.
 * Either output pointer may be NULL. */
JCE_API void jce_camera_shake_offset(const JceCameraShake *s,
                                     float out_pos[3], float out_euler_deg[3]);

JCE_EXTERN_C_END

#endif /* JCE_CAMERA_SHAKE_H */
