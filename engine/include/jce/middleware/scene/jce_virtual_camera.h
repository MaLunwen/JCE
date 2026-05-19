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

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_VCAM_TRACK_NONE        = 0, /* use vcam.position directly */
    JCE_VCAM_TRACK_FOLLOW      = 1, /* position = follow_pos + offset */
    JCE_VCAM_TRACK_LOOK_AT     = 2, /* target   = look_at_pos */
    JCE_VCAM_TRACK_FOLLOW_LOOK = 3  /* both */
} JceVcamTrackMode;

/* Body / aim rigs (Cinemachine taxonomy, phase 2). */
typedef enum {
    JCE_VCAM_RIG_BASIC       = 0,  /* legacy behaviour */
    JCE_VCAM_RIG_FREELOOK    = 1,  /* three-orbit arc */
    JCE_VCAM_RIG_THIRD_PERSON_AIM = 2,
} JceVcamRigType;

typedef enum {
    JCE_VCAM_LOOKAT_DAMPED = 0,
    JCE_VCAM_LOOKAT_HARD   = 1,    /* snap, ignore damping */
} JceVcamLookatMode;

/* FreeLook rig: three orbits stacked vertically; pitch (-1..1) selects
 * which orbit + interpolates between adjacent ones. */
typedef struct {
    /* Orbit radii + heights (top, middle, bottom). */
    float orbit_radius[3];
    float orbit_height[3];
    /* Current orbit position in [-1, 1]: -1 bottom, 0 mid, +1 top. */
    float vertical_axis;
    /* Yaw in radians (rotation around target's up axis). */
    float horizontal_axis;
    /* Speed multipliers (input × speed = axis delta per second). */
    float vertical_speed;
    float horizontal_speed;
} JceVcamFreeLookRig;

/* Composer framing rule: keep target inside a screen-space rect. */
typedef struct {
    float screen_x;          /* desired NDC X of target (-1..1) */
    float screen_y;
    float dead_zone_w;       /* tolerance before any correction */
    float dead_zone_h;
    float soft_zone_w;       /* outside dead zone but inside soft, damped */
    float soft_zone_h;
    /* If true, snap target instantly to screen center (hard look-at). */
    JceVcamLookatMode lookat_mode;
} JceVcamComposer;

/* DollyCart rig: position the camera along a path curve.  The path
 * is a polyline of waypoints; `position_along_path` ∈ [0, 1] picks
 * the interpolated point.  Speed is units/second along path length. */
#define JCE_VCAM_DOLLY_MAX_WAYPOINTS 32

typedef struct {
    float    waypoints[JCE_VCAM_DOLLY_MAX_WAYPOINTS][3];
    uint32_t waypoint_count;
    float    position_along_path; /* 0..1 normalised */
    float    speed;               /* units per second */
    bool     auto_advance;
    bool     loop;
} JceVcamDollyCart;

/* Group composer: aim at the weighted centroid of N targets and
 * frame a bounding circle around them. */
#define JCE_VCAM_GROUP_MAX_TARGETS 8

typedef struct {
    float    target_pos[JCE_VCAM_GROUP_MAX_TARGETS][3];
    float    target_weight[JCE_VCAM_GROUP_MAX_TARGETS];
    /* Per-target radius added when computing the bounding circle. */
    float    target_radius[JCE_VCAM_GROUP_MAX_TARGETS];
    uint32_t target_count;
    /* When true, framing pulls the camera back so the bounding
     * circle fits inside the frame; else only re-aims look-at. */
    bool     adjust_distance;
    /* Minimum allowed framing distance (camera-to-centroid). */
    float    min_distance;
    /* Maximum allowed framing distance — caps zoom-out for very
     * spread groups. */
    float    max_distance;
} JceVcamGroupComposer;

typedef struct {
    char    name[64];
    int32_t priority;     /* higher = wins */
    int     active;       /* 0 = ignored */

    JceVcamTrackMode mode;
    JceVcamRigType   rig;          /* basic / freelook / third-person */

    float position[3];
    float target  [3];
    float fov_deg;

    float follow_pos [3]; /* updated externally each frame */
    float look_at_pos[3];
    float offset     [3];

    float damping;        /* 0 = snap, larger = slower; ~5 is "filmic" */

    /* Phase-2 rig + composer (active when rig != BASIC). */
    JceVcamFreeLookRig freelook;
    JceVcamComposer    composer;
    /* B21 extras (active when respective struct has data). */
    JceVcamDollyCart      dolly;
    JceVcamGroupComposer  group;
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
