/*
 * api_xr.h  XR / OpenXR subsystem.
 *
 * Pulls in the session model, action table, and stereo camera rig.
 * Runtime loader binding (xrCreateInstance + friends) is provided by
 * a separate downstream module that respects this header's
 * forward-declared opaque handles.
 */

#ifndef JCE_API_XR_H
#define JCE_API_XR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/xr/jce_xr.h>
#include <jce/middleware/xr/jce_xr_types.h>
#include <jce/renderer/jce_xr_camera.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_XR_H */
