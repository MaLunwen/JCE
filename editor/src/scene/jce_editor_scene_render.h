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

#include <jce/resource/jce_pak_loader.h>
#include <jce/middleware/scene/jce_scene.h>
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

/* Rebind project-relative content after a source-project switch or VFS mount
 * transition.  In isolated Bundle Preview this reads render settings and
 * component assets only from the active bundle.  Call before scene entities
 * are deserialized so particle descriptors resolve in the correct context. */
void jce_editor_scene_render_refresh_content_context(void);

/* Render one frame of the 3D scene to the internal FBO.
 * The FBO is resized automatically when width/height change.
 * Call this from inside the ImGui panel so the correct size is known.
 * width/height are the viewport content area in pixels. */
void jce_editor_scene_render_frame(uint32_t width, uint32_t height);

/* Get the bgfx texture handle index of the FBO color attachment.
 * Returns UINT16_MAX if not yet initialized. Use with ImGui::Image(),
 * encoding the handle as (idx + 1) so a valid idx 0 is not mistaken for
 * ImTextureID_Invalid(0) — the imgui_renderer backend decodes -1 (audit P2-B):
 *   ImGui::Image((ImTextureID)(uintptr_t)((uint32_t)handle + 1u), size); */
uint16_t jce_editor_scene_render_get_texture(void);

/* Capture the Scene View's final (post-graded) image to a PNG at `path`
 * (async — written next frame by the renderer's screen_shot callback).  Used by
 * F12 screenshot / F9 recording: the D3D flip-model BACKBUFFER capture returns
 * all-black even when focused, so this grabs the offscreen postfx-output FBO
 * instead.  Returns false if the Scene View isn't ready. */
bool jce_editor_scene_render_screenshot(const char *path);

/* Pump an in-flight read-back capture started by jce_editor_scene_render_screenshot;
 * call once per frame.  Returns -1 idle, 0 pending, 1 wrote PNG, 2 write failed. */
int jce_editor_scene_render_capture_poll(void);

/* GPU object-ID picking for the Scene View.  Coordinates are render-target
 * pixels, not absolute ImGui screen coordinates.  Requests are asynchronous:
 * call request() after jce_editor_scene_render_frame(), then poll on later
 * frames until it returns true. */
bool jce_editor_scene_pick_supported(void);
bool jce_editor_scene_pick_request(uint32_t x, uint32_t y);
bool jce_editor_scene_pick_poll(uint32_t *out_entity_id);
/* GPU rectangle (marquee) selection: pixel-accurate for any geometry incl.
 * streamed glTF models.  request() with pick-buffer pixel coords; poll_rect()
 * fills out_ids with up to max_ids UNIQUE entity ids + *out_count when ready. */
bool jce_editor_scene_pick_request_rect(uint32_t x0, uint32_t y0,
                                        uint32_t x1, uint32_t y1);
bool jce_editor_scene_pick_poll_rect(uint32_t *out_ids, uint32_t max_ids,
                                     uint32_t *out_count);

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

/* Capture / restore the full orbit camera state (camera bookmarks):
 * target point + yaw + pitch + distance. set_state restores an exact view. */
void jce_editor_scene_camera_get_state(float out_target3[3], float *out_yaw,
                                       float *out_pitch, float *out_distance);
void jce_editor_scene_camera_set_state(const float target3[3], float yaw,
                                       float pitch, float distance);

/* Restore the orbit pose saved per scene in the project state (written
 * continuously as the camera moves).  Returns false — leaving the current
 * framing untouched — when the scene has no stored pose. */
bool jce_editor_scene_camera_restore_pose(const char *scene_path);

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

/* Bird's-eye "Overview": frame the WHOLE streamed world from a high angle.
 * World AABB = union of the scene's streaming chunk table (center ± radius over
 * all authored chunks, not just loaded ones); falls back to the ±2757 m world
 * span when the scene has no streaming settings. Sets a steep pitch (~ -65°)
 * and animates the orbit target/distance there so the camera swings up smoothly
 * (driven by jce_editor_scene_camera_update). */
void jce_editor_scene_frame_overview(void);

/* Position the scene camera at a fixed eye-level vista over the meadow centre
 * (headless visual-QA aid; armed by env JCE_DBG_VISTA=1). */
void jce_editor_scene_frame_vista(void);

/* Frame the camera onto a world-space AABB so the box is fully in view.
 * Sets target to the centre and orbit_distance to fit at the current FOV.
 * Pass min_extent_pad to enforce a minimum size when the AABB is very small
 * (e.g. focusing a single point light). Pitch/yaw are preserved. */
void jce_editor_scene_camera_focus_aabb(const float min3[3], const float max3[3]);

/* Frame the camera onto an entity transform using the same fallback bounds
 * as Scene View selection framing. */
void jce_editor_scene_camera_focus_transform(const JceTransform *transform);

/* Resolve the best available focus bounds for an entity. Mesh renderers use
 * the cached mesh's true local AABB when loaded; other entities fall back to
 * a small transform-scale box. Returns false when the entity cannot be framed. */
bool jce_editor_scene_camera_get_entity_focus_bounds(uint32_t entity_id,
                                                     float out_min3[3],
                                                     float out_max3[3]);

void jce_editor_scene_camera_focus_entity(uint32_t entity_id);

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

/* Access the low-level engine renderer owned by the editor viewport (used by
 * the F9 screen recorder to toggle backbuffer capture). */
typedef struct JceRenderer JceRenderer;
JceRenderer *jce_editor_get_renderer(void);

/* Resolve a scene-relative or assetdb-relative asset path to an absolute
 * disk path the host filesystem can open.  Tries (in order): the input
 * as-is, assetdb root + input, and a 5-level walk-up from the current
 * scene file's directory.  Returns true and fills `out` on success. */
bool jce_editor_resolve_asset_path(const char *in, char *out, int outsz);

/* World-streaming FS base (source asset root = scene file's grandparent).
 * Shared by the scene-view preview streamer AND the editor-Play streamer so
 * chunk fragments resolve identically in both viewports (no divergence). */
bool jce_editor_streaming_fs_base(char *out, int out_size);

/* Access the world streamer (open-world chunk I/O + scene entity lifecycle).
 * Returns NULL if not initialized (e.g. before first frame). */
typedef struct JceWorldStreamer JceWorldStreamer;
JceWorldStreamer *jce_editor_get_world_streamer(void);

/* Destroy the preview streamer (and with it every streamed chunk entity in
 * the live scene).  Idempotent.  Called before scene save/swap so streamed
 * content never bakes into the main scene file. */
void jce_editor_scene_render_streaming_teardown(void);

/* Drop the renderer + pick model caches (neither evicts; both cache load
 * failures) so a scene switch reloads all models fresh — fixes models that
 * fail to load after switching scenes without restarting the editor.  Call
 * on every scene swap, after clear_scene_entities() and before loading the
 * new scene's entities. */
void jce_editor_scene_render_invalidate_model_caches(void);

/* Drop the scene-view occlusion culler's per-entity slots (returns the bgfx
 * query pool).  Call whenever the ECS world is destroyed+recreated: entity
 * ids restart, so stale slots would false-cull recreated entities (objects
 * vanishing after undo) and exhaust the 256-query pool. */
void jce_editor_scene_render_reset_occlusion(void);

/* Recreate the preview streamer from the current scene's streaming
 * settings.  Tears down any existing streamer first; creates a new one
 * only when the scene has streaming enabled AND the session preview
 * toggle (jce_state_get/set_streaming_preview) is on.  Call after every
 * scene swap and after the World Streaming panel edits settings. */
void jce_editor_scene_render_streaming_rebuild(void);

/* Call after a scene load: auto-enables the streaming preview for streaming-
 * enabled scenes (so the editor scene view shows the streamed world, matching
 * Play) then rebuilds.  The user can still toggle preview off afterwards. */
void jce_editor_scene_render_streaming_autostart(void);

typedef struct JceAnimPlayer JceAnimPlayer;
typedef struct JceModel      JceModel;

/* Look up (or load) the model cache entry for the skeleton path.
 * Returns the animation player, or NULL if not available. */
JceAnimPlayer *jce_editor_scene_get_anim_player(const char *skeleton_path,
                                                 uint32_t entity_id);

typedef struct JceAnimSmBinding JceAnimSmBinding;
/* Live state-machine binding for an entity's skeletal animator, or NULL until
 * the SM has been bound (set sm_path + the scene rendered at least once). */
JceAnimSmBinding *jce_editor_scene_get_anim_sm(uint32_t entity_id);

/* Look up (or load) the cached model for the skeleton path.
 * Returns the model, or NULL if not available. */
JceModel *jce_editor_scene_get_model(const char *skeleton_path,
                                     uint32_t entity_id);

/* Lightweight rig probe for a glTF/GLB asset path: resolves the path,
 * reads the file, and cgltf-parses only its header to report whether it
 * carries a skin and/or animation clips.  Does NOT build geometry, decode
 * images, or create an animation player — so the editor's asset-drop path
 * can decide "skinned?" without the heavy synchronous full-model load (an
 * 8 K-vertex / 45-clip character is microseconds of parse vs building the
 * whole model + clip player on the main thread).  Returns false if the
 * path can't be resolved/read/parsed (caller should treat as static). */
bool jce_editor_probe_model_rig(const char *asset_path,
                                bool *out_has_skin, bool *out_has_anim);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_RENDER_H */
