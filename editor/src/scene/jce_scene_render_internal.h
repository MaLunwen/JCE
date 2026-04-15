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
#include <jce/graphics/jce_editor_render_bridge.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/graphics/jce_lighting_system.h>
#include <jce/graphics/jce_material.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_views.h>
#include <jce/core/pak_loader.h>
}

#define LOG_TAG "scene_render"

/* ── Background color ─────────────────────────────────────────────── */

#define BG_COLOR_RGBA  0x365FA0FF

/* ── Vertex type for transient buffers ────────────────────────────── */

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

/* ── Internal state (single instance, defined in jce_editor_scene_render.cpp) */

struct SceneRenderState {
    bool                    initialized;
    JceRenderer            *renderer;
    JceEditorRenderBridge  *bridge;
    JceCamera              *camera;
    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   prog_color;
    bgfx_program_handle_t   prog_grid;

    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
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

    bgfx_texture_handle_t   white_tex;
    bgfx_texture_handle_t   checker_tex;

    bgfx_uniform_handle_t   u_light_dir;
    bgfx_uniform_handle_t   u_light_color;

    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;

    JceLightEnv              *light_env;

    /* Ghost (drag-preview) model state. */
    bool         ghost_active;
    char         ghost_mesh_path[512];
    float        ghost_pos[3];

    /* Hover highlight for drag-drop onto entity. */
    uint32_t     hover_entity_id;
};

extern SceneRenderState s_sr;

/* ── Shared helpers ───────────────────────────────────────────────── */

uint16_t scene_view_id(void);
JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos);
JceTexture get_cached_texture(const char *material_path, const char *mesh_path);

/* ── Functions from jce_scene_render_draw.cpp ─────────────────────── */

void draw_sky_gradient(void);
void draw_grid(void);
void draw_entities(void);
void draw_ghost_entity(void);
void draw_hover_highlight(void);

/* ── Functions from jce_scene_render_camera.cpp ───────────────────── */

void orbit_apply(void);

#endif /* JCE_SCENE_RENDER_INTERNAL_H */
