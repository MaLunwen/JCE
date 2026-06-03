/*
 * jce_scene_render_internal.h  Shared state for scene render subsystem files.
 *
 * Phase B refactor: scene rendering (sky, shadows, entities, IBL, sprites,
 * skybox, post-fx parameters) is owned by the engine `JceSceneRenderer`.
 * The editor only retains overlay state (grid, selection, ghost, hover)
 * and the orbit camera.
 */

#ifndef JCE_SCENE_RENDER_INTERNAL_H
#define JCE_SCENE_RENDER_INTERNAL_H

#include "jce_editor_scene_render.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "scene/jce_scene_camera_focus.h"

#include <math.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>
#include <jce/resource/jce_world_streamer.h>
}

#define LOG_TAG "scene_render"

/* ── Background color ─────────────────────────────────────────────── */

#define BG_COLOR_RGBA  0x365FA0FF

/* ── Vertex type for transient buffers (grid / overlays) ──────────── */

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

/* ── Internal state (single instance, defined in jce_editor_scene_render.cpp) */

struct SceneRenderState {
    bool                    initialized;
    bool                    homogeneous_depth;
    JceRenderer            *renderer;
    JceOffscreenTarget  *bridge;
    JceCamera              *camera;

    /* Engine-owned scene renderer (sky, shadows, entities, sprites, IBL). */
    JceSceneRenderer       *scene_renderer;

    /* Pos+color vertex layout used by grid / overlay transient buffers. */
    JceVertexLayout         layout;

    /* Cached color shader handle from the renderer (overlay flat-color path). */
    JceProgramHandle        prog_color;

    /* Editor-only infinite grid shader. */
    JceProgramHandle        prog_grid;
    JceUniformHandle        u_grid_camera;
    JceUniformHandle        u_grid_fade;

    /* Lighting uniforms used by selection / hover / ghost flat-color overlays. */
    JceUniformHandle        u_light_dir;
    JceUniformHandle        u_light_color;

    /* Orbit camera state. */
    jce_vec3                orbit_target;
    float                   orbit_distance;
    float                   orbit_yaw;
    float                   orbit_pitch;
    bool                    orbit_clip_valid;
    float                   orbit_near_cached;
    float                   orbit_far_cached;

    /* Smooth frame-selection focus state. */
    JceEditorSceneFocusAnim focus_anim;
    bool                    focus_last_bounds_valid;
    float                   focus_last_min[3];
    float                   focus_last_max[3];
    int                     focus_zoom_step;

    /* Cached camera matrices for the current frame. */
    bool                    camera_cache_valid;
    float                   cached_view[16];
    float                   cached_proj[16];
    float                   cached_eye[3];

    /* 1x1 white texture used by overlay flat-color binding. */
    JceTextureHandle        white_tex;

    /* Viewport size from the current frame. */
    uint32_t                viewport_width;
    uint32_t                viewport_height;

    /* PostFX output (from engine pipeline, applied by frame()). */
    uint16_t                postfx_output_tex;

    /* Ghost (drag-preview) model state. */
    bool         ghost_active;
    char         ghost_mesh_path[512];
    float        ghost_pos[3];

    /* Hover highlight for drag-drop onto entity. */
    uint32_t     hover_entity_id;

    /* Animation delta-time accumulator (driven by frame()). */
    uint64_t     anim_last_ticks;

    /* World streamer — optional open-world chunk streaming. */
    JceWorldStreamer   *world_streamer;
    JceFileSystem      *stream_fs;

    /* GPU-query occlusion culler — optional two-pass coherence culling. */
    JceOcclusionCuller *occlusion_culler;
};

extern SceneRenderState s_sr;

/* ── Shared helpers ───────────────────────────────────────────────── */

uint16_t scene_view_id(void);
JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos);
JceTexture get_cached_texture(const char *material_path, const char *mesh_path);

/* ── Functions from jce_scene_render_draw.cpp ─────────────────────── */

void draw_grid(void);
void draw_selection_outlines(void);
void draw_ghost_entity(void);
void draw_hover_highlight(void);
void draw_physics_debug(void);
void draw_joint_gizmos(void);
void draw_cloth_gizmos(void);
void draw_compound_collider_gizmos(void);

/* ── Functions from jce_scene_render_camera.cpp ───────────────────── */

void orbit_apply(void);
void jce_editor_scene_camera_update(float dt_sec);

#endif /* JCE_SCENE_RENDER_INTERNAL_H */
