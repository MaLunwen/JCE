/*
 * jce_virtual_camera.h  Cinemachine-style virtual cameras (Sprint 4 #17).
 *
 * A virtual camera describes a *desire* for the live (real) camera: a
 * position+target+fov, plus a follow target, optional offset, damping
 * and priority.  A small manager picks the highest-priority *active*
 * vcam, blends from the previous one over a configurable duration,
 * and writes the result into a plain output struct that the renderer
 * (or scene system) feeds into the actual view+proj matrices.
 *
 * Out of scope here: noise, dolly tracks, group composer, collision.
 * Those can layer on top of jce_vcam_evaluate by post-processing the
 * output before assigning to the live camera.
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

typedef struct JceVcamManager JceVcamManager;

JCE_API JceVcamManager *JCE_CALL jce_vcam_manager_create(void);
JCE_API void            JCE_CALL jce_vcam_manager_destroy(JceVcamManager *m);

JCE_API int  JCE_CALL jce_vcam_add(JceVcamManager *m, const JceVirtualCamera *cam);
JCE_API void JCE_CALL jce_vcam_update(JceVcamManager *m, int handle, const JceVirtualCamera *cam);
JCE_API void JCE_CALL jce_vcam_remove(JceVcamManager *m, int handle);
JCE_API int  JCE_CALL jce_vcam_count (const JceVcamManager *m);

JCE_API void JCE_CALL jce_vcam_set_blend_duration(JceVcamManager *m, float seconds);

/* Advance the manager and produce the live camera output for this frame. */
JCE_API void JCE_CALL jce_vcam_evaluate(JceVcamManager *m, float dt, JceVcamOutput *out);

/* Introspection (for editor panel). */
JCE_API const JceVirtualCamera *JCE_CALL jce_vcam_get(const JceVcamManager *m, int handle);
JCE_API int                     JCE_CALL jce_vcam_active_handle(const JceVcamManager *m);

JCE_EXTERN_C_END

#endif /* JCE_VIRTUAL_CAMERA_H */
