/*
 * jce_xr.h  OpenXR session / action authoring (data layer).
 *
 * Stores the headset + controller pose ring + named action table.
 * The actual OpenXR loader binding (xrCreateInstance, xrPollEvent,
 * etc.) lives in a downstream TU; this module is the engine-side
 * model game code talks to.
 *
 * Forward-declares OpenXR handles as void* so consumers don't pull
 * <openxr.h> — keeps JCE buildable without the SDK.
 *
 * Layer: middleware/xr (Layer 4) — public.
 */

#ifndef JCE_XR_H
#define JCE_XR_H

#include <jce/middleware/xr/jce_xr_types.h>

JCE_EXTERN_C_BEGIN

#define JCE_XR_POSE_RING_SIZE    16
#define JCE_XR_ACTION_TABLE_SIZE 64

typedef enum {
    JCE_XR_STATE_INACTIVE    = 0,
    JCE_XR_STATE_INITIALIZED = 1,
    JCE_XR_STATE_SESSION_RUN = 2,
    JCE_XR_STATE_SESSION_END = 3,
} JceXrSessionState;

typedef struct {
    JceXrSessionState state;
    /* Per-eye poses for the current frame. */
    JceXrPose         head_pose;
    JceXrPose         eye_pose[JCE_XR_EYE_COUNT];

    /* Pose ring per controller (left = 0, right = 1). */
    JceXrPose         controller_left [JCE_XR_POSE_RING_SIZE];
    JceXrPose         controller_right[JCE_XR_POSE_RING_SIZE];
    uint32_t          left_head;
    uint32_t          right_head;

    JceXrAction       actions[JCE_XR_ACTION_TABLE_SIZE];

    /* Opaque runtime objects (XrInstance, XrSession, ...). */
    void *instance_handle;
    void *session_handle;
} JceXrSession;

JCE_API void jce_xr_session_init(JceXrSession *s);

/* Push a head + eye pose snapshot for the current frame. */
JCE_API void jce_xr_set_head_pose(JceXrSession *s, const JceXrPose *head);
JCE_API void jce_xr_set_eye_pose (JceXrSession *s, JceXrEye eye,
                                    const JceXrPose *pose);

/* Append a controller pose to the corresponding ring. */
JCE_API void jce_xr_push_controller_pose(JceXrSession *s,
                                           bool is_right_hand,
                                           const JceXrPose *pose);

/* Latest controller pose convenience. */
JCE_API const JceXrPose *jce_xr_latest_controller_pose(const JceXrSession *s,
                                                         bool is_right_hand);

/* Register / find an action by name.  Caller pre-fills `kind`. */
JCE_API JceXrAction *jce_xr_register_action(JceXrSession *s,
                                              const char *name,
                                              JceXrActionKind kind);
JCE_API JceXrAction *jce_xr_find_action(JceXrSession *s, const char *name);

JCE_API uint32_t jce_xr_action_count(const JceXrSession *s);

JCE_EXTERN_C_END

#endif /* JCE_XR_H */
