/*
 * jce_scene_renderer.c  Engine scene renderer implementation.
 *
 * Adapted from editor's scene render code, factored to render a JceScene
 * directly with no editor-state coupling.
 */

#include <jce/render/jce_scene_renderer.h>

#include <bgfx/c99/bgfx.h>

#include <jce/scene/jce_scene.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_csm.h>
#include <jce/graphics/jce_ibl.h>
#include <jce/graphics/jce_lighting.h>
#include <jce/graphics/jce_lighting_system.h>
#include <jce/graphics/jce_material.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_model.h>
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_postfx.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_renderer_caps.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/graphics/jce_skybox.h>
#include <jce/graphics/jce_sprite_batch.h>
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_views.h>
#include <jce/animation/jce_animation.h>
#include <jce/core/jce_allocator.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_math.h>
#include <jce/core/pak_loader.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LOG_TAG "scene_renderer"

#define SR_MODEL_CACHE_MAX     32
#define SR_TEX_CACHE_MAX       512
#define SR_MAX_ENTITIES        4096
#define SHADOW_ORTHO_SIZE      50.0f
#define CSM_DIST_SCALE         512.0f
#define CSM_DIST_MAX           1200.0f
#define CSM_DIST_MIN           50.0f
#define CSM_FAR_HYST_REL       0.03f
#define CSM_FAR_HYST_ABS       8.0f

/* ── Internal struct ──────────────────────────────────────────────── */

typedef struct {
    char            path[256];
    JceModel       *model;
    JceAnimPlayer  *player;
    uint32_t        bound_entity;
    int             active_clip;
    bool            loop;
    float           speed;
    bool            paused;
    bool            used;
} SrModelCache;

struct JceSceneRenderer {
    JceRenderer            *renderer;
    const JcePakArchive    *pak;
    JceSceneRendererCallbacks cbs;
    bool                    has_cbs;
    bool                    homogeneous_depth;

    /* Sky shader + uniforms. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_sky_params;
    bgfx_uniform_handle_t   u_sky_equirect;

    /* Procedural meshes. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    /* Fallback textures. */
    bgfx_texture_handle_t   white_tex;
    bgfx_texture_handle_t   checker_tex;     /* magenta/yellow "missing" pattern */

    /* Legacy lighting uniforms. */
    bgfx_uniform_handle_t   u_light_dir;
    bgfx_uniform_handle_t   u_light_color;

    /* Shadow map resources. */
    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;
    bool                       shadow_use_csm;
    uint16_t                   shadow_map_size;
    bool                       shadow_far_valid;
    float                      shadow_far_cached;

    /* CSM resources. */
    uint32_t                   csm_cascade_count;
    bgfx_texture_handle_t      csm_tex[JCE_CSM_MAX_CASCADES];
    bgfx_frame_buffer_handle_t csm_fbo[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_samplers[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_vp;
    bgfx_uniform_handle_t      u_csm_splits;
    bgfx_uniform_handle_t      u_csm_params;
    bgfx_uniform_handle_t      u_csm_bias_scales;
    bool                       csm_valid;
    float                      csm_blend_ratio;
    float                      csm_normal_bias;
    float                      csm_filter_radius;
    JceCsmData                 last_csm;
    bool                       last_csm_valid;

    /* Lighting environment. */
    JceLightEnv              *light_env;

    /* Skybox / IBL state. */
    JceSkybox               *skybox;
    JceIblData              *ibl_data;
    bgfx_texture_handle_t    brdf_lut;
    bgfx_uniform_handle_t    u_ibl_irradiance;
    bgfx_uniform_handle_t    u_ibl_prefilter;
    bgfx_uniform_handle_t    u_ibl_brdf_lut;
    bgfx_uniform_handle_t    u_ibl_params;
    char                     skybox_hdr_path[256];
    bool                     skybox_active;
    float                    skybox_exposure;
    float                    skybox_rotation;
    bool                     postfx_tonemap_active;

    /* Sprite batch for 2D sprite entities. */
    JceSpriteBatch          *sprite_batch;

    /* Model / animation cache. */
    SrModelCache             model_cache[SR_MODEL_CACHE_MAX];

    /* Resolved-texture cache (path → JceTexture). Prevents per-frame
     * bgfx texture leaks. Cleared at destroy. */
    struct {
        char       path[256];
        JceTexture tex;
        bool       used;
        bool       failed; /* tried, but loader returned invalid */
    } tex_cache[SR_TEX_CACHE_MAX];
    int tex_cache_count;

    /* PostFX pipeline (owned). */
    JcePostFXPipeline       *postfx_pipeline;
};

/* ── Entity collection ────────────────────────────────────────────── */

typedef struct {
    JceEntity entities[SR_MAX_ENTITIES];
    int       count;
} EntityList;

static void collect_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    EntityList *list = (EntityList *)ud;
    if (list->count < SR_MAX_ENTITIES)
        list->entities[list->count++] = e;
}

static bool entity_enabled(JceScene *scene, JceEntity e)
{
    if (jce_scene_has_editor_meta(scene, e)) {
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
        if (m && !m->enabled) return false;
    }
    return true;
}

/* ── Texture cache (resolved paths) ───────────────────────────────── */

static JceTexture sr_resolve_texture2(JceSceneRenderer *sr,
                                       const char *material_path,
                                       const char *mesh_path);

static JceTexture sr_resolve_texture(JceSceneRenderer *sr, const char *path)
{
    return sr_resolve_texture2(sr, path, NULL);
}

static JceTexture sr_resolve_texture2(JceSceneRenderer *sr,
                                       const char *material_path,
                                       const char *mesh_path)
{
    JceTexture invalid = { UINT16_MAX };
    bool have_mat  = material_path && material_path[0] != '\0';
    bool have_mesh = mesh_path     && mesh_path[0]     != '\0';
    if (!sr || (!have_mat && !have_mesh)) return invalid;

    bool have_cb = sr->has_cbs && sr->cbs.load_texture;

    /* Editor mode: the callback (asset cache) already maintains its own
     * deduplicated cache AND can recreate texture handles on async reloads.
     * Caching the handle locally would pin a stale (destroyed) bgfx handle
     * and cause the texture to render BLACK after any invalidation. So we
     * always re-query through the callback in editor mode. */
    if (have_cb)
        return sr->cbs.load_texture(have_mat ? material_path : NULL,
                                    have_mesh ? mesh_path : NULL,
                                    sr->cbs.userdata);

    /* Runtime mode: PAK-only loader is stable; cache for performance.
     * Runtime never uses mesh_path fallback (PAK has no MTL parser). */
    const char *path = have_mat ? material_path : mesh_path;
    for (int i = 0; i < sr->tex_cache_count; i++) {
        if (sr->tex_cache[i].used &&
            strncmp(sr->tex_cache[i].path, path,
                    sizeof(sr->tex_cache[i].path)) == 0)
        {
            return sr->tex_cache[i].failed ? invalid : sr->tex_cache[i].tex;
        }
    }

    JceTexture tex = invalid;
    if (sr->pak) tex = jce_texture_load(sr->pak, path);
    bool ok = jce_texture_valid(tex);

    if (sr->tex_cache_count < SR_TEX_CACHE_MAX) {
        int idx = sr->tex_cache_count++;
        snprintf(sr->tex_cache[idx].path, sizeof(sr->tex_cache[idx].path),
                 "%s", path);
        sr->tex_cache[idx].tex    = tex;
        sr->tex_cache[idx].used   = true;
        sr->tex_cache[idx].failed = !ok;
    }
    return tex;
}

/* ── Model cache ──────────────────────────────────────────────────── */

static SrModelCache *sr_get_model(JceSceneRenderer *sr, const char *path,
                                  uint32_t entity_id)
{
    if (!path || path[0] == '\0') return NULL;

    int free_slot = -1;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, path) == 0) {
            e->bound_entity = entity_id;
            return e;
        }
        if (!e->used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    JceModel *model = NULL;
    if (sr->has_cbs && sr->cbs.load_model) {
        model = sr->cbs.load_model(path, sr->cbs.userdata);
    } else {
        model = jce_model_load_gltf(sr->pak, path);
    }
    if (!model) {
        LOG_WARN(LOG_TAG, "model cache: cannot load %s", path);
        return NULL;
    }

    SrModelCache *e = &sr->model_cache[free_slot];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->model        = model;
    e->player       = NULL;
    e->bound_entity = entity_id;
    e->active_clip  = -1;
    e->loop         = false;
    e->speed        = 1.0f;
    e->paused       = true;
    e->used         = true;

    JceSkeleton *skel = jce_model_get_skeleton(model);
    if (skel && jce_model_anim_count(model) > 0) {
        e->player = jce_anim_player_create(skel);
    }
    return e;
}

/* ── Mesh resolution ──────────────────────────────────────────────── */

static JceMesh *sr_resolve_mesh(JceSceneRenderer *sr, const JceMeshRenderer *mr)
{
    if (!mr) return NULL;
    JceMesh *mesh = NULL;
    if (mr->mesh_path[0] != '\0') {
        if (sr->has_cbs && sr->cbs.load_mesh)
            mesh = sr->cbs.load_mesh(mr->mesh_path, sr->cbs.userdata);
        else
            mesh = jce_mesh_load(sr->pak, mr->mesh_path);
    }
    if (mesh) return mesh;

    switch (mr->mesh_shape) {
    default:
    case 0: return sr->cube_mesh;
    case 1: return sr->sphere_mesh;
    case 2: return sr->plane_mesh;
    case 3: return sr->capsule_mesh;
    case 4: return sr->cylinder_mesh;
    }
}

static bool sr_build_entity_model(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, jce_mat4 *out_model,
                                  JceMesh **out_mesh)
{
    if (!scene || e == JCE_ENTITY_INVALID) return false;
    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t) return false;

    float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
    float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
    float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
    *out_model = jce_m4_from_trs(t->position, t->rotation, jce_v3(sx, sy, sz));

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            *out_mesh = sr_resolve_mesh(sr, mr);
        }
    }
    return true;
}

/* ── Light direction helpers ──────────────────────────────────────── */

static jce_vec3 sr_light_shine_direction(const jce_vec3 *comp_dir)
{
    if (!comp_dir) return jce_dir_light_default().direction;
    float len2 = comp_dir->x * comp_dir->x +
                 comp_dir->y * comp_dir->y +
                 comp_dir->z * comp_dir->z;
    if (len2 < 1e-8f) return jce_dir_light_default().direction;
    return *comp_dir;
}

static jce_vec3 sr_resolve_shadow_light_direction(JceScene *scene,
                                                  EntityList *list)
{
    jce_vec3 fallback = jce_dir_light_default().direction;
    jce_vec3 first_dir = fallback;
    bool have_any = false;

    if (!scene) return fallback;

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_dir_light(scene, e)) continue;
        JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
        if (!dl) continue;
        jce_vec3 dir = sr_light_shine_direction(&dl->direction);
        dir = jce_v3_scale(dir, -1.0f);
        if (!have_any) { first_dir = dir; have_any = true; }
        if (dl->casts_shadow) return dir;
    }
    return have_any ? first_dir : fallback;
}

/* ── Shadow VP / CSM helpers ──────────────────────────────────────── */

static void sr_compute_shadow_vp(JceSceneRenderer *sr, const jce_vec3 *light_dir,
                                 float shadow_vp[16])
{
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 80.0f);
    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0, 0, 1) : jce_v3(0, 1, 0);

    jce_mat4 view = jce_m4_look_at(light_pos, center, up);
    float S = SHADOW_ORTHO_SIZE;
    jce_mat4 proj = jce_m4_ortho(-S, S, -S, S, 0.1f, 200.0f, sr->homogeneous_depth);
    jce_mat4 vp = jce_m4_multiply(&proj, &view);
    memcpy(shadow_vp, vp.raw, 16 * sizeof(float));
}

static void sr_fill_csm_bias_scales(const JceCsmData *csm, float out_scales[4])
{
    float base_range = 0.1f;
    if (csm->cascade_count > 0) {
        base_range = csm->splits[1] - csm->splits[0];
        if (base_range < 0.0001f) base_range = 0.1f;
    }
    float last_scale = 1.0f;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        float scale = last_scale;
        if (i < csm->cascade_count) {
            float range = csm->splits[i + 1] - csm->splits[i];
            if (range < 0.0001f) range = base_range;
            scale = range / base_range;
            if (scale < 1.0f)  scale = 1.0f;
            if (scale > 20.0f) scale = 20.0f;
            last_scale = scale;
        }
        out_scales[i] = scale;
    }
}

/* ── Sky pass ─────────────────────────────────────────────────────── */

static void sr_draw_sky_gradient(JceSceneRenderer *sr, uint16_t view_id)
{
    if (!BGFX_HANDLE_IS_VALID(sr->prog_sky)) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &sr->sky_layout, 4, &tib, 6, false))
        return;

    float *v = (float *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;

    v[0] = -1.0f; v[1] = -1.0f; v[2]  = 0.0f;
    v[3] =  1.0f; v[4] = -1.0f; v[5]  = 0.0f;
    v[6] =  1.0f; v[7] =  1.0f; v[8]  = 0.0f;
    v[9] = -1.0f; v[10]=  1.0f; v[11] = 0.0f;

    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,
        0.65f, 0.78f, 0.92f, 1.0f,
        0.22f, 0.22f, 0.28f, 1.0f,
    };
    bgfx_set_uniform(sr->u_sky_colors, sky_colors, 3);

    bgfx_texture_handle_t equirect_tex = { UINT16_MAX };
    float sky_params[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (sr->skybox_active && sr->skybox) {
        JceTexture jet = jce_skybox_get_equirect_texture(sr->skybox);
        equirect_tex.idx = jet.idx;
        if (BGFX_HANDLE_IS_VALID(equirect_tex)) {
            sky_params[0] = 1.0f;
            sky_params[1] = sr->skybox_exposure;
            sky_params[2] = sr->skybox_rotation * 0.0174533f;
            bgfx_set_texture(0, sr->u_sky_equirect, equirect_tex, UINT32_MAX);
        }
    }
    bgfx_set_uniform(sr->u_sky_params, sky_params, 1);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(view_id, sr->prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Shadow pass ──────────────────────────────────────────────────── */

static void sr_draw_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                const JceCamera *camera, EntityList *list,
                                uint16_t view_id_base, uint32_t vp_w, uint32_t vp_h)
{
    sr->shadow_use_csm = false;
    sr->last_csm_valid = false;

    /* Default uniforms even if shadows aren't rendered. */
    {
        float disabled_splits[4] = { 0, 0, 0, 0 };
        float csm_params[4] = {
            sr->shadow_map_size > 0 ? 1.0f / (float)sr->shadow_map_size : 0.0f,
            sr->csm_blend_ratio,
            sr->csm_normal_bias,
            sr->csm_filter_radius,
        };
        float bias_scales[4] = { 1, 1, 1, 1 };
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_splits))
            bgfx_set_uniform(sr->u_csm_splits, disabled_splits, 1);
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_params))
            bgfx_set_uniform(sr->u_csm_params, csm_params, 1);
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_bias_scales))
            bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);
    }

    if (!sr->shadow_valid) return;

    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(sr->renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    const bool use_csm = sr->csm_valid && sr->csm_cascade_count > 0;
    jce_vec3 shadow_dir = sr_resolve_shadow_light_direction(scene, list);

    /* Compute shadow view IDs from base. */
    const uint16_t shadow_view_0 = (uint16_t)(view_id_base + 10);

    if (!use_csm) {
        float shadow_vp[16];
        sr_compute_shadow_vp(sr, &shadow_dir, shadow_vp);

        bgfx_set_view_rect(shadow_view_0, 0, 0,
                           sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(shadow_view_0, sr->shadow_fbo);
        bgfx_set_view_clear(shadow_view_0, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bgfx_set_view_transform(shadow_view_0, identity, shadow_vp);
        bgfx_set_uniform(sr->u_shadowVP, shadow_vp, 1);

        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
            if (!mesh) continue;
            bgfx_set_transform(model.raw[0], 1);
            jce_mesh_submit_shadow(mesh, sr->renderer, shadow_view_0);
        }
        return;
    }

    sr->shadow_use_csm = true;

    /* CSM cascades. */
    float cam_near = camera ? jce_camera_get_near(camera) : 0.1f;
    float cam_far  = camera ? jce_camera_get_far(camera)  : 200.0f;
    float cam_fov  = camera ? jce_camera_get_fov(camera)  : 45.0f;
    float aspect   = (vp_w > 0 && vp_h > 0)
        ? ((float)vp_w / (float)vp_h) : (16.0f / 9.0f);

    if (cam_near <= 0.0f) cam_near = 0.1f;
    if (cam_far <= cam_near) cam_far = cam_near + 200.0f;

    float shadow_far_target = cam_far;
    shadow_far_target = fminf(shadow_far_target,
        fmaxf(cam_near * CSM_DIST_SCALE, CSM_DIST_MAX));
    shadow_far_target = fmaxf(shadow_far_target, cam_near + CSM_DIST_MIN);

    if (!sr->shadow_far_valid) {
        sr->shadow_far_cached = shadow_far_target;
        sr->shadow_far_valid = true;
    } else {
        float far_delta = fabsf(shadow_far_target - sr->shadow_far_cached);
        float far_rel = far_delta / fmaxf(sr->shadow_far_cached, CSM_DIST_MIN);
        if (far_delta > CSM_FAR_HYST_ABS && far_rel > CSM_FAR_HYST_REL)
            sr->shadow_far_cached = shadow_far_target;
    }
    float shadow_far = sr->shadow_far_cached;

    jce_mat4 cam_view = camera ? jce_camera_view(camera) : jce_m4_identity();
    jce_vec3 light_dir = shadow_dir;

    JceCsmData csm;
    jce_csm_compute(&csm, sr->csm_cascade_count,
                    cam_near, shadow_far, cam_fov, aspect,
                    &cam_view, &light_dir,
                    sr->homogeneous_depth, sr->shadow_map_size);

    sr->last_csm = csm;
    sr->last_csm_valid = true;

    for (uint32_t c = 0; c < csm.cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
        uint16_t cv = (uint16_t)(view_id_base + 11 + c);

        bgfx_set_view_rect(cv, 0, 0, sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(cv, sr->csm_fbo[c]);
        bgfx_set_view_clear(cv, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bgfx_set_view_transform(cv, identity, csm.vp[c].raw[0]);

        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
            if (!mesh) continue;
            bgfx_set_transform(model.raw[0], 1);
            jce_mesh_submit_shadow(mesh, sr->renderer, cv);
        }
    }

    /* Upload CSM uniforms. */
    bgfx_set_uniform(sr->u_csm_vp, csm.vp[0].raw[0], (uint16_t)csm.cascade_count);

    float splits_v4[4] = {
        csm.cascade_count > 0 ? csm.splits[1] : shadow_far,
        csm.cascade_count > 1 ? csm.splits[2] : shadow_far,
        csm.cascade_count > 2 ? csm.splits[3] : shadow_far,
        csm.cascade_count > 3 ? csm.splits[4] : shadow_far,
    };
    bgfx_set_uniform(sr->u_csm_splits, splits_v4, 1);

    float csm_params[4] = {
        1.0f / (float)sr->shadow_map_size,
        sr->csm_blend_ratio,
        sr->csm_normal_bias,
        sr->csm_filter_radius,
    };
    bgfx_set_uniform(sr->u_csm_params, csm_params, 1);

    float bias_scales[4];
    sr_fill_csm_bias_scales(&csm, bias_scales);
    bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);
}

/* ── Skybox scan ──────────────────────────────────────────────────── */

static void sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list)
{
    const char *hdr_path = NULL;
    float rotation = 0.0f;
    float exposure = 1.0f;

    for (int i = 0; i < list->count && !hdr_path; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skybox(scene, e)) continue;
        JceSkyboxComponent *c = jce_scene_get_skybox(scene, e);
        if (!c || c->hdr_path[0] == '\0') continue;
        hdr_path = c->hdr_path;
        rotation = c->rotation;
        exposure = c->exposure > 0.0f ? c->exposure : 1.0f;
    }

    if (hdr_path && strcmp(hdr_path, sr->skybox_hdr_path) != 0) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        sr->skybox = jce_skybox_create_from_hdr_file(hdr_path, 512);
        if (sr->skybox) {
            snprintf(sr->skybox_hdr_path, sizeof(sr->skybox_hdr_path),
                     "%s", hdr_path);
            sr->skybox_active = true;
            JceTexture eq = jce_skybox_get_equirect_texture(sr->skybox);
            sr->ibl_data = jce_ibl_generate(eq, 32, 128, 256);
            LOG_INFO(LOG_TAG, "skybox loaded: %s", hdr_path);
        } else {
            sr->skybox_hdr_path[0] = '\0';
            sr->skybox_active = false;
        }
    } else if (!hdr_path && sr->skybox_active) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        sr->skybox_hdr_path[0] = '\0';
        sr->skybox_active = false;
    }

    sr->skybox_exposure = exposure;
    sr->skybox_rotation = rotation;
}

/* ── Entity rendering ─────────────────────────────────────────────── */

static void sr_draw_entities(JceSceneRenderer *sr, JceScene *scene,
                             const JceCamera *camera, EntityList *list,
                             uint16_t view_id, float dt_sec,
                             const JceSceneRenderConfig *cfg)
{
    if (list->count == 0) return;

    /* Gather lights. */
    if (sr->light_env) {
        jce_light_env_clear(sr->light_env);
        jce_light_env_set_ambient(sr->light_env, jce_v3(1, 1, 1), 0.15f);

        bool has_any_light = false;
        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            JceTransform *xf = jce_scene_get_transform(scene, e);

            if (jce_scene_has_dir_light(scene, e)) {
                JceDirectionalLight *dlc = jce_scene_get_dir_light(scene, e);
                if (dlc) {
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = dlc->color;
                    dl.intensity = dlc->intensity > 0.0f ? dlc->intensity : 1.0f;
                    dl.direction = sr_light_shine_direction(&dlc->direction);
                    jce_light_env_add_dir_light(sr->light_env, &dl);
                    has_any_light = true;
                }
            }
            if (jce_scene_has_point_light(scene, e)) {
                JcePointLight *plc = jce_scene_get_point_light(scene, e);
                if (plc) {
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = plc->color;
                    pl.intensity = plc->intensity > 0.0f ? plc->intensity : 1.0f;
                    pl.radius    = plc->radius    > 0.0f ? plc->radius    : 10.0f;
                    pl.position  = xf ? xf->position : plc->position;
                    jce_light_env_add_point_light(sr->light_env, &pl);
                    has_any_light = true;
                }
            }
            if (jce_scene_has_spot_light(scene, e)) {
                JceSpotLight *slc = jce_scene_get_spot_light(scene, e);
                if (slc) {
                    JceSpotLightDesc sl;
                    memset(&sl, 0, sizeof(sl));
                    sl.color = slc->color;
                    sl.intensity = slc->intensity > 0.0f ? slc->intensity : 1.0f;
                    sl.radius    = slc->radius    > 0.0f ? slc->radius    : 10.0f;
                    sl.inner_cone_cos = slc->inner_cone_cos;
                    sl.outer_cone_cos = slc->outer_cone_cos;
                    sl.position  = xf ? xf->position : slc->position;
                    sl.direction = sr_light_shine_direction(&slc->direction);
                    jce_light_env_add_spot_light(sr->light_env, &sl);
                    has_any_light = true;
                }
            }
        }

        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3(-0.5f, -1.0f, -0.3f);
            dl.color = jce_v3(1, 1, 1);
            dl.intensity = 1.0f;
            jce_light_env_add_dir_light(sr->light_env, &dl);
        }

        if (camera) {
            jce_vec3 cp = jce_camera_get_position(camera);
            jce_light_env_set_camera_pos(sr->light_env, cp);
        }
        jce_light_env_apply(sr->light_env, sr->renderer);
    }

    JceDirLight sun = jce_dir_light_default();
    sun.direction = sr_resolve_shadow_light_direction(scene, list);
    jce_lighting_apply(sr->renderer, &sun);

    /* WIREFRAME_TEXTURED debug view: trigger fs_mesh.sc hue-Lambert branch by
     * setting u_lightDir.w = 0.25 (matches 0.5.7 behaviour). Done after
     * jce_lighting_apply since that overwrites these uniforms. */
    if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED &&
        sr->has_cbs && sr->cbs.load_texture)
    {
        jce_vec3 sd = sun.direction;
        float len = sqrtf(sd.x * sd.x + sd.y * sd.y + sd.z * sd.z);
        if (len > 1e-6f) { sd.x /= len; sd.y /= len; sd.z /= len; }
        float wf_dir[4]   = { sd.x, sd.y, sd.z, 0.25f };
        float wf_color[4] = { 1.0f, 1.0f, 1.0f, 0.2f };
        bgfx_set_uniform(sr->u_light_dir,   wf_dir,   1);
        bgfx_set_uniform(sr->u_light_color, wf_color, 1);
    }

    /* Begin sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites)
        jce_sprite_batch_begin(sr->sprite_batch);

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;

        /* ── Skinned/animated path ───────────────────────────────── */
        if (jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
            if (sa && sa->skeleton_path[0]) {
                SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
                if (mc && mc->model) {
                    if (mc->player) {
                        int ac = sa->active_clip;
                        float sp = sa->speed > 0.0f ? sa->speed : 1.0f;
                        JceAnimClip *clip = NULL;
                        if (ac >= 0 && ac < (int)jce_model_anim_count(mc->model))
                            clip = jce_model_get_anim(mc->model, (uint32_t)ac);

                        bool comp_playing = sa->playing;
                        bool clip_changed = (mc->active_clip != ac);
                        bool loop_changed = (mc->loop != sa->loop);
                        bool speed_changed = fabsf(mc->speed - sp) > 0.0001f;
                        bool paused_changed = (mc->paused == comp_playing);

                        if (comp_playing && clip) {
                            if (!jce_anim_player_is_playing(mc->player)
                                || clip_changed || loop_changed) {
                                jce_anim_player_play(mc->player, clip, sa->loop, sp);
                            } else if (speed_changed || paused_changed) {
                                jce_anim_player_set_speed(mc->player, sp);
                            }
                            jce_anim_player_pause(mc->player, false);
                            jce_anim_player_set_speed(mc->player, sp);
                        } else {
                            if (clip && (clip_changed || loop_changed)) {
                                jce_anim_player_play(mc->player, clip, sa->loop, sp);
                                jce_anim_player_set_time(mc->player, 0.0f);
                            }
                            if (jce_anim_player_is_playing(mc->player))
                                jce_anim_player_pause(mc->player, true);
                        }

                        mc->active_clip = ac;
                        mc->loop = sa->loop;
                        mc->speed = sp;
                        mc->paused = !comp_playing;

                        jce_mat4 joints[64];
                        uint32_t nj = jce_anim_player_update(mc->player,
                                                             dt_sec, joints, 64);
                        jce_model_draw(mc->model, sr->renderer, view_id, &model,
                                       nj > 0 ? joints : NULL, nj);
                    } else {
                        jce_model_draw(mc->model, sr->renderer, view_id,
                                       &model, NULL, 0);
                    }
                    continue;
                }
            }
        }

        /* ── Sprite path ─────────────────────────────────────────── */
        if (cfg->draw_sprites && sr->sprite_batch &&
            jce_scene_has_sprite_renderer(scene, e)) {
            JceSpriteRendererComponent *spr = jce_scene_get_sprite_renderer(scene, e);
            if (spr) {
                bgfx_texture_handle_t st = { UINT16_MAX };
                if (spr->sprite_path[0]) {
                    JceTexture t = sr_resolve_texture(sr, spr->sprite_path);
                    if (jce_texture_valid(t)) st.idx = t.idx;
                }
                if (!BGFX_HANDLE_IS_VALID(st)) st = sr->white_tex;

                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                if (spr->flip_x) { float tmp = u0; u0 = u1; u1 = tmp; }
                if (spr->flip_y) { float tmp = v0; v0 = v1; v1 = tmp; }

                const float *sc = spr->color;
                uint8_t r8 = (uint8_t)(sc[0] * 255.0f);
                uint8_t g8 = (uint8_t)(sc[1] * 255.0f);
                uint8_t b8 = (uint8_t)(sc[2] * 255.0f);
                uint8_t a8 = (uint8_t)(sc[3] * 255.0f);
                uint32_t abgr = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16)
                              | ((uint32_t)g8 << 8) | (uint32_t)r8;

                JceTexture sjt; sjt.idx = st.idx;
                jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                     u0, v0, u1, v1, abgr, spr->sorting_order);
                continue;
            }
        }

        if (!mesh || !cfg->draw_opaque) continue;
        bgfx_set_transform(model.raw[0], 1);

        /* ── PBR mesh path ───────────────────────────────────────── */
        JceMeshRenderer *mr_comp = jce_scene_has_mesh_renderer(scene, e)
            ? jce_scene_get_mesh_renderer(scene, e) : NULL;

        /* ── View-mode dispatch (mirrors the 0.5.7 editor exactly):
         *      SHADED                : PBR factors (no textures), full lighting
         *      TEXTURED              : PBR + all textures, checker fallback
         *      WIREFRAME             : simple-mesh shader, white tint, flat Lambert
         *      WIREFRAME_TEXTURED    : simple-mesh shader, albedo tint, hue-Lambert
         *
         * Only the editor (callback mode) uses these debug modes; runtime
         * always renders PBR with whatever textures the asset specifies. */
        bool editor_mode = (sr->has_cbs && sr->cbs.load_texture);
        bool wireframe_path = editor_mode &&
            (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME ||
             cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED);

        if (mr_comp && !wireframe_path) {
            JcePbrMaterial pbr = jce_pbr_material_default();
            bool use_checker_fallback = false;
            if (mr_comp->base_color[3] > 0.0f) {
                pbr.base_color_factor[0] = mr_comp->base_color[0];
                pbr.base_color_factor[1] = mr_comp->base_color[1];
                pbr.base_color_factor[2] = mr_comp->base_color[2];
                pbr.base_color_factor[3] = mr_comp->base_color[3];
            }
            pbr.metallic_factor      = mr_comp->metallic;
            pbr.roughness_factor     = mr_comp->roughness;
            pbr.emissive_factor[0]   = mr_comp->emissive[0];
            pbr.emissive_factor[1]   = mr_comp->emissive[1];
            pbr.emissive_factor[2]   = mr_comp->emissive[2];
            pbr.normal_scale         = mr_comp->normal_scale;
            pbr.ao_strength          = mr_comp->ao_strength;
            pbr.alpha_mode           = (JceAlphaMode)mr_comp->alpha_mode;
            pbr.alpha_cutoff         = mr_comp->alpha_cutoff;
            pbr.double_sided         = mr_comp->double_sided;

            /* Editor SHADED mode: skip texture loading entirely (factors only).
             * Runtime / TEXTURED: load all maps. */
            bool load_tex =
                !editor_mode ||
                cfg->view_mode == JCE_SCENE_VIEW_TEXTURED;

            if (load_tex) {
                if (mr_comp->albedo_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->albedo_tex);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* 0.5.5-compatible fallback: derive the texture from the
                 * material file (.mat.json's albedoMap) or the mesh's
                 * sidecar (.obj's MTL map_Kd). Both paths go into ONE
                 * cache entry to avoid duplicate async filesystem walks. */
                if (!jce_texture_valid(pbr.albedo_map) &&
                    (mr_comp->material_path[0] || mr_comp->mesh_path[0])) {
                    JceTexture t = sr_resolve_texture2(sr,
                        mr_comp->material_path[0] ? mr_comp->material_path : NULL,
                        mr_comp->mesh_path[0]     ? mr_comp->mesh_path     : NULL);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* TEXTURED mode + missing albedo → trigger pink-checker shader fallback. */
                if (editor_mode && cfg->view_mode == JCE_SCENE_VIEW_TEXTURED &&
                    !jce_texture_valid(pbr.albedo_map)) {
                    use_checker_fallback = true;
                }
                if (mr_comp->mr_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->mr_tex);
                    if (jce_texture_valid(t)) pbr.metallic_roughness_map = t;
                }
                if (mr_comp->normal_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->normal_tex);
                    if (jce_texture_valid(t)) pbr.normal_map = t;
                }
                if (mr_comp->ao_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->ao_tex);
                    if (jce_texture_valid(t)) pbr.ao_map = t;
                }
                if (mr_comp->emissive_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->emissive_tex);
                    if (jce_texture_valid(t)) pbr.emissive_map = t;
                }
            }

            /* Encode the checker-fallback flag in normal_scale sign (the
             * fs_pbr.sc shader checks `u_normalScale.x < 0`). */
            if (use_checker_fallback)
                pbr.normal_scale = -fmaxf(fabsf(pbr.normal_scale), 0.0001f);
            else
                pbr.normal_scale = fabsf(pbr.normal_scale);

            jce_pbr_material_bind(&pbr, sr->renderer, view_id);

            if (sr->shadow_valid && !sr->shadow_use_csm) {
                bgfx_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
                float shadow_vp[16];
                jce_vec3 shadow_dir = sr_resolve_shadow_light_direction(scene, list);
                sr_compute_shadow_vp(sr, &shadow_dir, shadow_vp);
                bgfx_set_uniform(sr->u_shadowVP, shadow_vp, 1);
            }
            if (sr->csm_valid && sr->shadow_use_csm) {
                for (uint32_t ci = 0;
                     ci < sr->csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
                    bgfx_set_texture((uint8_t)(9 + ci), sr->u_csm_samplers[ci],
                                     sr->csm_tex[ci], UINT32_MAX);
            }

            float ibl_params[4] = {
                0.0f, 5.0f, 0.0f,
                sr->postfx_tonemap_active ? 1.0f : 0.0f
            };
            if (sr->skybox_active && sr->ibl_data) {
                JceTexture irr = jce_ibl_get_irradiance(sr->ibl_data);
                JceTexture pf  = jce_ibl_get_prefilter(sr->ibl_data);
                bgfx_texture_handle_t hi = { irr.idx };
                bgfx_texture_handle_t hp = { pf.idx };
                if (BGFX_HANDLE_IS_VALID(hi) && BGFX_HANDLE_IS_VALID(hp)
                    && BGFX_HANDLE_IS_VALID(sr->brdf_lut)) {
                    bgfx_set_texture(6, sr->u_ibl_irradiance, hi, UINT32_MAX);
                    bgfx_set_texture(7, sr->u_ibl_prefilter,  hp, UINT32_MAX);
                    bgfx_set_texture(8, sr->u_ibl_brdf_lut, sr->brdf_lut, UINT32_MAX);
                    ibl_params[0] = 1.0f;
                }
            }
            bgfx_set_uniform(sr->u_ibl_params, ibl_params, 1);

            jce_mesh_submit_pbr(mesh, sr->renderer, view_id);
        } else {
            /* Simple mesh shader path. Used for:
             *   - WIREFRAME / WIREFRAME_TEXTURED (editor debug views)
             *   - entities without a MeshRenderer component (white blob) */
            bgfx_texture_handle_t bind_tex = sr->white_tex;

            if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED && mr_comp)
            {
                JceTexture t = { UINT16_MAX };
                if (mr_comp->albedo_tex[0])
                    t = sr_resolve_texture(sr, mr_comp->albedo_tex);
                if (!jce_texture_valid(t) &&
                    (mr_comp->material_path[0] || mr_comp->mesh_path[0]))
                    t = sr_resolve_texture2(sr,
                        mr_comp->material_path[0] ? mr_comp->material_path : NULL,
                        mr_comp->mesh_path[0]     ? mr_comp->mesh_path     : NULL);
                if (jce_texture_valid(t)) bind_tex.idx = t.idx;
            }

            /* TEXTURED mode + entity with no MR component (or no albedo path):
             * fall back to the magenta/yellow checker so missing assets are
             * visually obvious. */
            if (editor_mode && cfg->view_mode == JCE_SCENE_VIEW_TEXTURED &&
                !mr_comp && BGFX_HANDLE_IS_VALID(sr->checker_tex))
            {
                bind_tex = sr->checker_tex;
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(sr->renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
            jce_mesh_submit(mesh, sr->renderer, view_id);
        }
    }

    /* Flush sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites &&
        jce_sprite_batch_count(sr->sprite_batch) > 0)
        jce_sprite_batch_flush(sr->sprite_batch, sr->renderer, view_id);
}

/* ── Public API ───────────────────────────────────────────────────── */

JceSceneRenderConfig jce_scene_render_config_default(void)
{
    JceSceneRenderConfig c;
    memset(&c, 0, sizeof(c));
    c.draw_skybox      = true;
    c.draw_shadows     = true;
    c.draw_opaque      = true;
    c.draw_sprites     = true;
    c.draw_transparent = true;
    c.apply_postfx     = true;
    c.postfx           = jce_postfx_default_params();
    c.shadow_map_size  = 0;
    c.csm_cascades     = 0;
    return c;
}

JceSceneRenderer *jce_scene_renderer_create(JceRenderer *renderer,
                                            const JcePakArchive *pak,
                                            const JceSceneRendererCallbacks *cbs)
{
    if (!renderer) return NULL;

    JceSceneRenderer *sr = (JceSceneRenderer *)calloc(1, sizeof(JceSceneRenderer));
    if (!sr) return NULL;

    sr->renderer = renderer;
    sr->pak = pak;
    if (cbs) { sr->cbs = *cbs; sr->has_cbs = true; }

    /* Invalidate handles. */
    sr->white_tex.idx       = UINT16_MAX;
    sr->checker_tex.idx     = UINT16_MAX;
    sr->prog_sky.idx        = UINT16_MAX;
    sr->u_sky_colors.idx    = UINT16_MAX;
    sr->u_sky_params.idx    = UINT16_MAX;
    sr->u_sky_equirect.idx  = UINT16_MAX;
    sr->u_light_dir.idx     = UINT16_MAX;
    sr->u_light_color.idx   = UINT16_MAX;
    sr->shadow_tex.idx      = UINT16_MAX;
    sr->shadow_fbo.idx      = UINT16_MAX;
    sr->u_shadowMap.idx     = UINT16_MAX;
    sr->u_shadowVP.idx      = UINT16_MAX;
    sr->u_csm_vp.idx        = UINT16_MAX;
    sr->u_csm_splits.idx    = UINT16_MAX;
    sr->u_csm_params.idx    = UINT16_MAX;
    sr->u_csm_bias_scales.idx = UINT16_MAX;
    sr->brdf_lut.idx        = UINT16_MAX;
    sr->u_ibl_irradiance.idx = UINT16_MAX;
    sr->u_ibl_prefilter.idx  = UINT16_MAX;
    sr->u_ibl_brdf_lut.idx   = UINT16_MAX;
    sr->u_ibl_params.idx     = UINT16_MAX;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        sr->csm_tex[i].idx = UINT16_MAX;
        sr->csm_fbo[i].idx = UINT16_MAX;
        sr->u_csm_samplers[i].idx = UINT16_MAX;
    }

    const bgfx_caps_t *caps = bgfx_get_caps();
    sr->homogeneous_depth = caps ? caps->homogeneousDepth : false;

    /* Pick best depth format. */
    bgfx_texture_format_t depth_fmt = BGFX_TEXTURE_FORMAT_D16;
    if (caps) {
        uint16_t d32f = caps->formats[BGFX_TEXTURE_FORMAT_D32F];
        uint16_t d24  = caps->formats[BGFX_TEXTURE_FORMAT_D24S8];
        if ((d32f & BGFX_CAPS_FORMAT_TEXTURE_2D)
            && (d32f & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            depth_fmt = BGFX_TEXTURE_FORMAT_D32F;
        else if ((d24 & BGFX_CAPS_FORMAT_TEXTURE_2D)
                 && (d24 & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            depth_fmt = BGFX_TEXTURE_FORMAT_D24S8;
    }

    /* GPU-tier-driven shadow defaults. */
    {
        JceRenderRecommendation rec = jce_renderer_get_recommendation();
        uint32_t sz = rec.shadow_map_size;
        if (sz < 1024) sz = 1024;
        if (sz > 4096) sz = 4096;
        sr->shadow_map_size = (uint16_t)sz;

        switch (rec.tier) {
        case JCE_GPU_TIER_HIGH:
            sr->csm_blend_ratio   = 0.22f;
            sr->csm_normal_bias   = 0.015f;
            sr->csm_filter_radius = 1.6f;
            break;
        case JCE_GPU_TIER_MEDIUM:
            sr->csm_blend_ratio   = 0.20f;
            sr->csm_normal_bias   = 0.012f;
            sr->csm_filter_radius = 1.4f;
            break;
        case JCE_GPU_TIER_LOW:
        default:
            sr->csm_blend_ratio   = 0.18f;
            sr->csm_normal_bias   = 0.010f;
            sr->csm_filter_radius = 1.2f;
            break;
        }
    }

    /* Sky vertex layout & shader. */
    bgfx_vertex_layout_begin(&sr->sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&sr->sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&sr->sky_layout);

    {
        JceShaderHandle sky_sh = shader_load_program(pak, "sky");
        sr->prog_sky.idx = sky_sh.idx;
        if (sky_sh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "sky shader not found in PAK");
    }

    sr->u_sky_colors   = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);
    sr->u_sky_params   = bgfx_create_uniform("u_sky_params",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_sky_equirect = bgfx_create_uniform("s_equirect",
                                             BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_light_dir   = bgfx_create_uniform("u_lightDir",
                                            BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_light_color = bgfx_create_uniform("u_lightColor",
                                            BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Procedural meshes. */
    sr->cube_mesh     = jce_mesh_create_cube(1.0f);
    sr->plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    sr->sphere_mesh   = jce_mesh_create_sphere(0.5f);
    sr->capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    sr->cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    /* 1×1 white fallback texture. */
    {
        uint32_t white = 0xFFFFFFFFu;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        sr->white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                               BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 8×8 magenta/yellow "missing texture" checkerboard (TEXTURED-mode
     * fallback when an entity has no albedo). ABGR pixel layout. */
    {
        const uint32_t MAG = 0xFFFF00FFu; /* alpha=FF, r=FF, g=00, b=FF → magenta */
        const uint32_t YEL = 0xFF00FFFFu; /* alpha=FF, r=FF, g=FF, b=00 → yellow */
        uint32_t pix[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                pix[y * 8 + x] = (((x ^ y) >> 1) & 1) ? MAG : YEL;
        const bgfx_memory_t *mem = bgfx_copy(pix, sizeof(pix));
        sr->checker_tex = bgfx_create_texture_2d(8, 8, false, 1,
                                                 BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* Shadow map (legacy single-cascade). */
    {
        const uint16_t sz = sr->shadow_map_size;
        sr->shadow_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL);
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, sr->shadow_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
        sr->shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        sr->u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                              BGFX_UNIFORM_TYPE_SAMPLER, 1);
        sr->u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                              BGFX_UNIFORM_TYPE_MAT4, 1);
        sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->shadow_fbo);
    }

    /* CSM cascades. */
    {
        const uint16_t sz = sr->shadow_map_size;
        const char *names[JCE_CSM_MAX_CASCADES] = {
            "s_csmShadow0", "s_csmShadow1", "s_csmShadow2", "s_csmShadow3"
        };
        sr->csm_cascade_count = JCE_CSM_MAX_CASCADES;
        sr->csm_valid = true;
        for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
            sr->csm_tex[i] = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
                BGFX_TEXTURE_RT
                | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                NULL);
            bgfx_attachment_t at;
            memset(&at, 0, sizeof(at));
            bgfx_attachment_init(&at, sr->csm_tex[i], BGFX_ACCESS_WRITE,
                                 0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
            sr->csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
            sr->u_csm_samplers[i] = bgfx_create_uniform(names[i],
                BGFX_UNIFORM_TYPE_SAMPLER, 1);
            if (!BGFX_HANDLE_IS_VALID(sr->csm_fbo[i])) sr->csm_valid = false;
        }
        sr->u_csm_vp = bgfx_create_uniform("u_csmVP",
            BGFX_UNIFORM_TYPE_MAT4, JCE_CSM_MAX_CASCADES);
        sr->u_csm_splits      = bgfx_create_uniform("u_csmSplits",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        sr->u_csm_params      = bgfx_create_uniform("u_csmParams",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        sr->u_csm_bias_scales = bgfx_create_uniform("u_csmBiasScales",
            BGFX_UNIFORM_TYPE_VEC4, 1);
    }

    /* Multi-light env. */
    sr->light_env = jce_light_env_create();

    /* IBL uniforms + BRDF LUT. */
    sr->u_ibl_irradiance = bgfx_create_uniform("s_irradiance",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_prefilter  = bgfx_create_uniform("s_prefilter",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_brdf_lut   = bgfx_create_uniform("s_brdfLUT",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_params     = bgfx_create_uniform("u_iblParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    {
        JceTexture brdf = jce_ibl_create_brdf_lut(256);
        sr->brdf_lut.idx = brdf.idx;
    }
    sr->skybox = NULL;
    sr->ibl_data = NULL;
    sr->skybox_active = false;
    sr->skybox_hdr_path[0] = '\0';

    /* Sprite batch. */
    sr->sprite_batch = jce_sprite_batch_create(256);

    /* PostFX pipeline. */
    sr->postfx_pipeline = jce_postfx_create(jce_allocator_default(), 1, 1);
    if (sr->postfx_pipeline) {
        if (!jce_postfx_load_shaders(sr->postfx_pipeline, pak))
            LOG_WARN(LOG_TAG, "postfx shaders failed to load");
    }

    LOG_INFO(LOG_TAG, "scene renderer created");
    return sr;
}

void jce_scene_renderer_destroy(JceSceneRenderer *sr)
{
    if (!sr) return;

    /* Model cache. */
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (!e->used) continue;
        if (e->player) jce_anim_player_destroy(e->player);
        if (e->model)  jce_model_destroy(e->model);
        e->used = false;
    }

    if (sr->cube_mesh)     jce_mesh_destroy(sr->cube_mesh);
    if (sr->plane_mesh)    jce_mesh_destroy(sr->plane_mesh);
    if (sr->sphere_mesh)   jce_mesh_destroy(sr->sphere_mesh);
    if (sr->capsule_mesh)  jce_mesh_destroy(sr->capsule_mesh);
    if (sr->cylinder_mesh) jce_mesh_destroy(sr->cylinder_mesh);

    if (BGFX_HANDLE_IS_VALID(sr->white_tex))      bgfx_destroy_texture(sr->white_tex);
    if (BGFX_HANDLE_IS_VALID(sr->checker_tex))    bgfx_destroy_texture(sr->checker_tex);
    if (BGFX_HANDLE_IS_VALID(sr->prog_sky))       bgfx_destroy_program(sr->prog_sky);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_colors))   bgfx_destroy_uniform(sr->u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_params))   bgfx_destroy_uniform(sr->u_sky_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_equirect)) bgfx_destroy_uniform(sr->u_sky_equirect);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_dir))    bgfx_destroy_uniform(sr->u_light_dir);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_color))  bgfx_destroy_uniform(sr->u_light_color);

    if (BGFX_HANDLE_IS_VALID(sr->shadow_fbo))  bgfx_destroy_frame_buffer(sr->shadow_fbo);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowMap)) bgfx_destroy_uniform(sr->u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowVP))  bgfx_destroy_uniform(sr->u_shadowVP);

    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->csm_fbo[i]))
            bgfx_destroy_frame_buffer(sr->csm_fbo[i]);
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_samplers[i]))
            bgfx_destroy_uniform(sr->u_csm_samplers[i]);
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_vp))         bgfx_destroy_uniform(sr->u_csm_vp);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_splits))     bgfx_destroy_uniform(sr->u_csm_splits);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_params))     bgfx_destroy_uniform(sr->u_csm_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_bias_scales))bgfx_destroy_uniform(sr->u_csm_bias_scales);

    if (sr->light_env) jce_light_env_destroy(sr->light_env);
    if (sr->ibl_data)  jce_ibl_destroy(sr->ibl_data);
    if (sr->skybox)    jce_skybox_destroy(sr->skybox);

    if (BGFX_HANDLE_IS_VALID(sr->brdf_lut))         bgfx_destroy_texture(sr->brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_irradiance)) bgfx_destroy_uniform(sr->u_ibl_irradiance);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_prefilter))  bgfx_destroy_uniform(sr->u_ibl_prefilter);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_brdf_lut))   bgfx_destroy_uniform(sr->u_ibl_brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_params))     bgfx_destroy_uniform(sr->u_ibl_params);

    if (sr->sprite_batch)    jce_sprite_batch_destroy(sr->sprite_batch);
    if (sr->postfx_pipeline) jce_postfx_destroy(sr->postfx_pipeline);

    free(sr);
}

uint16_t jce_scene_renderer_render(JceSceneRenderer *sr, JceScene *scene,
                                   const JceCamera *camera,
                                   uint16_t view_id_base, float dt_sec,
                                   const JceSceneRenderConfig *config)
{
    if (!sr || !scene) return view_id_base;

    JceSceneRenderConfig defcfg = jce_scene_render_config_default();
    const JceSceneRenderConfig *cfg = config ? config : &defcfg;

    /* Apply optional config overrides. */
    if (cfg->shadow_map_size != 0)
        sr->shadow_map_size = cfg->shadow_map_size;
    if (cfg->csm_cascades != 0)
        sr->csm_cascade_count =
            cfg->csm_cascades < JCE_CSM_MAX_CASCADES
            ? cfg->csm_cascades : JCE_CSM_MAX_CASCADES;

    /* Ensure wireframe is OFF before sky draws (sky's fullscreen quad must
     * render solid). The previous frame may have left it ON. Editor mode
     * only — runtime games own their own wireframe state. */
    if (sr->has_cbs && sr->cbs.load_texture)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* PBR view_mode shader branch was an experiment; the editor's TEXTURED
     * mode actually wants PBR LIGHTING with textures (matching the old
     * behavior), not unlit albedo. Pass 0 so the unlit branch stays
     * dormant; texture loading is gated above instead. */
    jce_pbr_material_set_view_mode(0);

    /* Push postfx params. */
    if (sr->postfx_pipeline)
        jce_postfx_set_params(sr->postfx_pipeline, &cfg->postfx);

    sr->postfx_tonemap_active = false;
    if (sr->postfx_pipeline)
        sr->postfx_tonemap_active =
            jce_postfx_is_enabled(sr->postfx_pipeline, JCE_POSTFX_TONEMAP);

    /* Collect entities. */
    EntityList list;
    list.count = 0;
    jce_scene_each_entity(scene, collect_entity_cb, &list);

    /* Skybox scan + IBL refresh. */
    sr_scan_skybox(sr, scene, &list);

    /* Sky pass into the main view.
     * Mirrors 0.5.7 gating: in plain WIREFRAME mode skip sky entirely;
     * in WIREFRAME_TEXTURED skip sky unless an HDR skybox is active. */
    bool sky_drawn = false;
    if (cfg->draw_skybox) {
        bool gate_ok = true;
        if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME)
            gate_ok = false;
        else if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED
                 && !sr->skybox_active)
            gate_ok = false;
        if (gate_ok) {
            sr_draw_sky_gradient(sr, view_id_base);
            sky_drawn = true;
        }
    }

    /* Editor overlay hook: invoked between sky and entities. Used by
     * the editor to draw the world grid behind scene geometry, matching
     * 0.5.7 ordering. Runtime games leave on_after_sky == NULL. */
    if (cfg->on_after_sky)
        cfg->on_after_sky(view_id_base, sky_drawn, cfg->on_after_sky_ud);

    /* Shadow passes (do them BEFORE entity pass so PBR can sample). */
    if (cfg->draw_shadows) {
        /* Determine viewport from camera bounds — if absent, assume 16:9. */
        uint32_t vp_w = 1920, vp_h = 1080;
        sr_draw_shadow_pass(sr, scene, camera, &list, view_id_base, vp_w, vp_h);
    } else {
        sr->shadow_use_csm = false;
        sr->last_csm_valid = false;
    }

    /* Apply view-mode wireframe via the renderer. CRITICAL: this MUST come
     * AFTER the sky and shadow passes, otherwise the sky's fullscreen quad
     * would be rendered as wireframe lines (looks like the sky is broken).
     * Mirrors 0.5.7 ordering exactly. */
    bool editor_wf_set = false;
    if (sr->has_cbs && sr->cbs.load_texture) {
        bool want_wf = (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME ||
                        cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED);
        jce_renderer_set_wireframe(sr->renderer, want_wf);
        editor_wf_set = want_wf;
    }

    /* Entity rendering. */
    sr_draw_entities(sr, scene, camera, &list, view_id_base, dt_sec, cfg);

    /* Reset wireframe so subsequent overlay passes (grid, selection) draw
     * solid. Mirrors 0.5.7 line 914 exactly. bgfx_set_debug() takes effect
     * per-submission, so submits between set(true) and set(false) draw as
     * wireframe; submits after set(false) draw solid. */
    if (editor_wf_set)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* PostFX pass — engine-level postfx is currently a no-op since we
       don't own an output FBO here. The caller drives final composition.
       We expose tonemap state through the IBL params binding so PBR
       output remains consistent. */
    (void)cfg->apply_postfx;

    return view_id_base;
}

const JceCsmData *jce_scene_renderer_get_csm(const JceSceneRenderer *sr)
{
    if (!sr || !sr->last_csm_valid) return NULL;
    return &sr->last_csm;
}

JcePostFXPipeline *jce_scene_renderer_get_postfx(JceSceneRenderer *sr)
{
    return sr ? sr->postfx_pipeline : NULL;
}

bool jce_scene_renderer_is_skybox_active(const JceSceneRenderer *sr)
{
    return sr ? sr->skybox_active : false;
}
