/*
 * jce_scene_render_draw.cpp  Editor overlay passes.
 *
 * Phase B refactor: sky, shadows, and entity rendering live in the engine
 * `JceSceneRenderer`. This file now only contains overlay passes drawn on
 * top of the engine output: grid, selection outlines, physics debug,
 * ghost, hover.
 */

#include "jce_scene_render_internal.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

/* ── Animation timer reset (kept for play.cpp compatibility) ─────── */

void jce_editor_scene_reset_anim_timer(void)
{
    s_sr.anim_last_ticks = 0;
}

/* ── Local entity model builder (overlay-only) ───────────────────── */

/* Builds a TRS model matrix from the entity's Transform component and
 * resolves its mesh from the editor asset cache.  Procedural-shape
 * fallback meshes are not provided here: overlays simply skip entities
 * whose mesh isn't loadable from a file path. */
static bool build_overlay_entity_model(uint32_t entity_id,
                                        jce_mat4 *out_model,
                                        JceMesh **out_mesh)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene || entity_id == 0) return false;
    JceEntity e = (JceEntity)entity_id;

    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t) return false;

    float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
    float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
    float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
    *out_model = jce_m4_from_trs(t->position, t->rotation,
                                  jce_v3(sx, sy, sz));

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr && mr->mesh_path[0] != '\0') {
                float wp[3] = { t->position.x, t->position.y, t->position.z };
                *out_mesh = get_cached_mesh(mr->mesh_path, wp);
            }
        }
    }

    return true;
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

void draw_grid(void)
{
    if (!jce_program_valid(s_sr.prog_grid)) return;

    /* Position-only fullscreen quad in NDC. */
    JceVertexLayout layout;
    jce_vertex_layout_begin(&layout);
    jce_vertex_layout_add(&layout, JCE_ATTRIB_POSITION, 3,
                          JCE_ATTRIB_TYPE_FLOAT, false, false);
    jce_vertex_layout_end(&layout);

    struct GridVertex { float x, y, z; };
    JceTransientVertexBuffer tvb;
    JceTransientIndexBuffer  tib;
    if (!jce_alloc_transient_buffers(&tvb, &layout, 4, &tib, 6, false))
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

    jce_uniform_set(s_sr.u_grid_camera, grid_camera, 1);
    jce_uniform_set(s_sr.u_grid_fade, grid_fade, 1);
    jce_set_transient_vertex_buffer(0, &tvb, 0, 4);
    jce_set_transient_index_buffer(&tib, 0, 6);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_MSAA
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA);
    jce_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    jce_set_transform(identity.raw[0], 1);
    jce_submit(scene_view_id(), s_sr.prog_grid, 0, JCE_DISCARD_ALL);
}

/* ── Selection outlines ───────────────────────────────────────────── */

void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);

    for (int i = 0; i < sel_count; i++) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!build_overlay_entity_model(id, &model, &mesh)) continue;
        if (!mesh) continue;

        jce_set_transform(model.raw[0], 1);

        jce_uniform_set(s_sr.u_light_dir,   flat_dir,   1);
        jce_uniform_set(s_sr.u_light_color, flat_color, 1);
        jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

        jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer, scene_view_id());
    }

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Physics debug visualization ──────────────────────────────────── */

void draw_physics_debug(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    const uint32_t col_box     = 0xFF00FF00; /* green */
    const uint32_t col_sphere  = 0xFF00FFFF; /* cyan */
    const uint32_t col_capsule = 0xFFFFFF00; /* yellow */

    for (int i = 0; i < count; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        jce_vec3 center = t->position;
        jce_quat q = t->rotation;

        if (jce_scene_has_box_collider(scene, e)) {
            float sx = (t->scale.x > 0) ? t->scale.x : 1.0f;
            float sy = (t->scale.y > 0) ? t->scale.y : 1.0f;
            float sz = (t->scale.z > 0) ? t->scale.z : 1.0f;
            jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
            jce_debug_draw_box(center, half, q, col_box);
        }
        if (jce_scene_has_sphere_collider(scene, e)) {
            float r = (t->scale.x > 0) ? t->scale.x * 0.5f : 0.5f;
            jce_debug_draw_sphere(center, r, col_sphere);
        }
        if (jce_scene_has_character_controller(scene, e)) {
            float radius = 0.3f;
            float half_h = 0.6f;
            jce_debug_draw_capsule(center, radius, half_h, q, col_capsule);
        }
    }

    jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* ── Ghost (drag-preview) model rendering ─────────────────────────── */

void draw_ghost_entity(void)
{
    if (!s_sr.ghost_active || s_sr.ghost_mesh_path[0] == '\0')
        return;

    JceMesh *mesh = get_cached_mesh(s_sr.ghost_mesh_path, s_sr.ghost_pos);
    if (!mesh) return;

    jce_mat4 model = jce_m4_identity();
    model.raw[3][0] = s_sr.ghost_pos[0];
    model.raw[3][1] = s_sr.ghost_pos[1];
    model.raw[3][2] = s_sr.ghost_pos[2];
    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_DEPTH_TEST_LESS
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float green_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float green_color[4] = { 0.2f, 0.9f, 0.3f, 0.45f };
    jce_uniform_set(s_sr.u_light_dir,   green_dir,   1);
    jce_uniform_set(s_sr.u_light_color, green_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());
}

/* ── Hover highlight for drag-drop onto entity ────────────────────── */

void draw_hover_highlight(void)
{
    if (s_sr.hover_entity_id == 0) return;

    uint32_t id = s_sr.hover_entity_id;
    if (!jce_state_entity_exists(id) || !jce_state_entity_enabled(id)) return;

    jce_mat4 model;
    JceMesh *mesh = NULL;
    if (!build_overlay_entity_model(id, &model, &mesh)) return;
    if (!mesh) return;

    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_DEPTH_TEST_LEQUAL
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_ONE,
                                          JCE_BLEND_ONE)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float hover_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float hover_color[4] = { 0.28f, 0.28f, 0.34f, 1.0f };
    jce_uniform_set(s_sr.u_light_dir,   hover_dir,   1);
    jce_uniform_set(s_sr.u_light_color, hover_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}
