/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Renders sky gradient, grid, and entities to an off-screen framebuffer.
 * The resulting texture is displayed in the ImGui scene panel.
 *
 * Reference: SceneViewWindow.java, EditorRenderOrchestrator.java.
 */

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

/* ── Background color: rgba(30, 30, 40, 255) ──────────────────────── */

#define BG_COLOR_RGBA  0x365FA0FF  /* matches Unity-like sky zenith */

/* ── Vertex type for transient buffers ─────────────────────────────── */

struct PosColorVertex {
    float    x, y, z;
    uint32_t abgr;
};

/* ── Internal state ────────────────────────────────────────────────── */

static struct {
    bool                    initialized;
    JceRenderer            *renderer;
    JceEditorRenderBridge  *bridge;
    JceCamera              *camera;
    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   prog_color;
    bgfx_program_handle_t   prog_grid;

    /* Sky shader resources. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_grid_camera;
    bgfx_uniform_handle_t   u_grid_fade;

    /* Procedural meshes for entity placeholders. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    /* Orbit camera state (Maya-style). */
    jce_vec3                orbit_target;
    float                   orbit_distance;
    float                   orbit_yaw;    /* radians */
    float                   orbit_pitch;  /* radians */

    /* 1×1 white fallback texture for SHADED mode and missing textures. */
    bgfx_texture_handle_t   white_tex;

    /* 8×8 magenta/black checkerboard for missing textures in TEXTURED mode. */
    bgfx_texture_handle_t   checker_tex;

    /* Cached lighting uniform handles for flat-color selection outlines. */
    bgfx_uniform_handle_t   u_light_dir;
    bgfx_uniform_handle_t   u_light_color;

    /* ── Shadow mapping ─────────────────────────────────────────── */
    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;

    /* ── Multi-light environment ────────────────────────────────── */
    JceLightEnv              *light_env;
} s_sr;

/* ── Helpers ───────────────────────────────────────────────────────── */

static uint16_t scene_view_id(void)
{
    if (s_sr.bridge)
        return jce_editor_render_bridge_get_view_id(s_sr.bridge);
    return (uint16_t)JCE_VIEW_EDITOR_SCENE;
}

static JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos)
{
    return jce_editor_scene_asset_cache_get_mesh(mesh_path, world_pos);
}

static JceTexture get_cached_texture(const char *material_path,
                                     const char *mesh_path)
{
    return jce_editor_scene_asset_cache_get_texture(material_path, mesh_path);
}

/* ── Sky gradient (smooth sky dome — no visible edges) ───────────────── */

static void draw_sky_gradient(void)
{
    /*
     * Fullscreen NDC quad sky — reference: EditorOverlayRenderer.java.
     *
     * A fullscreen quad is placed at depth 1.0 (far plane) in clip space.
     * The fragment shader reconstructs the world-space view ray direction
     * for each pixel using bgfx's built-in u_invViewProj, then blends
     * three colours (top / horizon / ground) based on direction.y.
     *
     * This avoids all geometry-in-world-space artefacts (tilted planes,
     * visible rim circles, corner artefacts) that plagued the dome/cone
     * approaches, and matches the Java reference sky exactly.
     *
     * Sky color palette (matching uSkyColorTop / uSkyColorHorizon /
     * uGroundColor from the Java reference):
     *   top     (0.40, 0.60, 0.90) — cornflower blue
     *   horizon (0.70, 0.80, 0.95) — pale sky
     *   ground  (0.25, 0.25, 0.30) — dark warm grey
     */
    if (!BGFX_HANDLE_IS_VALID(s_sr.prog_sky)) return;

    /* Fullscreen quad: 4 NDC corners, 2 triangles. */
    struct SkyVertex { float x, y, z; };

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.sky_layout, 4, &tib, 6, false))
        return;

    SkyVertex *v  = (SkyVertex *)tvb.data;
    uint16_t  *ix = (uint16_t  *)tib.data;

    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };

    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    /* Sky gradient colors: top / horizon / ground. */
    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,   /* [0] top     — deeper blue */
        0.65f, 0.78f, 0.92f, 1.0f,   /* [1] horizon — soft pale   */
        0.22f, 0.22f, 0.28f, 1.0f,   /* [2] ground  — dark grey   */
    };
    bgfx_set_uniform(s_sr.u_sky_colors, sky_colors, 3);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);

    /* No depth test / depth write: the sky always fills the background
     * and is overdrawn by grid and entities submitted afterwards. */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(scene_view_id(), s_sr.prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

static void draw_grid(void)
{
    if (!BGFX_HANDLE_IS_VALID(s_sr.prog_grid)) return;

    struct GridVertex { float x, y, z; };
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &s_sr.sky_layout, 4, &tib, 6, false))
        return;

    GridVertex *v = (GridVertex *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;
    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };
    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    float fade_near = fmaxf(16.0f, s_sr.orbit_distance * 3.0f);
    float fade_far  = fmaxf(fade_near + 40.0f, s_sr.orbit_distance * 24.0f);
    float grid_camera[4] = { cam_pos.x, cam_pos.y, cam_pos.z, 0.0f };
    float grid_fade[4] = { fade_near, fade_far, 10.0f, 1.0f };

    bgfx_set_uniform(s_sr.u_grid_camera, grid_camera, 1);
    bgfx_set_uniform(s_sr.u_grid_fade, grid_fade, 1);
    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);

    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_MSAA
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                            BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(scene_view_id(), s_sr.prog_grid, 0, BGFX_DISCARD_ALL);
}

/* ── Entity Rendering (auto-detect components) ─────────────────────── */

/* Build a model matrix for entity i from its transform components.
   Returns false if the entity has no transform. */
static bool build_entity_model(JceEntityInfo *ent, jce_mat4 *out_model,
                                JceMesh **out_mesh,
                                const char **out_material_path)
{
    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &comp_count);
    if (!comps) return false;

    const float *pos   = NULL;
    const float *rot   = NULL;
    const float *scale = NULL;
    bool has_mesh  = false;
    const char *mesh_path = NULL;
    const char *mat_path  = NULL;
    int  mesh_shape = 0; /* JCE_MESH_SHAPE_CUBE */

    for (int c = 0; c < comp_count; c++) {
        switch (comps[c].type) {
        case JCE_COMP_TRANSFORM:
            pos   = comps[c].data.transform.pos;
            rot   = comps[c].data.transform.rot;
            scale = comps[c].data.transform.scale;
            break;
        case JCE_COMP_MESH_RENDERER:
            has_mesh  = true;
            mesh_path = comps[c].data.mesh_renderer.mesh_path;
            mat_path  = comps[c].data.mesh_renderer.material_path;
            mesh_shape = comps[c].data.mesh_renderer.mesh_shape;
            break;
        default: break;
        }
    }

    if (!pos) return false;

    float sx = (scale && scale[0] != 0.0f) ? scale[0] : 1.0f;
    float sy = (scale && scale[1] != 0.0f) ? scale[1] : 1.0f;
    float sz = (scale && scale[2] != 0.0f) ? scale[2] : 1.0f;

    if (rot && (rot[0] != 0.0f || rot[1] != 0.0f || rot[2] != 0.0f)) {
        *out_model = jce_m4_from_trs(
            jce_v3(pos[0], pos[1], pos[2]),
            jce_q_from_euler(rot[0] * JCE_DEG2RAD, rot[1] * JCE_DEG2RAD, rot[2] * JCE_DEG2RAD),
            jce_v3(sx, sy, sz));
    } else {
        *out_model = jce_m4_identity();
        out_model->raw[0][0] = sx;
        out_model->raw[1][1] = sy;
        out_model->raw[2][2] = sz;
        out_model->raw[3][0] = pos[0];
        out_model->raw[3][1] = pos[1];
        out_model->raw[3][2] = pos[2];
    }

    if (out_mesh) {
        *out_mesh = NULL;
        if (has_mesh) {
            if (mesh_path && mesh_path[0] != '\0') {
                /* Queue background decode and return placeholder until ready. */
                *out_mesh = get_cached_mesh(mesh_path, pos);
            }
            /* If no file mesh loaded, fall back to procedural shape. */
            if (!*out_mesh) {
                switch (mesh_shape) {
                default: /* fall through */
                case JCE_MESH_SHAPE_CUBE:     *out_mesh = s_sr.cube_mesh;     break;
                case JCE_MESH_SHAPE_SPHERE:   *out_mesh = s_sr.sphere_mesh;   break;
                case JCE_MESH_SHAPE_PLANE:    *out_mesh = s_sr.plane_mesh;    break;
                case JCE_MESH_SHAPE_CAPSULE:  *out_mesh = s_sr.capsule_mesh;  break;
                case JCE_MESH_SHAPE_CYLINDER: *out_mesh = s_sr.cylinder_mesh; break;
                }
            }
        }
        /* has_mesh == false: entity has no MeshRenderer — no visual geometry. */
    }

    if (out_material_path)
        *out_material_path = mat_path;

    return true;
}

/* Orange wireframe overlay for selected entities (flat color, no lighting). */
static void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    /*
     * Flat-color mode: set u_lightDir.w = -1.0 to tell fs_mesh to output
     * u_lightColor.xyz directly, bypassing diffuse lighting.
     * This produces a consistent orange regardless of face normal.
     * Reference: EditorOverlayRenderer.java — SELECTION_COLOR (1.0, 0.75, 0.0).
     */
    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };  /* w<0 = flat mode */
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };    /* pure orange */

    /* Bind white texture so texel sampling is neutral. */
    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    bgfx_uniform_handle_t su = { uh.idx };

    for (int i = 0; i < sel_count; i++) {
        JceEntityInfo *ent = jce_state_get_entity(sel[i]);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);

        /* Set flat-color uniforms + texture per draw call (bgfx consumes per submit). */
        bgfx_set_uniform(s_sr.u_light_dir,   flat_dir,   1);
        bgfx_set_uniform(s_sr.u_light_color, flat_color,  1);
        bgfx_set_texture(0, su, s_sr.white_tex, UINT32_MAX);

        jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer, scene_view_id());
    }

    /* Restore normal lighting. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Shadow map pass ────────────────────────────────────────────────── */

#define SHADOW_MAP_SIZE  2048
#define SHADOW_ORTHO_SIZE 20.0f

static void compute_shadow_vp(const jce_vec3 *light_dir, float shadow_vp[16])
{
    /* Build an orthographic "camera" looking along the light direction. */
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 30.0f); /* push back from origin */

    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);

    /* Look-at view matrix: light position → center. */
    jce_mat4 view = jce_m4_look_at(light_pos, center, up);

    /* Orthographic projection enclosing the scene. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float S = SHADOW_ORTHO_SIZE;
    jce_mat4 proj = jce_m4_ortho(-S, S, -S, S, 0.1f, 80.0f,
                                  caps->homogeneousDepth);

    jce_mat4 vp = jce_m4_multiply(&proj, &view);
    memcpy(shadow_vp, vp.raw, 16 * sizeof(float));
}

static void draw_shadow_pass(void)
{
    if (!s_sr.shadow_valid) return;

    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(s_sr.renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    JceDirLight sun = jce_dir_light_default();

    /* Compute light-space view-projection. */
    float shadow_vp[16];
    compute_shadow_vp(&sun.direction, shadow_vp);

    /* Configure shadow view. */
    const uint16_t shadow_view = (uint16_t)JCE_VIEW_SHADOW_0;
    bgfx_set_view_rect(shadow_view, 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    bgfx_set_view_frame_buffer(shadow_view, s_sr.shadow_fbo);
    bgfx_set_view_clear(shadow_view,
                        BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

    /* Set the VP matrix for the shadow view. */
    float identity[16];
    memset(identity, 0, sizeof(identity));
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    bgfx_set_view_transform(shadow_view, identity, shadow_vp);

    /* Store shadow VP for the PBR shader. */
    bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);

    /* Submit all mesh entities to the shadow depth view. */
    int count = jce_state_get_entity_count();
    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(mesh, s_sr.renderer, shadow_view);
    }
}

static void draw_entities(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    /* Render shadow depth pass before main scene. */
    draw_shadow_pass();

    /* ── Gather lights from entity components ─────────────────── */
    if (s_sr.light_env) {
        jce_light_env_clear(s_sr.light_env);
        jce_light_env_set_ambient(s_sr.light_env,
                                  jce_v3(1.0f, 1.0f, 1.0f), 0.15f);

        bool has_any_light = false;
        for (int i = 0; i < count; i++) {
            JceEntityInfo *ent = jce_state_get_entity_by_index(i);
            if (!ent || !ent->enabled) continue;

            int comp_count = 0;
            JceComponentInfo *comps = jce_state_get_entity_components(ent->id,
                                                                      &comp_count);
            for (int c = 0; c < comp_count; c++) {
                if (comps[c].type != JCE_COMP_LIGHT) continue;

                const auto &ld = comps[c].data.light;
                jce_vec3 color = jce_v3(ld.color[0], ld.color[1], ld.color[2]);
                float intensity = ld.intensity;

                /* Get transform for light position/direction. */
                JceComponentInfo *xf = NULL;
                for (int t = 0; t < comp_count; t++) {
                    if (comps[t].type == JCE_COMP_TRANSFORM) { xf = &comps[t]; break; }
                }

                if (ld.type == 0) {
                    /* Directional light. */
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = color;
                    dl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    if (xf) {
                        /* Use negative Z as direction (forward). */
                        float yaw_rad = xf->data.transform.rot[1] * JCE_DEG2RAD;
                        float pitch_rad = xf->data.transform.rot[0] * JCE_DEG2RAD;
                        dl.direction = jce_v3(
                            -sinf(yaw_rad),
                             sinf(pitch_rad),
                            -cosf(yaw_rad));
                    } else {
                        dl.direction = jce_v3(0.5f, 1.0f, 0.3f);
                    }
                    jce_light_env_add_dir_light(s_sr.light_env, &dl);
                    has_any_light = true;
                } else if (ld.type == 1) {
                    /* Point light. */
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = color;
                    pl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    pl.radius = 10.0f;
                    if (xf) {
                        pl.position = jce_v3(xf->data.transform.pos[0],
                                             xf->data.transform.pos[1],
                                             xf->data.transform.pos[2]);
                    }
                    jce_light_env_add_point_light(s_sr.light_env, &pl);
                    has_any_light = true;
                }
            }
        }

        /* Fallback: always ensure at least one directional light. */
        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3(0.5f, 1.0f, 0.3f);
            dl.color = jce_v3(1.0f, 1.0f, 1.0f);
            dl.intensity = 1.0f;
            jce_light_env_add_dir_light(s_sr.light_env, &dl);
        }

        /* Set camera position for PBR specular. */
        jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
        jce_light_env_set_camera_pos(s_sr.light_env, cam_pos);

        /* Upload all light uniforms. */
        jce_light_env_apply(s_sr.light_env, s_sr.renderer);

        /* Also set legacy u_lightDir / u_lightColor for the basic mesh shader. */
        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    } else {
        /* Fallback to legacy single-light. */
        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    }

    /* Apply render mode. */
    JceSceneViewMode view_mode = jce_state_get_view_mode();
    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, true);

    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
        if (!mesh) continue;

        bgfx_set_transform(model.raw[0], 1);

        if (view_mode != JCE_VIEW_WIREFRAME) {
            /* Find the mesh renderer component for PBR data. */
            int cc = 0;
            JceComponentInfo *cs = jce_state_get_entity_components(ent->id, &cc);
            JceComponentInfo *mr_comp = NULL;
            for (int c = 0; c < cc; c++) {
                if (cs[c].type == JCE_COMP_MESH_RENDERER) {
                    mr_comp = &cs[c];
                    break;
                }
            }

            /* Check if entity has explicit PBR texture data configured. */
            bool has_pbr_textures = mr_comp
                && (mr_comp->data.mesh_renderer.albedo_tex[0]
                    || mr_comp->data.mesh_renderer.mr_tex[0]
                    || mr_comp->data.mesh_renderer.normal_tex[0]
                    || mr_comp->data.mesh_renderer.ao_tex[0]
                    || mr_comp->data.mesh_renderer.emissive_tex[0]);

            if (view_mode == JCE_VIEW_TEXTURED && has_pbr_textures) {
                /* Build a JcePbrMaterial from the component's inline PBR data. */
                JcePbrMaterial pbr = jce_pbr_material_default();
                /* Only override base_color if it looks explicitly set (alpha > 0). */
                if (mr_comp->data.mesh_renderer.base_color[3] > 0.0f) {
                    pbr.base_color_factor[0] = mr_comp->data.mesh_renderer.base_color[0];
                    pbr.base_color_factor[1] = mr_comp->data.mesh_renderer.base_color[1];
                    pbr.base_color_factor[2] = mr_comp->data.mesh_renderer.base_color[2];
                    pbr.base_color_factor[3] = mr_comp->data.mesh_renderer.base_color[3];
                }
                pbr.metallic_factor      = mr_comp->data.mesh_renderer.metallic;
                pbr.roughness_factor     = mr_comp->data.mesh_renderer.roughness;
                pbr.emissive_factor[0]   = mr_comp->data.mesh_renderer.emissive[0];
                pbr.emissive_factor[1]   = mr_comp->data.mesh_renderer.emissive[1];
                pbr.emissive_factor[2]   = mr_comp->data.mesh_renderer.emissive[2];
                pbr.normal_scale         = mr_comp->data.mesh_renderer.normal_scale;
                pbr.ao_strength          = mr_comp->data.mesh_renderer.ao_strength;
                pbr.alpha_mode           = (JceAlphaMode)mr_comp->data.mesh_renderer.alpha_mode;
                pbr.alpha_cutoff         = mr_comp->data.mesh_renderer.alpha_cutoff;
                pbr.double_sided         = mr_comp->data.mesh_renderer.double_sided;

                /* Try to load cached textures for each PBR slot. */
                if (mr_comp->data.mesh_renderer.albedo_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.albedo_tex, NULL);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* Fallback: use legacy mat_path/mesh_path texture search for albedo. */
                if (!jce_texture_valid(pbr.albedo_map)) {
                    const char *mp = mr_comp->data.mesh_renderer.mesh_path;
                    JceTexture t = get_cached_texture(mat_path, mp);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                if (mr_comp->data.mesh_renderer.mr_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.mr_tex, NULL);
                    if (jce_texture_valid(t)) pbr.metallic_roughness_map = t;
                }
                if (mr_comp->data.mesh_renderer.normal_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.normal_tex, NULL);
                    if (jce_texture_valid(t)) pbr.normal_map = t;
                }
                if (mr_comp->data.mesh_renderer.ao_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.ao_tex, NULL);
                    if (jce_texture_valid(t)) pbr.ao_map = t;
                }
                if (mr_comp->data.mesh_renderer.emissive_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.emissive_tex, NULL);
                    if (jce_texture_valid(t)) pbr.emissive_map = t;
                }

                jce_pbr_material_bind(&pbr, s_sr.renderer, scene_view_id());

                /* Bind shadow map to texture stage 5 for PBR shader. */
                if (s_sr.shadow_valid) {
                    bgfx_set_texture(5, s_sr.u_shadowMap, s_sr.shadow_tex, UINT32_MAX);
                    float shadow_vp[16];
                    jce_vec3 shadow_dir = jce_v3(0.5f, 1.0f, 0.3f); /* default */
                    compute_shadow_vp(&shadow_dir, shadow_vp);
                    bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);
                }

                jce_mesh_submit_pbr(mesh, s_sr.renderer, scene_view_id());
                continue; /* skip the default submit below */
            }

            /* SHADED or TEXTURED fallback: use basic mesh program with single texture. */
            bgfx_texture_handle_t bind_tex = s_sr.white_tex;

            if (view_mode == JCE_VIEW_TEXTURED) {
                const char *mp = mr_comp ? mr_comp->data.mesh_renderer.mesh_path : NULL;

                JceTexture tex = get_cached_texture(mat_path, mp);
                if (tex.idx != UINT16_MAX) {
                    bind_tex.idx = tex.idx;
                } else {
                    /* Graceful degradation: use magenta/black checkerboard
                     * so missing textures are visually obvious. */
                    bind_tex = s_sr.checker_tex;
                    /* Only warn for entities that have a non-empty mesh renderer
                     * path; container/group nodes with empty mat+mesh are silent. */
                    bool has_mat  = mat_path && mat_path[0] != '\0';
                    bool has_mesh = mp       && mp[0]       != '\0';
                    if (has_mat || has_mesh) {
                        if (jce_editor_scene_asset_cache_take_texture_warning(mat_path, mp)) {
                            LOG_WARN(LOG_TAG,
                                     "TEXTURED entity '%s': NO texture (mat='%s' mesh='%s')",
                                     ent->name,
                                     has_mat  ? mat_path : "",
                                     has_mesh ? mp       : "");
                        }
                    }
                }
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
        }

        jce_mesh_submit(mesh, s_sr.renderer, scene_view_id());
    }

    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, false);

    /* Draw orange wireframe outlines for selected entities. */
    draw_selection_outlines();
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_scene_render_init(JceRenderer *renderer,
                                  const PakArchive *pak,
                                  JceAssetManager *assets)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    jce_editor_scene_asset_cache_init(assets);
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.checker_tex.idx = UINT16_MAX;
    s_sr.renderer = renderer;
    s_sr.bridge = jce_editor_render_bridge_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    /* Create the editor orbit camera. */
    JceCameraDesc cam_desc;
    memset(&cam_desc, 0, sizeof(cam_desc));
    cam_desc.mode       = JCE_CAMERA_PERSPECTIVE;
    cam_desc.position   = jce_v3(8.0f, 6.0f, 8.0f);
    cam_desc.target     = jce_v3(0.0f, 0.0f, 0.0f);
    cam_desc.up         = jce_v3(0.0f, 1.0f, 0.0f);
    cam_desc.fov_deg    = 45.0f;
    cam_desc.near_plane = 0.1f;
    cam_desc.far_plane  = 500.0f;

    s_sr.camera = jce_camera_create(&cam_desc);
    if (!s_sr.camera) {
        LOG_WARN(LOG_TAG, "failed to create editor camera");
        if (s_sr.bridge) {
            jce_editor_render_bridge_destroy(s_sr.bridge);
            s_sr.bridge = NULL;
        }
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    /* Initialize orbit state from the camera's initial position/target. */
    s_sr.orbit_target = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    jce_vec3 diff = jce_v3_sub(cam_pos, s_sr.orbit_target);
    s_sr.orbit_distance = jce_v3_len(diff);
    s_sr.orbit_yaw   = atan2f(diff.x, -diff.z);
    s_sr.orbit_pitch = asinf(diff.y / s_sr.orbit_distance);

    /* Pos + color vertex layout for transient buffers (grid, sky). */
    bgfx_vertex_layout_begin(&s_sr.layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&s_sr.layout);

    /* Cache the color shader program handle. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    s_sr.prog_color.idx = sh.idx;

    /* Load sky/grid shader programs from the PAK archive. */
    JceShaderHandle sky_sh = shader_load_program(pak, "sky");
    s_sr.prog_sky.idx = sky_sh.idx;
    if (sky_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "sky shader not found in PAK — sky will be skipped");
    JceShaderHandle grid_sh = shader_load_program(pak, "grid");
    s_sr.prog_grid.idx = grid_sh.idx;
    if (grid_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "grid shader not found in PAK — grid will be skipped");

    /* Position-only vertex layout for the fullscreen sky quad. */
    bgfx_vertex_layout_begin(&s_sr.sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&s_sr.sky_layout);

    /* Uniforms for sky gradient and fullscreen grid. */
    s_sr.u_sky_colors = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);
    s_sr.u_grid_camera = bgfx_create_uniform("u_grid_camera",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_grid_fade = bgfx_create_uniform("u_grid_fade",
                                           BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Cache lighting uniform handles for flat-color selection outlines.
     * bgfx_create_uniform with the same name returns a reference to the
     * same uniform, so these share handles with the renderer's copies. */
    s_sr.u_light_dir   = bgfx_create_uniform("u_lightDir",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = bgfx_create_uniform("u_lightColor",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Procedural meshes. */
    s_sr.cube_mesh     = jce_mesh_create_cube(1.0f);
    s_sr.plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    s_sr.sphere_mesh   = jce_mesh_create_sphere(0.5f);
    s_sr.capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    s_sr.cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    /* 1×1 white fallback texture for SHADED mode. */
    {
        uint32_t white = 0xFFFFFFFF;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        s_sr.white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                  BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 8×8 magenta/black checkerboard for missing textures (TEXTURED mode). */
    {
        const uint32_t M = 0xFFFF00FF; /* magenta (ABGR) */
        const uint32_t K = 0xFF000000; /* black   (ABGR) */
        uint32_t checker[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                checker[y * 8 + x] = ((x ^ y) & 1) ? K : M;
        const bgfx_memory_t *cmem = bgfx_copy(checker, sizeof(checker));
        s_sr.checker_tex = bgfx_create_texture_2d(8, 8, false, 1,
                                                    BGFX_TEXTURE_FORMAT_RGBA8,
                                                    BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                                                    cmem);
    }

    /* ── Shadow map resources ──────────────────────────────────────── */
    {
        const uint16_t shadow_size = 2048;
        s_sr.shadow_tex = bgfx_create_texture_2d(
            shadow_size, shadow_size, false, 1,
            BGFX_TEXTURE_FORMAT_D16,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            NULL);
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, s_sr.shadow_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
        s_sr.shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        s_sr.u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                                BGFX_UNIFORM_TYPE_SAMPLER, 1);
        s_sr.u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                                BGFX_UNIFORM_TYPE_MAT4, 1);
        s_sr.shadow_valid = BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo);
        if (s_sr.shadow_valid)
            LOG_INFO(LOG_TAG, "shadow map created (%dx%d)", shadow_size, shadow_size);
    }

    /* ── Multi-light environment ──────────────────────────────────── */
    s_sr.light_env = jce_light_env_create();

    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (FBO pipeline)");
    return true;
}

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

    jce_editor_scene_asset_cache_shutdown();

    if (s_sr.bridge) {
        jce_editor_render_bridge_destroy(s_sr.bridge);
        s_sr.bridge = NULL;
    }

    if (s_sr.camera)     { jce_camera_destroy(s_sr.camera);   s_sr.camera = NULL; }
    if (s_sr.cube_mesh)     { jce_mesh_destroy(s_sr.cube_mesh);     s_sr.cube_mesh = NULL; }
    if (s_sr.plane_mesh)    { jce_mesh_destroy(s_sr.plane_mesh);    s_sr.plane_mesh = NULL; }
    if (s_sr.sphere_mesh)   { jce_mesh_destroy(s_sr.sphere_mesh);   s_sr.sphere_mesh = NULL; }
    if (s_sr.capsule_mesh)  { jce_mesh_destroy(s_sr.capsule_mesh);  s_sr.capsule_mesh = NULL; }
    if (s_sr.cylinder_mesh) { jce_mesh_destroy(s_sr.cylinder_mesh); s_sr.cylinder_mesh = NULL; }

    /* Destroy white fallback texture. */
    if (BGFX_HANDLE_IS_VALID(s_sr.white_tex))
        bgfx_destroy_texture(s_sr.white_tex);
    if (BGFX_HANDLE_IS_VALID(s_sr.checker_tex))
        bgfx_destroy_texture(s_sr.checker_tex);

    /* Destroy sky/grid shader resources owned by the scene renderer. */
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_sky))
        bgfx_destroy_program(s_sr.prog_sky);
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_grid))
        bgfx_destroy_program(s_sr.prog_grid);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_colors))
        bgfx_destroy_uniform(s_sr.u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_camera))
        bgfx_destroy_uniform(s_sr.u_grid_camera);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_fade))
        bgfx_destroy_uniform(s_sr.u_grid_fade);

    /* Destroy shadow map resources. */
    if (BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo))
        bgfx_destroy_frame_buffer(s_sr.shadow_fbo);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowMap))
        bgfx_destroy_uniform(s_sr.u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowVP))
        bgfx_destroy_uniform(s_sr.u_shadowVP);

    /* Destroy multi-light environment. */
    if (s_sr.light_env) {
        jce_light_env_destroy(s_sr.light_env);
        s_sr.light_env = NULL;
    }

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    /* Configure the scene view to render into the FBO. */
    const bgfx_caps_t *caps = bgfx_get_caps();
    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, caps->homogeneousDepth);

    JceSceneViewMode view_mode = jce_state_get_view_mode();

    uint32_t clear_color = (view_mode == JCE_VIEW_WIREFRAME)
        ? 0x373737FF
        : BG_COLOR_RGBA;

    if (!jce_editor_render_bridge_prepare(
            s_sr.bridge,
            width,
            height,
            view.raw[0],
            proj.raw[0],
            clear_color,
            "EditorScene")) {
        return;
    }

    /* Main-thread GPU finalize for background-decoded meshes/textures. */
    jce_editor_scene_asset_cache_finalize();

    /* Draw sky gradient (behind everything) — skip in wireframe mode. */
    if (view_mode != JCE_VIEW_WIREFRAME)
        draw_sky_gradient();

    /* Draw grid. */
    if (jce_state_get_show_grid()) {
        draw_grid();
    }

    /* Draw entities. */
    draw_entities();
}

uint16_t jce_editor_scene_render_get_texture(void)
{
    if (!s_sr.initialized || !s_sr.bridge)
        return UINT16_MAX;
    return jce_editor_render_bridge_get_color_texture(s_sr.bridge);
}

JceCamera *jce_editor_scene_get_camera(void)
{
    return s_sr.camera;
}

bool jce_editor_scene_get_camera_matrices(float *out_view16,
                                           float *out_proj16,
                                           float *out_eye3,
                                           float viewport_w,
                                           float viewport_h)
{
    if (!s_sr.initialized || !s_sr.camera) return false;

    float aspect = (viewport_h > 0.0f) ? viewport_w / viewport_h : 1.0f;
    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, false);
    memcpy(out_view16, JCE_M4_PTR(view), 16 * sizeof(float));
    memcpy(out_proj16, JCE_M4_PTR(proj), 16 * sizeof(float));

    jce_vec3 pos = jce_camera_get_position(s_sr.camera);
    out_eye3[0] = pos.x;
    out_eye3[1] = pos.y;
    out_eye3[2] = pos.z;
    return true;
}

/* ── Orbit camera helpers ──────────────────────────────────────────── */

#define ORBIT_PITCH_MAX  (89.0f * JCE_DEG2RAD)
#define ORBIT_DIST_MIN   0.5f
#define ORBIT_DIST_MAX   500.0f

static void orbit_apply(void)
{
    if (!s_sr.camera) return;

    /* Compute camera position on sphere around orbit_target. */
    float y = s_sr.orbit_pitch;
    float x = s_sr.orbit_yaw;
    float d = s_sr.orbit_distance;

    jce_vec3 pos;
    pos.x = s_sr.orbit_target.x + d * sinf(x) * cosf(y);
    pos.y = s_sr.orbit_target.y + d * sinf(y);
    pos.z = s_sr.orbit_target.z - d * cosf(x) * cosf(y);

    jce_camera_set_position(s_sr.camera, pos);
    jce_camera_look_at(s_sr.camera, s_sr.orbit_target);
}

void jce_editor_scene_camera_orbit(float dyaw, float dpitch)
{
    if (!s_sr.initialized) return;
    s_sr.orbit_yaw   += dyaw;
    s_sr.orbit_pitch += dpitch;
    if (s_sr.orbit_pitch >  ORBIT_PITCH_MAX) s_sr.orbit_pitch =  ORBIT_PITCH_MAX;
    if (s_sr.orbit_pitch < -ORBIT_PITCH_MAX) s_sr.orbit_pitch = -ORBIT_PITCH_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_pan(float dx, float dy)
{
    if (!s_sr.initialized || !s_sr.camera) return;
    jce_vec3 right = jce_camera_get_right(s_sr.camera);
    jce_vec3 up    = jce_camera_get_up(s_sr.camera);

    /* Scale by distance so panning feels natural at all zoom levels. */
    float scale = s_sr.orbit_distance * 0.002f;
    jce_vec3 offset = jce_v3_add(
        jce_v3_scale(right, -dx * scale),
        jce_v3_scale(up,     dy * scale));

    s_sr.orbit_target = jce_v3_add(s_sr.orbit_target, offset);
    orbit_apply();
}

void jce_editor_scene_camera_zoom(float delta)
{
    if (!s_sr.initialized) return;
    s_sr.orbit_distance -= delta * s_sr.orbit_distance * 0.1f;
    if (s_sr.orbit_distance < ORBIT_DIST_MIN) s_sr.orbit_distance = ORBIT_DIST_MIN;
    if (s_sr.orbit_distance > ORBIT_DIST_MAX) s_sr.orbit_distance = ORBIT_DIST_MAX;
    orbit_apply();
}

void jce_editor_scene_camera_get_target(float *out3)
{
    if (out3) {
        out3[0] = s_sr.orbit_target.x;
        out3[1] = s_sr.orbit_target.y;
        out3[2] = s_sr.orbit_target.z;
    }
}

void jce_editor_scene_camera_set_target(float x, float y, float z)
{
    s_sr.orbit_target = jce_v3(x, y, z);
    if (s_sr.initialized) orbit_apply();
}

void jce_editor_scene_camera_snap_view(JceCamPresetView preset)
{
    if (!s_sr.initialized) return;

    switch (preset) {
    case JCE_CAM_VIEW_FRONT:   s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_BACK:    s_sr.orbit_yaw = JCE_PI;         s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_LEFT:    s_sr.orbit_yaw = -JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_RIGHT:   s_sr.orbit_yaw =  JCE_PI * 0.5f; s_sr.orbit_pitch = 0;   break;
    case JCE_CAM_VIEW_TOP:     s_sr.orbit_yaw = 0;              s_sr.orbit_pitch =  ORBIT_PITCH_MAX; break;
    case JCE_CAM_VIEW_BOTTOM:  s_sr.orbit_yaw = 0;              s_sr.orbit_pitch = -ORBIT_PITCH_MAX; break;
    }
    orbit_apply();
}

void jce_editor_scene_camera_reset(void)
{
    if (!s_sr.initialized) return;
    /* Restore default orbit: position (8,6,8) looking at (0,0,0). */
    s_sr.orbit_target   = jce_v3(0.0f, 0.0f, 0.0f);
    s_sr.orbit_distance = sqrtf(8.0f*8.0f + 6.0f*6.0f + 8.0f*8.0f); /* ~12.2 */
    s_sr.orbit_yaw      = atan2f(8.0f, -8.0f); /* 135° → F/T/R quadrant (+X,+Y,+Z) */
    s_sr.orbit_pitch    = asinf(6.0f / s_sr.orbit_distance);
    orbit_apply();
}

void jce_editor_scene_set_scene_dir(const char *dir)
{
    jce_editor_scene_asset_cache_set_scene_dir(dir);
}