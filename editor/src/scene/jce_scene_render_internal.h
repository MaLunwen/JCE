/*
 * jce_scene_render_internal.h  Shared state for scene render subsystem files.
 */

#ifndef JCE_SCENE_RENDER_INTERNAL_H
#define JCE_SCENE_RENDER_INTERNAL_H

#include "jce_editor_scene_render.h"
#include "jce_editor_state.h"
#include "scene/jce_editor_scene_asset_cache.h"

#include <bgfx/c99/bgfx.h>
#include <math.h>
#include <string.h>

extern "C" {
#include <jce/core/jce_log.h>
#include <jce/core/jce_math.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_csm.h>
#include <jce/graphics/jce_debug_draw.h>
#include <jce/graphics/jce_editor_render_bridge.h>
#include <jce/graphics/jce_ibl.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/graphics/jce_lighting_system.h>
#include <jce/graphics/jce_material.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_model.h>
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_renderer_caps.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_skybox.h>
#include <jce/graphics/jce_sprite_batch.h>
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_views.h>
#include <jce/animation/jce_animation.h>
#include <jce/core/pak_loader.h>
}

#define LOG_TAG "scene_render"

/* ── Background color ─────────────────────────────────────────────── */

#define BG_COLOR_RGBA  0x365FA0FF

/* ── Model / animation cache for viewport playback ───────────────── */

#define MODEL_CACHE_MAX 32

struct ModelCacheEntry {
    char            path[256];
    JceModel       *model;
    JceAnimPlayer  *player;
    uint32_t        bound_entity;   /* entity that owns this entry */
    int             active_clip;
    bool            loop;
    float           speed;
    bool            paused;
    bool            used;
};

/* ── Vertex type for transient buffers ────────────────────────────── */

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

/* ── Internal state (single instance, defined in jce_editor_scene_render.cpp) */

struct SceneRenderState {
    bool                    initialized;
    bool                    homogeneous_depth;
    JceRenderer            *renderer;
    JceEditorRenderBridge  *bridge;
    JceCamera              *camera;
    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   prog_color;
    bgfx_program_handle_t   prog_grid;

    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_sky_params;
    bgfx_uniform_handle_t   u_sky_equirect;
    bgfx_uniform_handle_t   u_grid_camera;
    bgfx_uniform_handle_t   u_grid_fade;

    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    jce_vec3                orbit_target;
    float                   orbit_distance;
    float                   orbit_yaw;
    float                   orbit_pitch;
    bool                    orbit_clip_valid;
    float                   orbit_near_cached;
    float                   orbit_far_cached;

    bool                    camera_cache_valid;
    float                   cached_view[16];
    float                   cached_proj[16];
    float                   cached_eye[3];

    bgfx_texture_handle_t   white_tex;
    bgfx_texture_handle_t   checker_tex;

    bgfx_uniform_handle_t   u_light_dir;
    bgfx_uniform_handle_t   u_light_color;

    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;
    bool                       shadow_use_csm;
    uint16_t                   shadow_map_size;
    bool                       shadow_far_valid;
    float                      shadow_far_cached;

    /* Cascaded shadow maps. */
    uint32_t                   csm_cascade_count;   /* 0=single shadow, 2-4=CSM */
    bgfx_texture_handle_t      csm_tex[JCE_CSM_MAX_CASCADES];
    bgfx_frame_buffer_handle_t csm_fbo[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_samplers[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_vp;     /* mat4[4] */
    bgfx_uniform_handle_t      u_csm_splits;
    bgfx_uniform_handle_t      u_csm_params;       /* vec4: invMapSize, blendRange, normalBias, filterRadius */
    bgfx_uniform_handle_t      u_csm_bias_scales;  /* vec4: per-cascade bias multipliers */
    bool                       csm_valid;
    float                      csm_blend_ratio;
    float                      csm_normal_bias;
    float                      csm_filter_radius;

    JceLightEnv              *light_env;

    /* Skybox / IBL state. */
    JceSkybox               *skybox;
    JceIblData              *ibl_data;
    bgfx_texture_handle_t    brdf_lut;       /* shared, created once */
    bgfx_uniform_handle_t    u_ibl_irradiance;
    bgfx_uniform_handle_t    u_ibl_prefilter;
    bgfx_uniform_handle_t    u_ibl_brdf_lut;
    bgfx_uniform_handle_t    u_ibl_params;
    char                     skybox_hdr_path[256];
    bool                     skybox_active;
    float                    skybox_exposure;
    float                    skybox_rotation;
    uint32_t                 viewport_width;
    uint32_t                 viewport_height;
    bool                     postfx_tonemap_active;
    uint16_t                 postfx_output_tex;

    /* Sprite batch for 2D sprite rendering. */
    JceSpriteBatch          *sprite_batch;

    /* Ghost (drag-preview) model state. */
    bool         ghost_active;
    char         ghost_mesh_path[512];
    float        ghost_pos[3];

    /* Hover highlight for drag-drop onto entity. */
    uint32_t     hover_entity_id;

    /* Model / animation cache for skinned entities. */
    ModelCacheEntry  model_cache[MODEL_CACHE_MAX];
};

extern SceneRenderState s_sr;

/* ── Shared helpers ───────────────────────────────────────────────── */

uint16_t scene_view_id(void);
JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos);
JceTexture get_cached_texture(const char *material_path, const char *mesh_path);

/* Model cache helpers (jce_editor_scene_render.cpp). */
ModelCacheEntry *get_cached_model(const char *skeleton_path, uint32_t entity_id);

/* ── Functions from jce_scene_render_draw.cpp ─────────────────────── */

void draw_sky_gradient(void);
void draw_grid(void);
void draw_entities(void);
void draw_ghost_entity(void);
void draw_hover_highlight(void);
void draw_physics_debug(void);

/* ── Functions from jce_scene_render_camera.cpp ───────────────────── */

void orbit_apply(void);

#endif /* JCE_SCENE_RENDER_INTERNAL_H */
