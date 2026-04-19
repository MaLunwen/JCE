/*
 * jce_scene_render_draw.cpp  Sky, grid, entities, shadows, selection outlines.
 */

#include "jce_scene_render_internal.h"
#include <SDL3/SDL_timer.h>

extern "C" {
#include <jce/graphics/jce_sprite.h>
}

/* ── Shadow map constants ─────────────────────────────────────────── */

#define SHADOW_ORTHO_SIZE 50.0f
#define CSM_SHADOW_DISTANCE_SCALE 512.0f
#define CSM_SHADOW_DISTANCE_MAX   1200.0f
#define CSM_SHADOW_DISTANCE_MIN   50.0f
#define CSM_SHADOW_FAR_HYSTERESIS_REL 0.03f
#define CSM_SHADOW_FAR_HYSTERESIS_ABS 8.0f

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

    /* Set sky mode: gradient (0) or equirect HDR (1). */
    bgfx_texture_handle_t equirect_tex = { UINT16_MAX };
    float sky_params[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (s_sr.skybox_active && s_sr.skybox) {
        equirect_tex = jce_skybox_get_equirect_texture(s_sr.skybox);
        if (BGFX_HANDLE_IS_VALID(equirect_tex)) {
            sky_params[0] = 1.0f;  /* mode = equirect */
            sky_params[1] = s_sr.skybox_exposure;
            sky_params[2] = s_sr.skybox_rotation * 0.0174533f; /* deg→rad */
            bgfx_set_texture(0, s_sr.u_sky_equirect, equirect_tex, UINT32_MAX);
        }
    }
    bgfx_set_uniform(s_sr.u_sky_params, sky_params, 1);

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

static jce_vec3 light_direction_from_transform(const JceComponentInfo *xf)
{
    if (!xf || xf->type != JCE_COMP_TRANSFORM)
        return jce_dir_light_default().direction;

    float yaw_rad = xf->data.transform.rot[1] * JCE_DEG2RAD;
    float pitch_rad = xf->data.transform.rot[0] * JCE_DEG2RAD;
    return jce_v3(
         sinf(yaw_rad) * cosf(pitch_rad),
        -sinf(pitch_rad),
        -cosf(yaw_rad) * cosf(pitch_rad));
}

static jce_vec3 resolve_shadow_light_direction(void)
{
    jce_vec3 fallback = jce_dir_light_default().direction;
    jce_vec3 first_dir = fallback;
    bool have_any_dir = false;

    int count = jce_state_get_entity_count();
    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        int comp_count = 0;
        JceComponentInfo *comps = jce_state_get_entity_components(ent->id,
                                                                  &comp_count);
        JceComponentInfo *xf = NULL;
        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type == JCE_COMP_TRANSFORM) {
                xf = &comps[c];
                break;
            }
        }

        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type != JCE_COMP_LIGHT) continue;
            if (comps[c].data.light.type != 0) continue;

            /* light_direction_from_transform returns the direction the
               light shines (from light toward scene).  All shadow/legacy
               callers expect "toward the light" convention, so negate. */
            jce_vec3 dir = light_direction_from_transform(xf);
            dir = jce_v3_scale(dir, -1.0f);
            if (!have_any_dir) {
                first_dir = dir;
                have_any_dir = true;
            }

            if (comps[c].data.light.casts_shadow)
                return dir;
        }
    }

    return have_any_dir ? first_dir : fallback;
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
    sun.direction = resolve_shadow_light_direction();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Shadow map pass ──────────────────────────────────────────────── */

static void compute_shadow_vp(const jce_vec3 *light_dir, float shadow_vp[16])
{
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 80.0f);

    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);

    jce_mat4 view = jce_m4_look_at(light_pos, center, up);

    float S = SHADOW_ORTHO_SIZE;
    jce_mat4 proj = jce_m4_ortho(-S, S, -S, S, 0.1f, 200.0f,
                                  s_sr.homogeneous_depth);

    jce_mat4 vp = jce_m4_multiply(&proj, &view);
    memcpy(shadow_vp, vp.raw, 16 * sizeof(float));
}

static void fill_csm_bias_scales(const JceCsmData *csm, float out_scales[4])
{
    float base_range = 0.1f;
    if (csm->cascade_count > 0) {
        base_range = csm->splits[1] - csm->splits[0];
        if (base_range < 0.0001f)
            base_range = 0.1f;
    }

    float last_scale = 1.0f;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        float scale = last_scale;
        if (i < csm->cascade_count) {
            float range = csm->splits[i + 1] - csm->splits[i];
            if (range < 0.0001f)
                range = base_range;
            scale = range / base_range;
            if (scale < 1.0f) scale = 1.0f;
            if (scale > 3.2f) scale = 3.2f;
            last_scale = scale;
        }
        out_scales[i] = scale;
    }
}

static void draw_shadow_pass(void)
{
    s_sr.shadow_use_csm = false;

    /* Keep CSM uniforms deterministic even if shadow rendering is unavailable. */
    {
        float disabled_splits[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float csm_params[4] = {
            1.0f / (float)s_sr.shadow_map_size,
            s_sr.csm_blend_ratio,
            s_sr.csm_normal_bias,
            s_sr.csm_filter_radius,
        };
        float bias_scales[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        bgfx_set_uniform(s_sr.u_csm_splits, disabled_splits, 1);
        bgfx_set_uniform(s_sr.u_csm_params, csm_params, 1);
        bgfx_set_uniform(s_sr.u_csm_bias_scales, bias_scales, 1);
    }

    if (!s_sr.shadow_valid) return;

    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(s_sr.renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    const bool use_csm = s_sr.csm_valid && s_sr.csm_cascade_count > 0;
    const int count = jce_state_get_entity_count();
    jce_vec3 shadow_dir = resolve_shadow_light_direction();

    if (!use_csm) {
        /* Legacy single shadow map fallback (view SHADOW_0). */
        float shadow_vp[16];
        compute_shadow_vp(&shadow_dir, shadow_vp);

        const uint16_t shadow_view = (uint16_t)JCE_VIEW_SHADOW_0;
        bgfx_set_view_rect(shadow_view, 0, 0,
                           s_sr.shadow_map_size,
                           s_sr.shadow_map_size);
        bgfx_set_view_frame_buffer(shadow_view, s_sr.shadow_fbo);
        bgfx_set_view_clear(shadow_view,
                            BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float identity[16];
        memset(identity, 0, sizeof(identity));
        identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
        bgfx_set_view_transform(shadow_view, identity, shadow_vp);

        bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);

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
        return;
    }

    s_sr.shadow_use_csm = true;

    /* CSM passes (views SHADOW_1..SHADOW_4). */
    float cam_near = s_sr.camera ? jce_camera_get_near(s_sr.camera) : 0.1f;
    float cam_far  = s_sr.camera ? jce_camera_get_far(s_sr.camera) : 200.0f;
    float cam_fov  = s_sr.camera ? jce_camera_get_fov(s_sr.camera) : 45.0f;
    float cam_aspect = (s_sr.viewport_width > 0 && s_sr.viewport_height > 0)
        ? ((float)s_sr.viewport_width / (float)s_sr.viewport_height)
        : (16.0f / 9.0f);
    float shadow_far_target;
    float shadow_far;

    if (cam_near <= 0.0f)
        cam_near = 0.1f;
    if (cam_far <= cam_near)
        cam_far = cam_near + 200.0f;
    shadow_far_target = cam_far;
    shadow_far_target = fminf(shadow_far_target,
                 fmaxf(cam_near * CSM_SHADOW_DISTANCE_SCALE,
                       CSM_SHADOW_DISTANCE_MAX));
    shadow_far_target = fmaxf(shadow_far_target,
                              cam_near + CSM_SHADOW_DISTANCE_MIN);

    if (!s_sr.shadow_far_valid) {
        s_sr.shadow_far_cached = shadow_far_target;
        s_sr.shadow_far_valid = true;
    } else {
        float far_delta = fabsf(shadow_far_target - s_sr.shadow_far_cached);
        float far_rel = far_delta / fmaxf(s_sr.shadow_far_cached,
                                          CSM_SHADOW_DISTANCE_MIN);
        if (far_delta > CSM_SHADOW_FAR_HYSTERESIS_ABS
            && far_rel > CSM_SHADOW_FAR_HYSTERESIS_REL)
        {
            s_sr.shadow_far_cached = shadow_far_target;
        }
    }

    shadow_far = s_sr.shadow_far_cached;

    jce_mat4 cam_view = jce_camera_view(s_sr.camera);
    jce_vec3 light_dir = shadow_dir;

    JceCsmData csm;
    jce_csm_compute(&csm, s_sr.csm_cascade_count,
                     cam_near, shadow_far,
                     cam_fov, cam_aspect,
                     &cam_view, &light_dir,
                     s_sr.homogeneous_depth,
                     s_sr.shadow_map_size);

    /* Render each cascade into its own FBO using SHADOW_1..SHADOW_4. */
    for (uint32_t c = 0; c < csm.cascade_count; c++) {
        uint16_t csm_view = (uint16_t)(JCE_VIEW_SHADOW_1 + c);
        if (c >= JCE_CSM_MAX_CASCADES) break;

        bgfx_set_view_rect(csm_view, 0, 0,
                           s_sr.shadow_map_size,
                           s_sr.shadow_map_size);
        bgfx_set_view_frame_buffer(csm_view, s_sr.csm_fbo[c]);
        bgfx_set_view_clear(csm_view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float csm_identity[16];
        memset(csm_identity, 0, sizeof(csm_identity));
        csm_identity[0] = csm_identity[5] = csm_identity[10] = csm_identity[15] = 1.0f;
        bgfx_set_view_transform(csm_view, csm_identity, csm.vp[c].raw[0]);

        for (int i = 0; i < count; i++) {
            JceEntityInfo *ent = jce_state_get_entity_by_index(i);
            if (!ent || !ent->enabled) continue;

            jce_mat4 model;
            JceMesh *mesh = NULL;
            const char *mat_path = NULL;
            if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;
            if (!mesh) continue;

            bgfx_set_transform(model.raw[0], 1);
            jce_mesh_submit_shadow(mesh, s_sr.renderer, csm_view);
        }
    }

    /* Upload CSM uniforms for the PBR shader. */
    bgfx_set_uniform(s_sr.u_csm_vp, csm.vp[0].raw[0],
                     (uint16_t)csm.cascade_count);

    float splits_vec4[4] = {
        csm.cascade_count > 0 ? csm.splits[1] : shadow_far,
        csm.cascade_count > 1 ? csm.splits[2] : shadow_far,
        csm.cascade_count > 2 ? csm.splits[3] : shadow_far,
        csm.cascade_count > 3 ? csm.splits[4] : shadow_far,
    };
    bgfx_set_uniform(s_sr.u_csm_splits, splits_vec4, 1);

    float csm_params[4] = {
        1.0f / (float)s_sr.shadow_map_size,
        s_sr.csm_blend_ratio,
        s_sr.csm_normal_bias,
        s_sr.csm_filter_radius,
    };
    bgfx_set_uniform(s_sr.u_csm_params, csm_params, 1);

    float bias_scales[4];
    fill_csm_bias_scales(&csm, bias_scales);
    bgfx_set_uniform(s_sr.u_csm_bias_scales, bias_scales, 1);
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
                    dl.direction = light_direction_from_transform(xf);
                    jce_light_env_add_dir_light(s_sr.light_env, &dl);
                    has_any_light = true;
                } else if (ld.type == 1) {
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = color;
                    pl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    pl.radius = ld.radius > 0.0f ? ld.radius : 10.0f;
                    if (xf) {
                        pl.position = jce_v3(xf->data.transform.pos[0],
                                             xf->data.transform.pos[1],
                                             xf->data.transform.pos[2]);
                    }
                    jce_light_env_add_point_light(s_sr.light_env, &pl);
                    has_any_light = true;
                } else if (ld.type == 2) {
                    JceSpotLightDesc sl;
                    memset(&sl, 0, sizeof(sl));
                    sl.color = color;
                    sl.intensity = intensity > 0.0f ? intensity : 1.0f;
                    sl.radius = ld.radius > 0.0f ? ld.radius : 10.0f;
                    float inner_deg = ld.inner_cone_deg > 0.0f ? ld.inner_cone_deg : 25.0f;
                    float outer_deg = ld.outer_cone_deg > 0.0f ? ld.outer_cone_deg : 35.0f;
                    sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
                    sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
                    if (xf) {
                        sl.position = jce_v3(xf->data.transform.pos[0],
                                             xf->data.transform.pos[1],
                                             xf->data.transform.pos[2]);
                        float yaw_rad = xf->data.transform.rot[1] * JCE_DEG2RAD;
                        float pitch_rad = xf->data.transform.rot[0] * JCE_DEG2RAD;
                        sl.direction = jce_v3(
                             sinf(yaw_rad) * cosf(pitch_rad),
                            -sinf(pitch_rad),
                            -cosf(yaw_rad) * cosf(pitch_rad));
                    } else {
                        sl.direction = jce_v3(0.0f, -1.0f, 0.0f);
                    }
                    jce_light_env_add_spot_light(s_sr.light_env, &sl);
                    has_any_light = true;
                }
            }
        }

        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3(-0.5f, -1.0f, -0.3f);
            dl.color = jce_v3(1.0f, 1.0f, 1.0f);
            dl.intensity = 1.0f;
            jce_light_env_add_dir_light(s_sr.light_env, &dl);
        }

        jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
        jce_light_env_set_camera_pos(s_sr.light_env, cam_pos);

        jce_light_env_apply(s_sr.light_env, s_sr.renderer);

        JceDirLight sun = jce_dir_light_default();
        sun.direction = resolve_shadow_light_direction();
        jce_lighting_apply(s_sr.renderer, &sun);
    } else {
        JceDirLight sun = jce_dir_light_default();
        sun.direction = resolve_shadow_light_direction();
        jce_lighting_apply(s_sr.renderer, &sun);
    }

    JceSceneViewMode view_mode = jce_state_get_view_mode();
    if (view_mode == JCE_VIEW_WIREFRAME || view_mode == JCE_VIEW_WIREFRAME_TEXTURED)
        jce_renderer_set_wireframe(s_sr.renderer, true);

    /* Wireframe-textured: trigger hue-Lambert shader mode (w=0.25).
       Pass the real sun direction so Lambert shading matches plain wireframe. */
    if (view_mode == JCE_VIEW_WIREFRAME_TEXTURED) {
        jce_vec3 sd  = jce_v3_normalize(resolve_shadow_light_direction());
        JceDirLight def = jce_dir_light_default();
        float wf_dir[4]   = { sd.x, sd.y, sd.z, 0.25f };  /* w=0.25 → hue-Lambert */
        float wf_color[4] = { def.color.x, def.color.y, def.color.z, def.ambient };
        bgfx_set_uniform(s_sr.u_light_dir,   wf_dir,   1);
        bgfx_set_uniform(s_sr.u_light_color, wf_color, 1);
    }

    /* Timing for animation updates. */
    static uint64_t s_last_ticks = 0;
    uint64_t now_ticks = SDL_GetPerformanceCounter();
    float anim_dt = 0.0f;
    if (s_last_ticks > 0) {
        anim_dt = (float)(now_ticks - s_last_ticks) /
                  (float)SDL_GetPerformanceFrequency();
        if (anim_dt > 0.1f) anim_dt = 0.1f; /* clamp large spikes */
    }
    s_last_ticks = now_ticks;

    /* Begin sprite batch for 2D sprite entities. */
    if (s_sr.sprite_batch)
        jce_sprite_batch_begin(s_sr.sprite_batch);

    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        const char *mat_path = NULL;
        if (!build_entity_model(ent, &model, &mesh, &mat_path)) continue;

        /* ── Skinned / animated entity path ──────────────────────── */
        {
            int anim_cc = 0;
            JceComponentInfo *anim_cs = jce_state_get_entity_components(
                ent->id, &anim_cc);
            JceComponentInfo *sa_comp = nullptr;
            for (int c = 0; c < anim_cc; ++c) {
                if (anim_cs[c].type == JCE_COMP_SKELETAL_ANIMATOR) {
                    sa_comp = &anim_cs[c];
                    break;
                }
            }
            if (sa_comp && sa_comp->data.skeletal_animator.skeleton_path[0]) {
                ModelCacheEntry *mc = get_cached_model(
                    sa_comp->data.skeletal_animator.skeleton_path, ent->id);
                if (mc && mc->model) {
                    if (mc->player) {
                        int ac = sa_comp->data.skeletal_animator.active_clip;
                        float anim_speed = sa_comp->data.skeletal_animator.speed;
                        if (anim_speed <= 0.0f) anim_speed = 1.0f;

                        JceAnimClip *clip = nullptr;
                        if (ac >= 0 && ac < (int)jce_model_anim_count(mc->model))
                            clip = jce_model_get_anim(mc->model, (uint32_t)ac);

                        bool comp_playing = sa_comp->data.skeletal_animator.playing;
                        bool clip_changed = (mc->active_clip != ac);
                        bool loop_changed = (mc->loop != sa_comp->data.skeletal_animator.loop);
                        bool speed_changed = fabsf(mc->speed -
                            anim_speed) > 0.0001f;
                        bool paused_changed = (mc->paused == comp_playing);

                        if (comp_playing && clip) {
                            if (!jce_anim_player_is_playing(mc->player)
                                || clip_changed || loop_changed) {
                                /* Start or restart the selected clip. */
                                jce_anim_player_play(mc->player, clip,
                                    sa_comp->data.skeletal_animator.loop,
                                    anim_speed);
                            } else if (speed_changed || paused_changed) {
                                jce_anim_player_set_speed(mc->player,
                                    anim_speed);
                            }
                            /* Ensure unpaused and speed synced. */
                            jce_anim_player_pause(mc->player, false);
                            jce_anim_player_set_speed(mc->player,
                                anim_speed);
                        } else {
                            if (clip && (clip_changed || loop_changed)) {
                                jce_anim_player_play(mc->player, clip,
                                    sa_comp->data.skeletal_animator.loop,
                                    anim_speed);
                                jce_anim_player_set_time(mc->player, 0.0f);
                            }
                            /* Paused — freeze at current pose or selected clip. */
                            if (jce_anim_player_is_playing(mc->player))
                                jce_anim_player_pause(mc->player, true);
                        }

                        mc->active_clip = ac;
                        mc->loop = sa_comp->data.skeletal_animator.loop;
                        mc->speed = anim_speed;
                        mc->paused = !comp_playing;

                        jce_mat4 joints[64];
                        uint32_t nj = jce_anim_player_update(mc->player,
                                                              anim_dt, joints, 64);
                        jce_model_draw(mc->model, s_sr.renderer,
                                       scene_view_id(), &model,
                                       nj > 0 ? joints : NULL, nj);
                    } else {
                        jce_model_draw(mc->model, s_sr.renderer,
                                       scene_view_id(), &model, NULL, 0);
                    }
                    continue; /* skip regular mesh path */
                }
            }
        }

        /* ── Sprite entity path (JCE_COMP_SPRITE_RENDERER or SPRITE_ANIMATOR) ── */
        if (s_sr.sprite_batch) {
            int sp_cc = 0;
            JceComponentInfo *sp_cs = jce_state_get_entity_components(ent->id, &sp_cc);
            JceComponentInfo *spr_comp = nullptr;
            JceComponentInfo *spa_comp = nullptr;
            for (int c = 0; c < sp_cc; ++c) {
                if (sp_cs[c].type == JCE_COMP_SPRITE_RENDERER) spr_comp = &sp_cs[c];
                if (sp_cs[c].type == JCE_COMP_SPRITE_ANIMATOR)  spa_comp = &sp_cs[c];
            }
            if (spr_comp) {
                /* Load sprite texture from the sprite_path. */
                bgfx_texture_handle_t spr_tex = { UINT16_MAX };
                if (spr_comp->data.sprite_renderer.sprite_path[0]) {
                    JceTexture t = get_cached_texture(
                        spr_comp->data.sprite_renderer.sprite_path, NULL);
                    if (jce_texture_valid(t)) spr_tex.idx = t.idx;
                }
                if (!BGFX_HANDLE_IS_VALID(spr_tex))
                    spr_tex = s_sr.white_tex;

                /* Compute UV region from sprite animator frame data. */
                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                if (spa_comp) {
                    int fw = spa_comp->data.sprite_animator.frame_width;
                    int fh = spa_comp->data.sprite_animator.frame_height;
                    if (fw > 0 && fh > 0) {
                        /* Simple grid-based UV: no atlas loaded yet, just use
                           frame dimensions relative to a 1x1 mapping. Full
                           integration would load the atlas and pick the frame. */
                        u0 = 0.0f; v0 = 0.0f;
                        u1 = 1.0f; v1 = 1.0f;
                    }
                }

                /* Flip UV if requested. */
                if (spr_comp->data.sprite_renderer.flip_x) {
                    float tmp = u0; u0 = u1; u1 = tmp;
                }
                if (spr_comp->data.sprite_renderer.flip_y) {
                    float tmp = v0; v0 = v1; v1 = tmp;
                }

                /* Tint color (ABGR). */
                const float *sc = spr_comp->data.sprite_renderer.color;
                uint8_t r8 = (uint8_t)(sc[0] * 255.0f);
                uint8_t g8 = (uint8_t)(sc[1] * 255.0f);
                uint8_t b8 = (uint8_t)(sc[2] * 255.0f);
                uint8_t a8 = (uint8_t)(sc[3] * 255.0f);
                uint32_t abgr = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16)
                              | ((uint32_t)g8 << 8) | (uint32_t)r8;

                jce_sprite_batch_add(s_sr.sprite_batch, spr_tex,
                                     model.raw[0],
                                     u0, v0, u1, v1,
                                     abgr, spr_comp->data.sprite_renderer.sorting_order);
                continue; /* skip regular mesh path */
            }
        }

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

        /* ── Non-wireframe rendering with PBR lighting ────────────────── */
        if (view_mode != JCE_VIEW_WIREFRAME && view_mode != JCE_VIEW_WIREFRAME_TEXTURED && mr_comp) {
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

            /* Load textures only for Textured mode; Shaded uses PBR
               factors with fallback white/flat-normal textures. */
            if (view_mode == JCE_VIEW_TEXTURED) {
                if (mr_comp->data.mesh_renderer.albedo_tex[0]) {
                    JceTexture t = get_cached_texture(mr_comp->data.mesh_renderer.albedo_tex, NULL);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                if (!jce_texture_valid(pbr.albedo_map)) {
                    const char *mp = mr_comp->data.mesh_renderer.mesh_path;
                    JceTexture t = get_cached_texture(mat_path, mp);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* If albedo is still missing, use magenta/black checker. */
                if (!jce_texture_valid(pbr.albedo_map)
                    && BGFX_HANDLE_IS_VALID(s_sr.checker_tex)) {
                    pbr.albedo_map.idx = s_sr.checker_tex.idx;
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
            }

            jce_pbr_material_bind(&pbr, s_sr.renderer, scene_view_id());

            if (s_sr.shadow_valid && !s_sr.shadow_use_csm) {
                bgfx_set_texture(5, s_sr.u_shadowMap, s_sr.shadow_tex, UINT32_MAX);
                float shadow_vp[16];
                jce_vec3 shadow_dir = resolve_shadow_light_direction();
                compute_shadow_vp(&shadow_dir, shadow_vp);
                bgfx_set_uniform(s_sr.u_shadowVP, shadow_vp, 1);
            }

            /* Bind CSM cascade textures (stages 9-12). */
            if (s_sr.csm_valid && s_sr.shadow_use_csm) {
                for (uint32_t ci = 0; ci < s_sr.csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
                    bgfx_set_texture((uint8_t)(9 + ci), s_sr.u_csm_samplers[ci],
                                     s_sr.csm_tex[ci], UINT32_MAX);
            }

            /* Bind IBL textures (stages 6-8) if active. */
            {
                float ibl_params[4] = {
                    0.0f,
                    5.0f,
                    0.0f,
                    s_sr.postfx_tonemap_active ? 1.0f : 0.0f
                };
                if (s_sr.skybox_active && s_sr.ibl_data) {
                    bgfx_texture_handle_t irr = jce_ibl_get_irradiance(s_sr.ibl_data);
                    bgfx_texture_handle_t pf  = jce_ibl_get_prefilter(s_sr.ibl_data);
                    if (BGFX_HANDLE_IS_VALID(irr) && BGFX_HANDLE_IS_VALID(pf) &&
                        BGFX_HANDLE_IS_VALID(s_sr.brdf_lut)) {
                        bgfx_set_texture(6, s_sr.u_ibl_irradiance, irr, UINT32_MAX);
                        bgfx_set_texture(7, s_sr.u_ibl_prefilter, pf, UINT32_MAX);
                        bgfx_set_texture(8, s_sr.u_ibl_brdf_lut, s_sr.brdf_lut, UINT32_MAX);
                        ibl_params[0] = 1.0f;
                    }
                }
                bgfx_set_uniform(s_sr.u_ibl_params, ibl_params, 1);
            }

            jce_mesh_submit_pbr(mesh, s_sr.renderer, scene_view_id());
            continue;
        }

        /* ── Bind line color source for wireframe modes. Plain wireframe
         *    keeps white lines; wireframe-textured samples entity albedo. */
        {
            bgfx_texture_handle_t bind_tex = s_sr.white_tex;

            if (view_mode == JCE_VIEW_TEXTURED || view_mode == JCE_VIEW_WIREFRAME_TEXTURED) {
                const char *mp = mr_comp ? mr_comp->data.mesh_renderer.mesh_path : NULL;
                bool bound_tex = false;

                if (mr_comp && mr_comp->data.mesh_renderer.albedo_tex[0]) {
                    JceTexture tex = get_cached_texture(
                        mr_comp->data.mesh_renderer.albedo_tex, NULL);
                    if (jce_texture_valid(tex)) {
                        bind_tex.idx = tex.idx;
                        bound_tex = true;
                    }
                }

                if (!bound_tex) {
                    JceTexture tex = get_cached_texture(mat_path, mp);
                    if (jce_texture_valid(tex)) {
                        bind_tex.idx = tex.idx;
                        bound_tex = true;
                    }
                }

                if (!bound_tex && view_mode == JCE_VIEW_TEXTURED) {
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
                /* wireframe-textured + no texture -> keep white_tex fallback. */
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
        }

        jce_mesh_submit(mesh, s_sr.renderer, scene_view_id());
    }

    /* Flush queued sprite quads. */
    if (s_sr.sprite_batch && jce_sprite_batch_count(s_sr.sprite_batch) > 0)
        jce_sprite_batch_flush(s_sr.sprite_batch, s_sr.renderer, scene_view_id());

    if (view_mode == JCE_VIEW_WIREFRAME || view_mode == JCE_VIEW_WIREFRAME_TEXTURED)
        jce_renderer_set_wireframe(s_sr.renderer, false);

    draw_selection_outlines();

    /* Physics debug visualization. */
    if (jce_state_get_show_physics_debug()) {
        draw_physics_debug();
    }
}

/* ── Physics debug visualization ──────────────────────────────────── */

void draw_physics_debug(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    const uint32_t col_box     = 0xFF00FF00; /* green */
    const uint32_t col_sphere  = 0xFF00FFFF; /* cyan */
    const uint32_t col_capsule = 0xFFFFFF00; /* yellow */

    for (int i = 0; i < count; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        int comp_count = 0;
        JceComponentInfo *comps = jce_state_get_entity_components(ent->id,
                                                                   &comp_count);

        /* Find transform. */
        const float *pos   = NULL;
        const float *rot_e = NULL;
        const float *scale = NULL;
        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type == JCE_COMP_TRANSFORM) {
                pos   = comps[c].data.transform.pos;
                rot_e = comps[c].data.transform.rot;
                scale = comps[c].data.transform.scale;
                break;
            }
        }
        if (!pos) continue;

        jce_vec3 center = jce_v3(pos[0], pos[1], pos[2]);
        jce_quat q = jce_q_identity();
        if (rot_e && (rot_e[0] != 0 || rot_e[1] != 0 || rot_e[2] != 0))
            q = jce_q_from_euler(rot_e[0]*JCE_DEG2RAD,
                                  rot_e[1]*JCE_DEG2RAD,
                                  rot_e[2]*JCE_DEG2RAD);

        /* Draw collider shapes. */
        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type == JCE_COMP_BOX_COLLIDER) {
                float sx = (scale && scale[0] > 0) ? scale[0] : 1.0f;
                float sy = (scale && scale[1] > 0) ? scale[1] : 1.0f;
                float sz = (scale && scale[2] > 0) ? scale[2] : 1.0f;
                jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
                jce_debug_draw_box(center, half, q, col_box);
            } else if (comps[c].type == JCE_COMP_SPHERE_COLLIDER) {
                float r = (scale && scale[0] > 0) ? scale[0] * 0.5f : 0.5f;
                jce_debug_draw_sphere(center, r, col_sphere);
            } else if (comps[c].type == JCE_COMP_CHARACTER_CONTROLLER) {
                /* Use the character controller's capsule shape. */
                float radius = 0.3f;
                float half_h = 0.6f;
                jce_debug_draw_capsule(center, radius, half_h, q, col_capsule);
            }
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
