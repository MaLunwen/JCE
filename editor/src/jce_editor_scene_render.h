/*
 * jce_editor_scene_render.h  3D scene rendering for the editor viewport.
 *
 * Renders to an off-screen framebuffer (FBO) so the resulting texture
 * can be displayed inside an ImGui panel via ImGui::Image().
 *
 * Draws: sky gradient, grid, entity meshes (auto-detected components).
 */

#ifndef JCE_EDITOR_SCENE_RENDER_H
#define JCE_EDITOR_SCENE_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer  JceRenderer;
typedef struct JceCamera    JceCamera;
typedef struct PakArchive   PakArchive;

/* Initialize the editor scene renderer (creates camera, shaders, etc.).
 * Must be called after the bgfx renderer is fully initialized.
 * pak is used to load the sky shader program at start-up. */
bool jce_editor_scene_render_init(JceRenderer *renderer, const PakArchive *pak);

/* Shut down and free all resources (camera, FBO, meshes). */
void jce_editor_scene_render_shutdown(void);

/* Render one frame of the 3D scene to the internal FBO.
 * The FBO is resized automatically when width/height change.
 * Call this from inside the ImGui panel so the correct size is known.
 * width/height are the viewport content area in pixels. */
void jce_editor_scene_render_frame(uint32_t width, uint32_t height);

/* Get the bgfx texture handle index of the FBO color attachment.
 * Returns UINT16_MAX if not yet initialized. Use with ImGui::Image():
 *   ImGui::Image((ImTextureID)(uintptr_t)handle, size); */
uint16_t jce_editor_scene_render_get_texture(void);

/* Get the editor camera (for gizmo projection, etc.). */
JceCamera *jce_editor_scene_get_camera(void);

/* Get camera view/projection as flat 16-float arrays (C++-safe).
 * Returns false if camera is not initialized. */
bool jce_editor_scene_get_camera_matrices(float *out_view16,
                                           float *out_proj16,
                                           float *out_eye3,
                                           float viewport_w,
                                           float viewport_h);

/* ── Maya-style orbit camera controls ──────────────────────────────── */

/* Orbit the camera around its target by delta yaw/pitch (radians). */
void jce_editor_scene_camera_orbit(float dyaw, float dpitch);

/* Pan the camera (move both eye and target) in view-relative XY. */
void jce_editor_scene_camera_pan(float dx, float dy);

/* Zoom the camera by changing its orbit distance (positive = closer). */
void jce_editor_scene_camera_zoom(float delta);

/* Get/set the orbit target point (default: 0,0,0). */
void jce_editor_scene_camera_get_target(float *out3);
void jce_editor_scene_camera_set_target(float x, float y, float z);

/* Snap camera to a preset view direction around current target. */
typedef enum {
    JCE_CAM_VIEW_FRONT,
    JCE_CAM_VIEW_BACK,
    JCE_CAM_VIEW_LEFT,
    JCE_CAM_VIEW_RIGHT,
    JCE_CAM_VIEW_TOP,
    JCE_CAM_VIEW_BOTTOM,
} JceCamPresetView;
void jce_editor_scene_camera_snap_view(JceCamPresetView preset);

/* Reset camera to the default position and target (Persp 45°, pos 8,6,8 → 0,0,0). */
void jce_editor_scene_camera_reset(void);

/* Set the scene base directory for resolving mesh paths. */
void jce_editor_scene_set_scene_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_RENDER_H */
