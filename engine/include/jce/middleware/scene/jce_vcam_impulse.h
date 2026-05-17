/*
 * jce_vcam_impulse.h  Cinemachine Impulse source + listener.
 *
 * An "impulse source" is a one-shot kick (force vector + duration +
 * decay curve) the game emits when something interesting happens —
 * explosion, big landing, weapon recoil.  A "listener" is the
 * camera-side component that integrates every active impulse within
 * its `attention_radius` and outputs a position-offset + rotation-
 * shake to layer on top of the vcam's normal output.
 *
 * Process-global impulse queue; sources push, listeners pull each
 * frame and decay-update.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_VCAM_IMPULSE_H
#define JCE_VCAM_IMPULSE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_VCAM_IMPULSE_MAX 32

typedef enum {
    JCE_VCAM_IMPULSE_LINEAR   = 0, /* linear decay */
    JCE_VCAM_IMPULSE_EXPO     = 1, /* exponential */
    JCE_VCAM_IMPULSE_BUMP     = 2, /* sin^2 (rise then fall) */
} JceVcamImpulseDecay;

typedef struct {
    float               source_pos[3];
    float               impulse_force[3];  /* direction + magnitude */
    float               rotation_axis[3];  /* shake axis (unit) */
    float               rotation_magnitude;
    float               attack_time;       /* seconds before peak */
    float               sustain_time;
    float               release_time;
    JceVcamImpulseDecay decay;
    float               attenuation_radius; /* full strength inside */
    float               age;               /* seconds since spawn */
    bool                active;
} JceVcamImpulse;

/* Output integrated by the listener each frame. */
typedef struct {
    float position_offset[3];
    float rotation_axis[3];
    float rotation_angle_rad;
} JceVcamImpulseFrame;

/* Lifecycle. */
JCE_API void jce_vcam_impulse_clear(void);
JCE_API bool jce_vcam_impulse_spawn(const JceVcamImpulse *impulse);

/* Listener: integrate every active impulse at `listener_pos`,
 * advancing internal age by `dt` and pruning expired sources. */
JCE_API void jce_vcam_impulse_integrate(const float listener_pos[3],
                                          float dt,
                                          JceVcamImpulseFrame *out);

JCE_API uint32_t jce_vcam_impulse_active_count(void);

JCE_EXTERN_C_END

#endif /* JCE_VCAM_IMPULSE_H */
