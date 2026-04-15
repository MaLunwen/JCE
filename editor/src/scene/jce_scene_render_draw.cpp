/*
 * jce_scene_render_draw.cpp  Sky, grid, entities, shadows, selection outlines.
 */

#include "jce_scene_render_internal.h"

/* ── Shadow map constants ─────────────────────────────────────────── */

#define SHADOW_MAP_SIZE  2048
#define SHADOW_ORTHO_SIZE 20.0f

/* ── Sky gradient (smooth sky dome — no visible edges) ────────────── */

void draw_sky_gradient(void)
{
    if (!BGFX_HANDLE_IS_VALID(s_sr.prog_sky)) return;

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

    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,
        0.65f, 0.78f, 0.92f, 1.0f,
        0.22f, 0.22f, 0.28f, 1.0f,
    };
    bgfx_set_uniform(s_sr.u_sky_colors, sky_colors, 3);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);

    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(scene_view_id(), s_sr.prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

void draw_grid(void)
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

/* ── Entity model builder ─────────────────────────────────────────── */

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
    int  mesh_shape = 0;

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
                *out_mesh = get_cached_mesh(mesh_path, pos);
            }
            if (!*out_mesh) {
                switch (mesh_shape) {
                default:
                case JCE_MESH_SHAPE_CUBE:     *out_mesh = s_sr.cube_mesh;     break;
                case JCE_MESH_SHAPE_SPHERE:   *out_mesh = s_sr.sphere_mesh;   break;
                case JCE_MESH_SHAPE_PLANE:    *out_mesh = s_sr.plane_mesh;    break;
                case JCE_MESH_SHAPE_CAPSULE:  *out_mesh = s_sr.capsule_mesh;  break;
                case JCE_MESH_SHAPE_CYLINDER: *out_mesh = s_sr.cylinder_mesh; break;
                }
            }
        }
    }

    if (out_material_path)
        *out_material_path = mat_path;

    return true;
}

/* ── Selection outlines ───────────────────────────────────────────── */

static void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };

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

        bgfx_set_uniform(s_sr.u_light_dir,   flat_dir,   1);
        bgfx_set_uniform(s_sr.u_light_color, flat_color,  1);
        bgfx_set_texture(0, su, s_sr.white_tex, UINT32_MAX);

        jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer, scene_view_id());
    }

    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Shadow map pass ──────────────────────────────────────────────── */

static void compute_shadow_vp(const jce_vec3 *light_dir, float shadow_vp[16])
{
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 30.0f);

    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);

    jce_mat4 view = jce_m4_look_at(light_pos, center, up);

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

    float shadow_vp[16];
    compute_shadow_vp(&sun.direction, shadow_vp);

    const uint16_t shadow_view = (uint16_t)JCE_VIEW_SHADOW_0;
    bgfx_set_view_rect(shadow_view, 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    bgfx_set_view_frame_buffer(shadow_view, s_sr.shadow_fbo);
    bgfx_set_view_clear(shadow_view,
                        BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

    float identity[16];
    memset(identity, 0, sizeof(identity));
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    bgfx_set_view_transform(shadow_view, identity, shadow_vp);

    bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);

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

/* ── Main entity rendering ────────────────────────────────────────── */

void draw_entities(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    draw_shadow_pass();

    /* Gather lights from entity components. */
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

                JceComponentInfo *xf = NULL;
                for (int t = 0; t < comp_count; t++) {
                    if (comps[t].type == JCE_COMP_TRANSFORM) { xf = &comps[t]; break; }
                }

                if (ld.type == 0) {
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = color;
                    dl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    if (xf) {
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

        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3(0.5f, 1.0f, 0.3f);
            dl.color = jce_v3(1.0f, 1.0f, 1.0f);
            dl.intensity = 1.0f;
            jce_light_env_add_dir_light(s_sr.light_env, &dl);
        }

        jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
        jce_light_env_set_camera_pos(s_sr.light_env, cam_pos);

        jce_light_env_apply(s_sr.light_env, s_sr.renderer);

        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    } else {
        JceDirLight sun = jce_dir_light_default();
        jce_lighting_apply(s_sr.renderer, &sun);
    }

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

        /* Resolve the MeshRenderer component for texture binding. */
        int cc = 0;
        JceComponentInfo *cs = jce_state_get_entity_components(ent->id, &cc);
        JceComponentInfo *mr_comp = NULL;
        for (int c = 0; c < cc; c++) {
            if (cs[c].type == JCE_COMP_MESH_RENDERER) {
                mr_comp = &cs[c];
                break;
            }
        }

        bool has_pbr_textures = mr_comp
            && (mr_comp->data.mesh_renderer.albedo_tex[0]
                || mr_comp->data.mesh_renderer.mr_tex[0]
                || mr_comp->data.mesh_renderer.normal_tex[0]
                || mr_comp->data.mesh_renderer.ao_tex[0]
                || mr_comp->data.mesh_renderer.emissive_tex[0]);

        /* ── TEXTURED (non-wireframe) with PBR textures → full PBR path ── */
        if (view_mode == JCE_VIEW_TEXTURED && has_pbr_textures) {
            JcePbrMaterial pbr = jce_pbr_material_default();
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

            if (mr_comp->data.mesh_renderer.albedo_tex[0]) {
                JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.albedo_tex, NULL);
                if (jce_texture_valid(t)) pbr.albedo_map = t;
            }
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

            if (s_sr.shadow_valid) {
                bgfx_set_texture(5, s_sr.u_shadowMap, s_sr.shadow_tex, UINT32_MAX);
                float shadow_vp[16];
                jce_vec3 shadow_dir = jce_v3(0.5f, 1.0f, 0.3f);
                compute_shadow_vp(&shadow_dir, shadow_vp);
                bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);
            }

            jce_mesh_submit_pbr(mesh, s_sr.renderer, scene_view_id());
            continue;
        }

        /* ── Bind texture for non-wireframe modes, and also for wireframe
         *    when the entity has a texture (texture-colored wireframe,
         *    like CK's F3+V).  Untextured entities get white/checker so
         *    their wireframe is a solid color. ────────────────────────── */
        {
            bgfx_texture_handle_t bind_tex = s_sr.white_tex;

            if (view_mode == JCE_VIEW_TEXTURED || view_mode == JCE_VIEW_WIREFRAME) {
                const char *mp = mr_comp ? mr_comp->data.mesh_renderer.mesh_path : NULL;

                JceTexture tex = get_cached_texture(mat_path, mp);
                if (tex.idx != UINT16_MAX) {
                    bind_tex.idx = tex.idx;
                } else if (view_mode == JCE_VIEW_TEXTURED) {
                    bind_tex = s_sr.checker_tex;
                    bool has_mat  = mat_path && mat_path[0] != '\0';
                    bool has_mesh_path = mp && mp[0] != '\0';
                    if (has_mat || has_mesh_path) {
                        if (jce_editor_scene_asset_cache_take_texture_warning(mat_path, mp)) {
                            LOG_WARN(LOG_TAG,
                                     "TEXTURED entity '%s': NO texture (mat='%s' mesh='%s')",
                                     ent->name,
                                     has_mat  ? mat_path : "",
                                     has_mesh_path ? mp  : "");
                        }
                    }
                }
                /* wireframe + no texture → keep white_tex for solid-color lines */
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
        }

        jce_mesh_submit(mesh, s_sr.renderer, scene_view_id());
    }

    if (view_mode == JCE_VIEW_WIREFRAME)
        jce_renderer_set_wireframe(s_sr.renderer, false);

    draw_selection_outlines();
}

/* ── Ghost (drag-preview) model rendering ─────────────────────────── */

void draw_ghost_entity(void)
{
    if (!s_sr.ghost_active || s_sr.ghost_mesh_path[0] == '\0')
        return;

    JceMesh *mesh = get_cached_mesh(s_sr.ghost_mesh_path, s_sr.ghost_pos);
    if (!mesh) return;

    /* Build identity-scale model matrix at the ghost position. */
    jce_mat4 model = jce_m4_identity();
    model.raw[3][0] = s_sr.ghost_pos[0];
    model.raw[3][1] = s_sr.ghost_pos[1];
    model.raw[3][2] = s_sr.ghost_pos[2];
    bgfx_set_transform(model.raw[0], 1);

    /* Bind a green-tinted white texture. */
    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    bgfx_uniform_handle_t su = { uh.idx };
    bgfx_set_texture(0, su, s_sr.white_tex, UINT32_MAX);

    /* Semi-transparent green: alpha blend + write RGB/A + depth test. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LESS
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA)
                   | BGFX_STATE_MSAA;
    bgfx_set_state(state, 0);

    /* Use the flat color shader with green tint via the light uniforms.
     * Use overlay submit to preserve the custom blend state. */
    float green_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float green_color[4] = { 0.2f, 0.9f, 0.3f, 0.45f };
    bgfx_set_uniform(s_sr.u_light_dir,   green_dir,   1);
    bgfx_set_uniform(s_sr.u_light_color, green_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());
}

/* ── Hover highlight for drag-drop onto entity ────────────────────── */

void draw_hover_highlight(void)
{
    if (s_sr.hover_entity_id == 0) return;

    JceEntityInfo *ent = jce_state_get_entity(s_sr.hover_entity_id);
    if (!ent || !ent->enabled) return;

    jce_mat4 model;
    JceMesh *mesh = NULL;
    const char *mat_path = NULL;
    if (!build_entity_model(ent, &model, &mesh, &mat_path)) return;
    if (!mesh) return;

    bgfx_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    bgfx_uniform_handle_t su = { uh.idx };
    bgfx_set_texture(0, su, s_sr.white_tex, UINT32_MAX);

    /* Additive brightness overlay — model lights up when hovered.
     * Use overlay submit to preserve the custom additive blend state. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_DEPTH_TEST_LEQUAL
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_ONE)
                   | BGFX_STATE_MSAA;
    bgfx_set_state(state, 0);

    float hover_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float hover_color[4] = { 0.28f, 0.28f, 0.34f, 1.0f };
    bgfx_set_uniform(s_sr.u_light_dir,   hover_dir,   1);
    bgfx_set_uniform(s_sr.u_light_color, hover_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());

    /* Restore normal lighting so subsequent draws are unaffected. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}
