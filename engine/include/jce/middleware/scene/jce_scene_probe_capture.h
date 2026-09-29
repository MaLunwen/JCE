/*
 * jce_scene_probe_capture.h  Render a reflection probe's six faces from the
 *                            actual scene.
 *
 * WHAT THIS REPLACES.  jce_reflection_probe_bake.c fills every face with "a
 * procedural sky-and-ground gradient derived from the probe position" and says
 * so in its own header under "v1 limitation".  So a probe reflected a synthetic
 * sky, never the room it stands in -- the artefact pipeline (button, progress,
 * component references the file, renderer samples it) was complete end to end
 * around a picture of nothing in the scene.
 *
 * HOW IT RENDERS.  Six 90-degree views through the ordinary scene renderer and
 * the ordinary offscreen bridge, one face per frame, read back with the same
 * blit + bgfx_read_texture the impostor bake uses.  Nothing about the scene is
 * special-cased: a probe sees the sky, the terrain, the shadows and the
 * materials the camera sees, because it IS the camera path.
 *
 * WHY IT IS MODAL.  It renders from the editor scene view's base id, and both
 * editor viewports yield while it runs -- exactly the arrangement the impostor
 * bake already uses and for the same reason.  A full scene render claims 117
 * view ids; two viewport bases plus the fixed ids leave no third span of that
 * size in bgfx's 256, and the alternative to yielding is two owners on one view
 * id with one of them silently producing nothing.  A bake taking a few frames
 * of viewport is what Unity's lightmap bake and this engine's impostor bake
 * both already do.
 *
 * FAILS TO THE OLD BEHAVIOUR.  If a capture cannot start -- no renderer, no
 * target, an unsupported readback -- the caller submits the procedural bake
 * instead, so a probe is never worse off than before this existed.
 */

#ifndef JCE_SCENE_PROBE_CAPTURE_H
#define JCE_SCENE_PROBE_CAPTURE_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/renderer/jce_scene_renderer.h>

JCE_EXTERN_C_BEGIN

typedef enum JceProbeCaptureStatus {
    JCE_PROBE_CAPTURE_IDLE = 0,
    JCE_PROBE_CAPTURE_RENDERING,   /* a face is being drawn / read back */
    JCE_PROBE_CAPTURE_DONE,        /* faces captured; the bake was submitted */
    JCE_PROBE_CAPTURE_FAILED
} JceProbeCaptureStatus;

/* The camera basis for one cube face.  Split out and exported because it is
 * the one part of a GPU capture a test can hold still: a cubemap convention
 * that is mirrored or rotated produces a plausible image whose reflections
 * point the wrong way, and this must agree with the sampling convention the
 * bake and the IBL convolutions use.
 *
 * `face` is 0..5 in +X,-X,+Y,-Y,+Z,-Z order.  Returns false for anything else
 * and leaves the outputs untouched. */
JCE_API bool JCE_CALL jce_probe_face_basis(int face, jce_vec3 *out_forward,
                                           jce_vec3 *out_up);

/* Start a capture for `probe` in `scene`.  `face_size` is the cubemap face
 * resolution and `out_path_ktx` the artefact the bake will write.
 *
 * Returns false when a capture or bake is already running, when the arguments
 * are unusable, or when the renderer cannot provide a readable target -- in
 * every one of those cases the caller should fall back to
 * jce_reflection_probe_bake_submit(), which is the behaviour that shipped
 * before this existed. */
JCE_API bool JCE_CALL jce_scene_probe_capture_begin(JceScene *scene,
                                                    JceEntity probe,
                                                    uint32_t face_size,
                                                    const char *out_path_ktx);

/* True while faces are being rendered or read back.  The editor viewports test
 * this and skip their own render for the frame, the same way they already do
 * for jce_impostor_bake_in_flight(). */
JCE_API bool JCE_CALL jce_scene_probe_capture_in_flight(void);

/* Drive the capture one frame.  Call once per frame from the host's render
 * loop with the scene renderer that owns the scene view's base id.  Returns
 * the status after this frame's work. */
JCE_API JceProbeCaptureStatus JCE_CALL
jce_scene_probe_capture_poll(JceSceneRenderer *sr, JceScene *scene,
                             JceRenderer *renderer);

/* Abandon an in-flight capture and release its GPU objects.  Safe when idle. */
JCE_API void JCE_CALL jce_scene_probe_capture_cancel(void);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_PROBE_CAPTURE_H */
