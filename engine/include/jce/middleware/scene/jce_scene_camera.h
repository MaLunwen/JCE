/*
 * jce_scene_camera.h  Scene-authored camera to render-camera binding.
 */

#ifndef JCE_SCENE_CAMERA_H
#define JCE_SCENE_CAMERA_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>

JCE_EXTERN_C_BEGIN

typedef enum JceSceneCameraResolveResult {
    JCE_SCENE_CAMERA_RESOLVE_OK = 0,
    JCE_SCENE_CAMERA_RESOLVE_INVALID_ARGUMENT,
    JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND,
    JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS,
    JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE
} JceSceneCameraResolveResult;

typedef struct JceSceneCameraPose {
    JceEntity entity;
    jce_vec3 position;
    jce_vec3 right;
    jce_vec3 up;
    jce_vec3 forward;
    float fov_deg;
    float near_plane;
    float far_plane;
    bool orthographic;
    /* JceCameraComponent.culling_mask, verbatim.  0 = no filtering.  Carried
     * here so a host resolves pose, lens and mask in one call instead of
     * looking the component up a second time and risking a different answer
     * when two primaries are authored (which resolve_primary rejects). */
    uint32_t culling_mask;
    /* JceCameraComponent.clear_mode, verbatim.  Carried for the same reason as
     * culling_mask: one resolve answers pose, lens, mask and clear together.
     * APPENDED -- the ABI snapshot is an ordered prefix. */
    uint8_t clear_mode;
    /* JceCameraComponent.stack_index, verbatim.  0 = the base camera, 1..3 an
     * overlay drawn on top of it.  Carried for the same reason clear_mode is:
     * one resolve answers everything about this camera.  APPENDED. */
    uint8_t stack_index;
    /* JceCameraComponent.ortho_size, verbatim: orthographic HEIGHT in world
     * units, width left to the viewport aspect.  0 = unset, which leaves the
     * camera's own default.  Meaningless unless `orthographic`.  APPENDED. */
    float ortho_size;
} JceSceneCameraPose;

/* Does this clear mode draw the sky?  The ONE place the enum turns into
 * behaviour, so a host cannot answer it differently from the renderer.
 *
 * SKYBOX (0) draws it; COLOR (1) does not.  DEPTH_ONLY (2) and NOTHING (3)
 * also answer no, and that stayed true when stacking landed rather than
 * becoming a third answer: the sky question and the CLEAR question are
 * separate, and they are now answered in separate places.  This one still
 * decides the sky, for a base camera and an overlay alike.  What DEPTH_ONLY
 * and NOTHING now change is what the OVERLAY view clears, which
 * jce_scene_renderer_render_camera_overlay reads out of
 * JceSceneRenderConfig::camera_clear_mode.
 *
 * For a BASE camera the answer is also still no, and what those two modes now
 * additionally change is what the base view clears -- see
 * jce_scene_camera_clear_keeps below.  Any other value keeps today's
 * behaviour. */
JCE_API bool jce_scene_camera_clear_draws_skybox(uint8_t clear_mode);

/* What a clear mode KEEPS from the previous frame.  The second half of the
 * clear question, and the same shape as the first: the enum turns into
 * behaviour in ONE place, so a host cannot answer it differently from the
 * renderer.
 *
 *   SKYBOX (0), COLOR (1)  keep nothing -- the ordinary full clear.
 *   DEPTH_ONLY (2)         keeps COLOUR; depth and stencil are cleared, so
 *                          this camera's geometry draws over last frame's
 *                          picture without being occluded by it.
 *   NOTHING (3)            keeps BOTH, so the previous frame's depth still
 *                          rejects fragments behind it.  This is the mode
 *                          that leaves trails, and that is what it is for.
 *   anything else          keeps nothing (an out-of-range clearMode must not
 *                          invent a fourth behaviour).
 *
 * Either out pointer may be NULL.
 *
 * WHY THIS IS SAFE NOW AND WAS NOT BEFORE.  Keeping a buffer means presenting
 * whatever is already in it, and an offscreen target's colour and D24S8 depth
 * are created with `NULL, 0` (jce_offscreen_target.c) -- so on the first frame
 * of a freshly created or resized target, "keep" would present uninitialised
 * memory, and garbage depth near the near plane would reject every draw with
 * no way to recover.  The renderer therefore OVERRIDES both keeps with a full
 * clear on that one frame; jce_offscreen_target_prepare_keep owns that guard,
 * because the target is the only thing that knows it is new.  This function
 * answers only what the AUTHOR asked for. */
JCE_API void jce_scene_camera_clear_keeps(uint8_t clear_mode,
                                          bool *out_keep_color,
                                          bool *out_keep_depth);

/* Resolve the single enabled primary Camera component in world space.
 * Ambiguous authoring is rejected instead of depending on ECS iteration order. */
JCE_API JceSceneCameraResolveResult jce_scene_camera_resolve_primary(
    JceScene *scene, JceSceneCameraPose *out_pose);

/* Resolve and atomically apply the primary scene camera to a render camera.
 * out_pose is optional and receives the exact applied values.
 *
 * NOTE: this applies POSE AND LENS.  culling_mask is a RENDERER-side filter,
 * not a camera property, so it is reported in out_pose and the caller puts it
 * in JceSceneRenderConfig.camera_culling_mask -- there is nothing on JceCamera
 * to apply it to. */
JCE_API JceSceneCameraResolveResult jce_scene_camera_apply_primary(
    JceScene *scene, JceCamera *camera, JceSceneCameraPose *out_pose);

/* Apply a resolved pose (position, orientation, projection mode, fov, clip
 * planes) to a render camera.  The ONE place a JceSceneCameraPose turns into a
 * JceCamera, so a stack's overlays cannot interpret one differently from its
 * base.  Returns false on a degenerate pose, leaving the camera untouched. */
JCE_API bool jce_scene_camera_apply_pose(JceCamera *camera,
                                         const JceSceneCameraPose *pose);

/* Resolve the OVERLAY cameras of the stack, in authored order.
 *
 * An overlay is an enabled Camera component with stack_index >= 1 that is NOT
 * the primary: the primary is the base of the stack and is what
 * resolve_primary already returns.  Overlays come back sorted ascending by
 * stack_index, which is the order they draw in -- overlay 1 under overlay 2.
 *
 * TWO cameras sharing one stack_index is AMBIGUOUS and is refused, for the
 * same reason two primaries are: the alternative is depending on ECS
 * iteration order for which of two things a player sees on top.
 *
 * Zero overlays is JCE_SCENE_CAMERA_RESOLVE_OK with *out_count == 0, not
 * NOT_FOUND -- a scene with one camera is the normal case, not a failure.
 * A camera whose stack_index exceeds `max_overlays` is dropped and reported
 * by count alone; the renderer warns about the cap on its side.
 *
 * out_overlays must have room for max_overlays entries. */
JCE_API JceSceneCameraResolveResult jce_scene_camera_resolve_stack(
    JceScene *scene, JceSceneCameraPose *out_overlays, uint8_t max_overlays,
    uint8_t *out_count);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_CAMERA_H */
