/*
 * jce_virtual_camera.h  Cinemachine-style virtual-camera data types.
 *
 * A virtual camera describes a *desire* for the live (real) camera: a
 * position+target+fov, plus a follow target, optional offset, damping
 * and priority.  The live consumer is the ECS-driven resolver in
 * <jce/middleware/scene/jce_vcam_system.h>, which picks the highest-priority
 * active vcam, damps toward it, and writes a JceVcamOutput that the renderer
 * feeds into the view+proj matrices.
 *
 * This header now carries only the shared value types (JceVcamTrackMode,
 * JceVirtualCamera, JceVcamOutput).  The earlier standalone handle-based
 * JceVcamManager API was removed in v0.9.4 — it duplicated the ECS path and
 * had no callers; jce_vcam_system.h is the single live entry point.
 */

#ifndef JCE_VIRTUAL_CAMERA_H
#define JCE_VIRTUAL_CAMERA_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_VCAM_TRACK_NONE        = 0, /* use vcam.position directly */
    JCE_VCAM_TRACK_FOLLOW      = 1, /* position = follow_pos + offset */
    JCE_VCAM_TRACK_LOOK_AT     = 2, /* target   = look_at_pos */
    JCE_VCAM_TRACK_FOLLOW_LOOK = 3  /* both */
} JceVcamTrackMode;

typedef struct {
    char    name[64];
    int32_t priority;     /* higher = wins */
    int     active;       /* 0 = ignored */

    JceVcamTrackMode mode;

    float position[3];
    float target  [3];
    float fov_deg;

    float follow_pos [3]; /* updated externally each frame */
    float look_at_pos[3];
    float offset     [3];

    float damping;        /* 0 = snap, larger = slower; ~5 is "filmic" */
} JceVirtualCamera;

typedef struct {
    float position[3];
    float target  [3];
    float fov_deg;
} JceVcamOutput;

JCE_EXTERN_C_END

#endif /* JCE_VIRTUAL_CAMERA_H */
