/*
 * jce_xr_types.h  Common XR primitives.
 *
 * Layer 4 — middleware/xr.  Shared types so xr.h + xr_camera.h don't
 * cyclically depend.
 */

#ifndef JCE_XR_TYPES_H
#define JCE_XR_TYPES_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_XR_EYE_LEFT  = 0,
    JCE_XR_EYE_RIGHT = 1,
    JCE_XR_EYE_COUNT = 2,
} JceXrEye;

typedef struct {
    float position[3];
    float orientation[4]; /* xyzw quaternion */
    /* True when this pose was recently updated by the runtime (not
     * extrapolated). */
    bool  is_valid;
    /* Timestamp in seconds since session start. */
    double time_seconds;
} JceXrPose;

typedef enum {
    JCE_XR_ACTION_BOOL    = 0,
    JCE_XR_ACTION_FLOAT   = 1,
    JCE_XR_ACTION_VEC2    = 2,
    JCE_XR_ACTION_POSE    = 3,
    JCE_XR_ACTION_HAPTIC  = 4,
} JceXrActionKind;

typedef struct {
    /* Action handle is opaque (e.g. OpenXR XrAction*).  Stored as
     * void* so this header doesn't pull <openxr.h>. */
    void          *runtime_handle;
    char           name[48];
    JceXrActionKind kind;
    /* Current value cached on poll. */
    union {
        bool      b;
        float     f;
        float     v2[2];
        JceXrPose pose;
    } value;
    bool active;
} JceXrAction;

JCE_EXTERN_C_END

#endif /* JCE_XR_TYPES_H */
