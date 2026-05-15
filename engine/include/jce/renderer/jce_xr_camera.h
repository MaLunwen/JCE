/*
 * jce_xr_camera.h  Stereo camera rig for XR.
 *
 * Per-eye view + projection matrices computed from the head pose +
 * a caller-supplied IPD (inter-pupillary distance) and FOV envelope.
 * Used by the render loop to set up two views per frame (one per
 * eye) for stereoscopic submission.
 *
 * Sits next to (not on top of) the regular jce_camera — XR cameras
 * have a fundamentally different driving signal (HMD pose), and the
 * runtime swaps between flat-camera and XR-camera paths based on
 * jce_xr session state.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_XR_CAMERA_H
#define JCE_XR_CAMERA_H

#include <jce/middleware/xr/jce_xr_types.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    /* Field-of-view, half angles in radians.  XR runtimes report
     * asymmetric fovs per eye — store them as up/down/left/right. */
    float fov_up;
    float fov_down;
    float fov_left;
    float fov_right;
    float near_z;
    float far_z;
} JceXrEyeFov;

typedef struct {
    JceXrPose   head_pose;
    JceXrPose   eye_pose[JCE_XR_EYE_COUNT];   /* relative to head pose */
    JceXrEyeFov eye_fov [JCE_XR_EYE_COUNT];
    /* IPD in metres — derived from eye_pose offsets but cached. */
    float       ipd;
} JceXrCameraRig;

/* Initialise with a sane default FOV (90° symmetric, 0.1..1000). */
JCE_API void jce_xr_camera_init(JceXrCameraRig *r);

/* Compute view matrix for `eye`.  Output is column-major 4x4. */
JCE_API void jce_xr_camera_view(const JceXrCameraRig *r,
                                  JceXrEye eye,
                                  float out_view[16]);

/* Compute projection matrix for `eye` from asymmetric FOV. */
JCE_API void jce_xr_camera_projection(const JceXrCameraRig *r,
                                        JceXrEye eye,
                                        float out_proj[16]);

JCE_EXTERN_C_END

#endif /* JCE_XR_CAMERA_H */
