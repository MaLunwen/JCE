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

#include <jce/os/core/jce_pak_loader.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/resource/jce_asset.h>

/* Initialize the editor scene renderer (creates camera, shaders, etc.).
 * Must be called after the bgfx renderer is fully initialized.
 * pak is used to load the sky shader program at start-up. */
bool jce_editor_scene_render_init(JceRenderer *renderer,
                                  const JcePakArchive *pak,
                                  JceAssetManager *assets);

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

/* Frame the camera onto a world-space AABB so the box is fully in view.
 * Sets target to the centre and orbit_distance to fit at the current FOV.
 * Pass min_extent_pad to enforce a minimum size when the AABB is very small
 * (e.g. focusing a single point light). Pitch/yaw are preserved. */
void jce_editor_scene_camera_focus_aabb(const float min3[3], const float max3[3]);

/* Set the scene base directory for resolving mesh paths. */
void jce_editor_scene_set_scene_dir(const char *dir);

/* ── Ghost (drag-preview) model ────────────────────────────────────── */

/* Show a semi-transparent green preview mesh at the given world position.
 * mesh_path points to the model file (.fbx/.glb/.gltf/.obj).
 * Call every frame while the drag is active. */
void jce_editor_scene_set_ghost(const char *mesh_path,
                                float world_x, float world_y, float world_z);

/* Clear the ghost preview (call when drag ends or leaves viewport). */
void jce_editor_scene_clear_ghost(void);

/* ── Hover highlight for drag-drop onto entity ─────────────────────── */

/* Highlight the given entity during a drag-over (bright overlay). */
void jce_editor_scene_set_hover_entity(uint32_t entity_id);

/* Clear the hover highlight. */
void jce_editor_scene_clear_hover_entity(void);

/* ── Animation query helpers (for timeline panel) ──────────────────── */

/* Reset the animation delta-time accumulator.
 * Call when entering/exiting play mode to avoid a large first-frame spike. */
void jce_editor_scene_reset_anim_timer(void);

/* Access the engine scene renderer owned by the editor viewport.
 * Used by panels (e.g., PostFX) to access engine-owned subsystems. */
typedef struct JceSceneRenderer JceSceneRenderer;
JceSceneRenderer *jce_editor_get_scene_renderer(void);

typedef struct JceAnimPlayer JceAnimPlayer;
typedef struct JceModel      JceModel;

/* Look up (or load) the model cache entry for the skeleton path.
 * Returns the animation player, or NULL if not available. */
JceAnimPlayer *jce_editor_scene_get_anim_player(const char *skeleton_path,
                                                 uint32_t entity_id);

/* Look up (or load) the cached model for the skeleton path.
 * Returns the model, or NULL if not available. */
JceModel *jce_editor_scene_get_model(const char *skeleton_path,
                                     uint32_t entity_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_RENDER_H */
