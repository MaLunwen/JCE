/*
 * jce_scene_renderer.c  Engine scene renderer implementation.
 *
 * Adapted from editor's scene render code, factored to render a JceScene
 * directly with no editor-state coupling.
 *
 * ARCHITECTURE NOTE (Wave 8c Phase 3):
 * This implementation file includes middleware headers (scene, animation, LOD)
 * to access component data and iterate the ECS. However, the PUBLIC API in
 * jce_scene_renderer.h uses only opaque types (JceScene*, JceEntity) and
 * does NOT expose flecs or ECS implementation details. This satisfies
 * The-Forge layering: the renderer's public interface depends only on
 * renderer + resource layers; middleware dependency is an implementation detail.
 */

#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/scene/jce_lod.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_terrain.h>
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_ibl.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_skybox.h>
#include <jce/renderer/jce_sprite_batch.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>
#include <jce/renderer/jce_volume_profile.h>

#include <bgfx/c99/bgfx.h>
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG "scene_renderer"

/* C99-compatible compile-time assertion. */
#define JCE_SASSERT_CAT_(a, b)  a##b
#define JCE_SASSERT_CAT(a, b)   JCE_SASSERT_CAT_(a, b)
#define JCE_SASSERT(cond)       typedef char JCE_SASSERT_CAT(jce_ct_, __LINE__)[(cond) ? 1 : -1]

/* Public scene_renderer.h declares JceSceneLodStats with picks[JCE_SCENE_LOD_MAX_LEVELS]
   so it does not have to include middleware/scene/jce_lod.h. Keep the
   constants in lock-step. */
JCE_SASSERT(JCE_SCENE_LOD_MAX_LEVELS == JCE_LOD_MAX_LEVELS);

#define SR_MODEL_CACHE_MAX     32
#define SR_TEX_CACHE_MAX       512
#define SR_MAX_ENTITIES        4096
#define SR_MAT_CACHE_MAX       512
#define SHADOW_ORTHO_SIZE      50.0f
#define CSM_DIST_SCALE         512.0f
#define CSM_DIST_MAX           1200.0f
#define CSM_DIST_MIN           50.0f
#define CSM_FAR_HYST_REL       0.03f
#define CSM_FAR_HYST_ABS       8.0f

/* ── Internal struct ──────────────────────────────────────────────── */

/* Per-frame material registry entry. Snapshot of everything the binder
 * needs to re-bind textures + uniforms when render-queue auto-batching
 * starts a new material run. Built lazily as scene_renderer iterates
 * entities; flushed at frame end. */
typedef struct {
    uint32_t        key;
    JcePbrMaterial  pbr;
    bool            is_terrain;
    int             terrain_slot;   /* index into sr->terrain_cache, -1 if none */
    /* Terrain runtime params (copied so binder doesn't need TerrainComponent) */
    float           terrain_tile_scale;
    bool            terrain_splat_enabled;
    bgfx_texture_handle_t terrain_layer_tex[4]; /* layer0..3 albedo handles (white fallback) */
} SrMaterialEntry;

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

    /* Optional time-of-day override (driven by jce_time_of_day_evaluate). */
    bool                    tod_active;
    JceTimeOfDayState       tod_state;

    /* Optional editor-supplied ambient override. When active and ToD is
     * inactive, replaces the renderer's hardcoded ambient before lights
     * are gathered each frame. */
    bool                    ambient_override_active;
    jce_vec3                ambient_override_color;
    float                   ambient_override_intensity;

    /* Shadow map resources. */
    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;
    bool                       shadow_use_csm;
    uint16_t                   shadow_map_size;
    bgfx_texture_format_t      shadow_depth_fmt;
    bool                       shadow_near_valid;
    float                      shadow_near_cached;
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

    /* Per-frame culling stats (updated each render). */
    uint32_t                   stat_total_entities;
    uint32_t                   stat_visible_entities;
    uint32_t                   stat_culled_entities;
    bool                       stat_culling_enabled;

    /* Persistent cull-space (uniform grid). Reused every frame; reset
     * + re-insert each draw to amortise allocation overhead. */
    JceSpaceIndex             *cull_space;
    JceAABB                   *cull_aabbs;
    uint32_t                   cull_aabb_cap;

    /* Optional global LOD group. When non-NULL every entity with a
     * resolved mesh has its mesh replaced by the pick at draw time.
     * MVP — per-entity LOD attachment lands when the ECS schema gains
     * a LodGroup component. */
    const JceLodGroup         *global_lod;
    /* Hash-bucketed previous-level memory for hysteresis. Aliases on
     * collision (harmless: at worst one frame of slightly-wrong level). */
    int8_t                     lod_prev[1024];
    uint32_t                   stat_lod_picks[JCE_LOD_MAX_LEVELS];
    uint32_t                   stat_lod_culled;
    bool                       stat_lod_enabled;

    /* Render-queue stats — accumulated across all flushes this frame
     * (shadow + main mesh pass).  Reset at start of each scene render. */
    JceRenderQueueStats        stat_rq;
    bool                       stat_rq_active;

    /* Occlusion culling stats (from the most recent render). */
    JceOcclusionStats          stat_occlusion;
    bool                       stat_occlusion_enabled;

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

    /* Terrain mesh cache (path -> JceTerrain* + combined JceMesh*).
     * Synthesised on first use so terrain entities flow through the
     * existing PBR mesh path (and therefore receive shadows / lighting). */
    struct {
        char                  path[256];
        JceTerrain           *terrain;
        JceMesh              *mesh;
        bgfx_texture_handle_t splat_tex;
        bool                  used;
        bool                  failed;
        bool                  splat_uploaded;
    } terrain_cache[16];

    /* Terrain shader uniforms (created lazily on first terrain submit). */
    bgfx_uniform_handle_t u_terrain_params;
    bgfx_uniform_handle_t s_terrain_splat;
    bgfx_uniform_handle_t s_terrain_layer0;
    bgfx_uniform_handle_t s_terrain_layer1;
    bgfx_uniform_handle_t s_terrain_layer2;
    bgfx_uniform_handle_t s_terrain_layer3;

    /* ── Render-queue integration (Phase 2 stub) ─────────────────────
     * material registry is per-frame; reset at each sr_render begin.
     * frame_view_id / frame_shadow_vp let the binder rebuild bindings
     * without re-walking the scene. binder/queue wired in Phase 3. */
    JceRenderQueue   *render_queue;
    SrMaterialEntry   mat_cache[SR_MAT_CACHE_MAX];
    uint32_t          mat_count;
    uint16_t          frame_view_id;
    JceScene         *frame_scene;
    float             frame_shadow_vp[16];
    bool              frame_shadow_vp_valid;

    /* Volumetric fog (lazily created when first enabled). */
    JceVolumetricFog *vfog;
    int               vfog_w;
    int               vfog_h;
    bool              vfog_last_rendered;
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

/* ── Forward declarations for Phase 3 helpers ─────────────────────── */
static void     sr_reset_material_cache(JceSceneRenderer *sr);
static uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex);
static uint32_t sr_register_material(JceSceneRenderer *sr,
                                     uint32_t key,
                                     const JcePbrMaterial *pbr,
                                     bool is_terrain,
                                     int terrain_slot,
                                     float terrain_tile_scale,
                                     bool terrain_splat_enabled,
                                     const bgfx_texture_handle_t *terrain_layer_tex);
static void     sr_bind_material_cb(uint32_t material_key, void *user);
static void     sr_inline_bind_pbr_global(JceSceneRenderer *sr,
                                          const JcePbrMaterial *pbr,
                                          uint16_t view_id,
                                          JceScene *scene, EntityList *list);

static void sr_rq_flush_and_collect(JceSceneRenderer *sr)
{
    if (!sr || !sr->render_queue) return;
    jce_rq_flush(sr->render_queue, sr->renderer);
    JceRenderQueueStats s;
    jce_rq_last_stats(sr->render_queue, &s);
    sr->stat_rq.commands_in     += s.commands_in;
    sr->stat_rq.submits_out     += s.submits_out;
    sr->stat_rq.batches_merged  += s.batches_merged;
    sr->stat_rq.instances_total += s.instances_total;
    sr->stat_rq_active           = true;
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
        /* Terrain fallback: only attempted when no mesh renderer mesh
         * was resolved.  Synthesises a single combined mesh from all
         * terrain chunks at LOD 0 the first time the path is seen. */
        if (!*out_mesh && jce_scene_has_terrain(scene, e)) {
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
            if (tc && tc->visible && tc->terrain_path[0]) {
                int slot = -1, free_slot = -1;
                for (int i = 0; i < 16; i++) {
                    if (sr->terrain_cache[i].used &&
                        strncmp(sr->terrain_cache[i].path, tc->terrain_path,
                                sizeof sr->terrain_cache[i].path) == 0) {
                        slot = i; break;
                    }
                    if (!sr->terrain_cache[i].used && free_slot < 0) free_slot = i;
                }
                if (slot < 0 && free_slot >= 0) {
                    slot = free_slot;
                    memset(&sr->terrain_cache[slot], 0,
                           sizeof sr->terrain_cache[slot]);
                    jce_strlcpy(sr->terrain_cache[slot].path, tc->terrain_path,
                            sizeof sr->terrain_cache[slot].path);
                    sr->terrain_cache[slot].used = true;
                    /* PAK-first: deployed bundles overlay sr->pak, so a
                     * bundled terrain meta+bin can be loaded with zero host
                     * filesystem access.  If the path isn't in the PAK, fall
                     * back to the host-resolved path (editor / loose files). */
                    JceTerrain *terr = jce_terrain_load_from_pak(sr->pak,
                                                                  tc->terrain_path);
                    char        resolved[1024];
                    const char *load_path = tc->terrain_path;
                    if (!terr) {
                        if (sr->has_cbs && sr->cbs.resolve_path &&
                            sr->cbs.resolve_path(tc->terrain_path,
                                                  resolved, (int)sizeof(resolved),
                                                  sr->cbs.userdata)) {
                            load_path = resolved;
                        }
                        terr = jce_terrain_load_file(load_path);
                    }
                    if (!terr) { sr->terrain_cache[slot].failed = true;
                        LOG_WARN(LOG_TAG,
                                 "terrain load failed: '%s' (from '%s')",
                                 load_path, tc->terrain_path);
                    }
                    else {
                        sr->terrain_cache[slot].terrain = terr;
                        /* Combine all chunks into one mesh. */
                        int ncx = jce_terrain_chunk_count_x(terr);
                        int ncz = jce_terrain_chunk_count_z(terr);
                        int total_v = 0, total_i = 0;
                        for (int cz = 0; cz < ncz; cz++)
                        for (int cx = 0; cx < ncx; cx++) {
                            int v=0,ii=0;
                            jce_terrain_chunk_mesh_size(terr, cx, cz, 0, &v, &ii);
                            total_v += v; total_i += ii;
                        }
                        if (total_v > 0 && total_i > 0) {
                            JceTerrainVertex *tv = (JceTerrainVertex *)
                                JCE_MALLOC(sizeof(JceTerrainVertex) * (size_t)total_v);
                            uint32_t *tiidx = (uint32_t *)
                                JCE_MALLOC(sizeof(uint32_t) * (size_t)total_i);
                            int v_off = 0, i_off = 0;
                            for (int cz = 0; cz < ncz; cz++)
                            for (int cx = 0; cx < ncx; cx++) {
                                int v_cap = total_v - v_off;
                                int i_cap = total_i - i_off;
                                int wrote_v = 0, wrote_i = 0;
                                /* Build into a scratch & re-base indices. */
                                int v_need=0,i_need=0;
                                jce_terrain_chunk_mesh_size(terr, cx, cz, 0,
                                                            &v_need, &i_need);
                                if (v_need <= 0 || i_need <= 0) continue;
                                JceTerrainVertex *vbuf = tv + v_off;
                                uint32_t *ibuf = tiidx + i_off;
                                jce_terrain_chunk_build_mesh(terr, cx, cz, 0,
                                    vbuf, v_cap, ibuf, i_cap,
                                    &wrote_v, &wrote_i);
                                for (int k = 0; k < wrote_i; k++)
                                    ibuf[k] += (uint32_t)v_off;
                                v_off += wrote_v;
                                i_off += wrote_i;
                            }
                            /* JceTerrainVertex layout matches JceMeshVertex. */
                            sr->terrain_cache[slot].mesh = jce_mesh_create(
                                (const JceMeshVertex *)tv, (uint32_t)v_off,
                                tiidx, (uint32_t)i_off);
                            JCE_FREE(tv); JCE_FREE(tiidx);
                            if (!sr->terrain_cache[slot].mesh)
                                sr->terrain_cache[slot].failed = true;
                        } else {
                            sr->terrain_cache[slot].failed = true;
                        }
                    }
                }
                if (slot >= 0 && !sr->terrain_cache[slot].failed)
                    *out_mesh = sr->terrain_cache[slot].mesh;
            }
        }
    }
    return true;
}

/* ── Light direction helpers ──────────────────────────────────────── */

static jce_vec3 sr_light_shine_direction(const jce_vec3 *comp_dir)
{
    jce_vec3 fallback = jce_v3(0.0f, -1.0f, 0.0f);
    if (!comp_dir) return fallback;
    float len2 = comp_dir->x * comp_dir->x +
                 comp_dir->y * comp_dir->y +
                 comp_dir->z * comp_dir->z;
    if (len2 < 1e-8f) return fallback;
    return jce_v3_scale(*comp_dir, 1.0f / sqrtf(len2));
}

static jce_vec3 sr_light_world_shine_direction(const jce_vec3 *comp_dir,
                                               const JceTransform *xf)
{
    jce_vec3 dir = sr_light_shine_direction(comp_dir);
    if (xf) {
        jce_quat q = jce_q_normalize(xf->rotation);
        dir = jce_q_rotate(q, dir);
    }
    return sr_light_shine_direction(&dir);
}

static jce_vec3 sr_resolve_shadow_light_direction(JceSceneRenderer *sr,
                                                  JceScene *scene,
                                                  EntityList *list)
{
    jce_vec3 fallback = jce_dir_light_default().direction;
    jce_vec3 first_dir = fallback;
    bool have_any = false;

    if (!scene) {
        if (sr && sr->tod_active) {
            /* CSM expects to-light direction; tod_state.sun_direction is
             * already the unit vector from origin TOWARD the sun, so
             * return it as-is (do NOT negate). */
            return sr->tod_state.sun_direction;
        }
        return fallback;
    }

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_dir_light(scene, e)) continue;
        JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
        if (!dl) continue;
        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 dir = sr_light_world_shine_direction(&dl->direction, xf);
        dir = jce_v3_scale(dir, -1.0f);
        if (!have_any) { first_dir = dir; have_any = true; }
        if (dl->casts_shadow) return dir;
    }
    if (!have_any && sr && sr->tod_active) {
        /* See comment above — sun_direction is already to-light. */
        return sr->tod_state.sun_direction;
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
    if (sr->tod_active) {
        const JceTimeOfDayState *t = &sr->tod_state;
        sky_colors[0]  = t->sky_top.x;     sky_colors[1]  = t->sky_top.y;
        sky_colors[2]  = t->sky_top.z;     sky_colors[3]  = 1.0f;
        sky_colors[4]  = t->sky_horizon.x; sky_colors[5]  = t->sky_horizon.y;
        sky_colors[6]  = t->sky_horizon.z; sky_colors[7]  = 1.0f;
        sky_colors[8]  = t->sky_ground.x;  sky_colors[9]  = t->sky_ground.y;
        sky_colors[10] = t->sky_ground.z;  sky_colors[11] = 1.0f;
    }
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

static void sr_destroy_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr) return;
    if (BGFX_HANDLE_IS_VALID(sr->shadow_fbo)) {
        bgfx_destroy_frame_buffer(sr->shadow_fbo);
        sr->shadow_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->shadow_tex)) {
        bgfx_destroy_texture(sr->shadow_tex);
        sr->shadow_tex.idx = UINT16_MAX;
    }
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->csm_fbo[i])) {
            bgfx_destroy_frame_buffer(sr->csm_fbo[i]);
            sr->csm_fbo[i].idx = UINT16_MAX;
        }
        if (BGFX_HANDLE_IS_VALID(sr->csm_tex[i])) {
            bgfx_destroy_texture(sr->csm_tex[i]);
            sr->csm_tex[i].idx = UINT16_MAX;
        }
    }
    sr->shadow_valid = false;
    sr->csm_valid = false;
    sr->last_csm_valid = false;
}

static void sr_create_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr || sr->shadow_map_size == 0) return;

    const uint16_t sz = sr->shadow_map_size;
    const bgfx_texture_format_t depth_fmt = sr->shadow_depth_fmt;
    bgfx_attachment_t at;

    sr->shadow_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL);
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, sr->shadow_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    sr->shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->shadow_fbo);

    sr->csm_valid = true;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        sr->csm_tex[i] = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL);
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, sr->csm_tex[i], BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        sr->csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        if (!BGFX_HANDLE_IS_VALID(sr->csm_fbo[i]))
            sr->csm_valid = false;
    }
}

static void sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size)
{
    if (!sr || size == 0) return;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    if (sr->shadow_map_size == size &&
        BGFX_HANDLE_IS_VALID(sr->shadow_fbo) &&
        BGFX_HANDLE_IS_VALID(sr->csm_fbo[0]))
        return;

    sr_destroy_shadow_targets(sr);
    sr->shadow_map_size = size;
    sr_create_shadow_targets(sr);
    sr->shadow_near_valid = false;
    sr->shadow_far_valid = false;
}

static void sr_draw_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                const JceCamera *camera, EntityList *list,
                                uint16_t view_id_base, uint32_t vp_w,
                                uint32_t vp_h, float shadow_distance,
                                float split_lambda)
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

    JceShaderHandle shadow_inst_sh = jce_renderer_get_program_shadow_inst(sr->renderer);

    /* Queue path: enabled by default (JCE_USE_RQ=0 disables) + render_queue +
     * instanced shadow program available. Pushes per-entity per-cascade
     * entries to the queue with view_id as part of the batch key — auto-
     * batched into one submit per (view_id, mesh) pair. */
    static int s_use_rq_shadow_env = -1;
    if (s_use_rq_shadow_env < 0) {
        const char *v = getenv("JCE_USE_RQ");
        s_use_rq_shadow_env = (v && v[0] == '0') ? 0 : 1;
    }
    bool use_rq_shadow = s_use_rq_shadow_env
                      && sr->render_queue
                      && shadow_inst_sh.idx != UINT16_MAX;

    const bool use_csm = sr->csm_valid && sr->csm_cascade_count > 0
                      && jce_render_pipeline_is_feature_enabled("csm");
    jce_vec3 shadow_dir = sr_resolve_shadow_light_direction(sr, scene, list);

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

        if (use_rq_shadow) {
            jce_rq_clear(sr->render_queue);
            jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
            for (int i = 0; i < list->count; i++) {
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
                if (!mesh) continue;
                JceDrawCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.view_id        = shadow_view_0;
                cmd.program        = (uint16_t)shadow_inst_sh.idx;
                cmd.program_single = (uint16_t)shadow_sh.idx;
                cmd.mesh_vbh       = jce_mesh_get_vbh(mesh);
                cmd.mesh_ibh       = jce_mesh_get_ibh(mesh);
                cmd.index_count    = jce_mesh_index_count(mesh);
                cmd.transform      = model;
                cmd.depth          = 0.0f;
                cmd.material_key   = 1u; /* single shadow material */
                jce_rq_push(sr->render_queue, &cmd);
            }
            if (jce_rq_count(sr->render_queue) > 0) {
                jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
                sr_rq_flush_and_collect(sr);
            }
            return;
        }

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

    float shadow_near_target = cam_near;
    if (shadow_near_target < 0.001f) shadow_near_target = 0.001f;
    if (shadow_near_target > 1.0f)   shadow_near_target = 1.0f;

    if (!sr->shadow_near_valid) {
        float bucket = 0.005f;
        sr->shadow_near_cached =
            ceilf(shadow_near_target / bucket) * bucket;
        sr->shadow_near_valid = true;
    } else {
        float diff = fabsf(shadow_near_target - sr->shadow_near_cached);
        float trigger = fmaxf(0.01f, sr->shadow_near_cached * 0.10f);
        if (diff > trigger) {
            float bucket = 0.01f;
            sr->shadow_near_cached =
                ceilf(shadow_near_target / bucket) * bucket;
        }
    }
    float shadow_near = sr->shadow_near_cached;
    if (shadow_near < 0.001f) shadow_near = 0.001f;

    float shadow_far_target = cam_far;
    if (shadow_distance > 0.0f) {
        shadow_far_target = fminf(shadow_far_target, shadow_distance);
        shadow_far_target = fmaxf(shadow_far_target, shadow_near + 1.0f);
    } else {
        shadow_far_target = fminf(shadow_far_target,
            fmaxf(shadow_near * CSM_DIST_SCALE, CSM_DIST_MAX));
        shadow_far_target = fmaxf(shadow_far_target,
                                  shadow_near + CSM_DIST_MIN);
    }

    if (!sr->shadow_far_valid) {
        sr->shadow_far_cached = shadow_far_target;
        sr->shadow_far_valid = true;
    } else {
        /* Deadband + snap-to-target tracking.  Earlier code applied an
         * exponential lerp every frame which meant the cached value
         * micro-wobbled forever — that propagates into per-cascade radius
         * and texel_size, breaking the texel-snap stability and producing
         * the parallel-stripe shimmer the user reported.
         *
         * Strategy: only update when the target moves by >5% (or >2 m).
         * On update, snap to a coarse 1 m bucket so the cached far moves
         * in discrete steps.  This is what stabilises CSM during free
         * camera movement (Unity's CullingResults.shadowDistance uses a
         * similar coarse bucketing). */
        float diff = fabsf(shadow_far_target - sr->shadow_far_cached);
        float trigger = fmaxf(2.0f, sr->shadow_far_cached * 0.05f);
        if (diff > trigger) {
            float bucket = 1.0f;
            sr->shadow_far_cached = ceilf(shadow_far_target / bucket) * bucket;
        }
    }
    float shadow_far = sr->shadow_far_cached;
    if (shadow_far <= shadow_near)
        shadow_far = shadow_near + CSM_DIST_MIN;

    jce_mat4 cam_view = camera ? jce_camera_view(camera) : jce_m4_identity();
    jce_vec3 light_dir = shadow_dir;

    JceCsmData csm;
    jce_csm_compute(&csm, sr->csm_cascade_count,
                    shadow_near, shadow_far, cam_fov, aspect,
                    &cam_view, &light_dir,
                    sr->homogeneous_depth, sr->shadow_map_size,
                    split_lambda);

    sr->last_csm = csm;
    sr->last_csm_valid = true;

    if (use_rq_shadow) {
        jce_rq_clear(sr->render_queue);
        jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
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
                JceDrawCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.view_id        = cv;
                cmd.program        = (uint16_t)shadow_inst_sh.idx;
                cmd.program_single = (uint16_t)shadow_sh.idx;
                cmd.mesh_vbh       = jce_mesh_get_vbh(mesh);
                cmd.mesh_ibh       = jce_mesh_get_ibh(mesh);
                cmd.index_count    = jce_mesh_index_count(mesh);
                cmd.transform      = model;
                cmd.depth          = 0.0f;
                cmd.material_key   = 1u;
                jce_rq_push(sr->render_queue, &cmd);
            }
        }
        if (jce_rq_count(sr->render_queue) > 0) {
            jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
            sr_rq_flush_and_collect(sr);
        }
    } else {
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
        /* Try PAK chain first (engine + bundle overlays) so a bundled
         * HDR works without a sidecar file on disk.  Fall back to the
         * host filesystem for user-authored / loose HDRs. */
        const JcePakAsset *hdr_asset = jce_pak_find(sr->pak, hdr_path);
        if (hdr_asset && hdr_asset->original_size > 0) {
            void *hdr_buf = JCE_MALLOC((size_t)hdr_asset->original_size);
            if (hdr_buf) {
                size_t got = jce_pak_decompress_ex(sr->pak, hdr_asset,
                                                  hdr_buf,
                                                  (size_t)hdr_asset->original_size);
                if (got == (size_t)hdr_asset->original_size)
                    sr->skybox = jce_skybox_create_from_hdr_memory(
                                     hdr_buf, (uint32_t)got, 512);
                JCE_FREE(hdr_buf);
            }
        }
        if (!sr->skybox)
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

/* ── Frustum culling (uniform-grid broadphase) ────────────────────── */

/* Extract 6 frustum planes from a column-major view*proj matrix.
 * Convention: plane.xyz = normal, plane.w = signed distance such that
 *             dot(plane.xyz, p) + plane.w >= 0  iff  p is INSIDE the frustum.
 * Works for D3D-style NDC ([0,1] depth) and OpenGL-style ([-1,1]) alike for
 * left/right/top/bottom; near plane uses (m3 + m2) which is correct for
 * GL and conservative (looser) for D3D — fine for broadphase culling. */
static void sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6])
{
    /* m->raw[col][row] (column-major); rows of the matrix = m->raw[*][row]. */
    #define RC(col, row) m->raw[(col)][(row)]
    /* Left:  row3 + row0 ; Right: row3 - row0 */
    for (int i = 0; i < 6; i++) {
        const int row  = i / 2;        /* 0..2 */
        const int sign = (i & 1) ? -1 : 1;
        planes[i].x = RC(0, 3) + sign * RC(0, row);
        planes[i].y = RC(1, 3) + sign * RC(1, row);
        planes[i].z = RC(2, 3) + sign * RC(2, row);
        planes[i].w = RC(3, 3) + sign * RC(3, row);
        const float L = sqrtf(planes[i].x * planes[i].x +
                              planes[i].y * planes[i].y +
                              planes[i].z * planes[i].z);
        if (L > 1e-6f) {
            const float inv = 1.0f / L;
            planes[i].x *= inv;
            planes[i].y *= inv;
            planes[i].z *= inv;
            planes[i].w *= inv;
        }
    }
    #undef RC
}

/* Build a transient grid from the entity list and frustum-cull it.
 * Output: visible[i] = true if entity list->entities[i] passes culling.
 * Returns the visible entity count. */
static uint32_t sr_compute_visible(JceSceneRenderer *sr,
                                    JceScene *scene,
                                    const EntityList *list,
                                    const jce_vec4 planes[6],
                                    bool *visible)
{
    /* Grow persistent AABB array if needed. */
    if (sr->cull_aabb_cap < (uint32_t)list->count) {
        uint32_t new_cap = sr->cull_aabb_cap ? sr->cull_aabb_cap * 2u : 64u;
        while (new_cap < (uint32_t)list->count) new_cap *= 2u;
        JceAABB *grown = (JceAABB *)JCE_REALLOC(sr->cull_aabbs,
                                                 new_cap * sizeof(JceAABB));
        if (!grown) {
            for (int i = 0; i < list->count; i++) visible[i] = true;
            return (uint32_t)list->count;
        }
        sr->cull_aabbs    = grown;
        sr->cull_aabb_cap = new_cap;
    }
    JceAABB *aabbs = sr->cull_aabbs;

    /* World bounds derived from a quick scan — we want a snug grid. */
    jce_vec3 wmin = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (int i = 0; i < list->count; i++) {
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, list->entities[i], &model, &mesh)) {
            aabbs[i].min = aabbs[i].max = jce_v3(0, 0, 0);
            visible[i] = true; /* keep entities without transform/model */
            continue;
        }

        /* Local-space AABB. Fall back to a unit cube around the origin
         * if no mesh resolved (terrain / future component types). */
        float lmn[3], lmx[3];
        if (mesh) {
            jce_mesh_get_aabb(mesh, lmn, lmx);
        } else {
            lmn[0] = lmn[1] = lmn[2] = -0.5f;
            lmx[0] = lmx[1] = lmx[2] =  0.5f;
        }

        /* Degenerate AABB safeguard. */
        if (lmx[0] - lmn[0] < 1e-4f && lmx[1] - lmn[1] < 1e-4f &&
            lmx[2] - lmn[2] < 1e-4f) {
            lmn[0] = lmn[1] = lmn[2] = -0.5f;
            lmx[0] = lmx[1] = lmx[2] =  0.5f;
        }

        /* Transform 8 corners by world matrix and refit. */
        const jce_vec3 corners[8] = {
            { lmn[0], lmn[1], lmn[2] }, { lmx[0], lmn[1], lmn[2] },
            { lmn[0], lmx[1], lmn[2] }, { lmx[0], lmx[1], lmn[2] },
            { lmn[0], lmn[1], lmx[2] }, { lmx[0], lmn[1], lmx[2] },
            { lmn[0], lmx[1], lmx[2] }, { lmx[0], lmx[1], lmx[2] },
        };
        jce_vec3 bmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
        jce_vec3 bmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int c = 0; c < 8; c++) {
            const jce_vec4 cv = { corners[c].x, corners[c].y, corners[c].z, 1.0f };
            const jce_vec4 wv = jce_m4_mul_v4(&model, cv);
            const float wx = wv.x, wy = wv.y, wz = wv.z;
            if (wx < bmn.x) bmn.x = wx; if (wx > bmx.x) bmx.x = wx;
            if (wy < bmn.y) bmn.y = wy; if (wy > bmx.y) bmx.y = wy;
            if (wz < bmn.z) bmn.z = wz; if (wz > bmx.z) bmx.z = wz;
        }
        aabbs[i].min = bmn;
        aabbs[i].max = bmx;
        if (bmn.x < wmin.x) wmin.x = bmn.x;
        if (bmn.y < wmin.y) wmin.y = bmn.y;
        if (bmn.z < wmin.z) wmin.z = bmn.z;
        if (bmx.x > wmax.x) wmax.x = bmx.x;
        if (bmx.y > wmax.y) wmax.y = bmx.y;
        if (bmx.z > wmax.z) wmax.z = bmx.z;
        visible[i] = false;   /* will be flipped to true by the query below */
    }

    /* Pad to avoid degenerate dimensions. */
    const float pad = 1.0f;
    wmin.x -= pad; wmin.y -= pad; wmin.z -= pad;
    wmax.x += pad; wmax.y += pad; wmax.z += pad;

    JceAABB world = { wmin, wmax };

    if (!sr->cull_space) {
        JceSpaceConfig cfg = { 0 };
        cfg.type             = JCE_SPACE_GRID;
        cfg.world_bounds     = world;
        cfg.max_objects      = (uint32_t)list->count;
        sr->cull_space = jce_space_create(&cfg);
    } else {
        jce_space_reset(sr->cull_space, &world);
    }

    if (!sr->cull_space) {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        return (uint32_t)list->count;
    }

    for (int i = 0; i < list->count; i++) {
        if (visible[i]) continue;     /* transform-less; already kept */
        jce_space_insert(sr->cull_space, aabbs[i], (uint32_t)i);
    }

    uint32_t hit_buf[SR_MAX_ENTITIES];
    const uint32_t hits = jce_space_query_frustum(sr->cull_space, planes,
                                                   hit_buf, SR_MAX_ENTITIES);
    for (uint32_t k = 0; k < hits; k++) {
        if (hit_buf[k] < (uint32_t)list->count) visible[hit_buf[k]] = true;
    }

    return hits;
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
        if (sr->ambient_override_active) {
            jce_light_env_set_ambient(sr->light_env,
                                       sr->ambient_override_color,
                                       sr->ambient_override_intensity);
        } else if (jce_scene_has_rendering_settings(scene)) {
            const JceSceneRenderingSettings *r =
                jce_scene_get_rendering_settings(scene);
            jce_vec3 color = jce_v3(r->ambient_color[0],
                                    r->ambient_color[1],
                                    r->ambient_color[2]);
            jce_light_env_set_ambient(sr->light_env, color,
                                       r->ambient_intensity);
        } else {
            jce_light_env_set_ambient(sr->light_env, jce_v3(1, 1, 1), 0.15f);
        }

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
                    dl.direction =
                        sr_light_world_shine_direction(&dlc->direction, xf);
                    /* P3-E.5 — propagate optional cookie. */
                    dl.cookie_texture  = dlc->cookie_texture;
                    dl.cookie_strength = dlc->cookie_strength;
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
                    sl.direction =
                        sr_light_world_shine_direction(&slc->direction, xf);
                    /* P3-E.5 — propagate cookie + IES profile bindings. */
                    sl.cookie_texture  = slc->cookie_texture;
                    sl.ies_lut_texture = slc->ies_lut_texture;
                    sl.cookie_strength = slc->cookie_strength;
                    jce_light_env_add_spot_light(sr->light_env, &sl);
                    has_any_light = true;
                }
            }
        }

        if (!has_any_light) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            if (sr->tod_active) {
                dl.direction = jce_v3_scale(sr->tod_state.sun_direction, -1.0f);
                dl.color     = sr->tod_state.sun_color;
                dl.intensity = 1.0f;
            } else {
                dl.direction = jce_v3(-0.5f, -1.0f, -0.3f);
                dl.color = jce_v3(1, 1, 1);
                dl.intensity = 1.0f;
            }
            jce_light_env_add_dir_light(sr->light_env, &dl);
        }

        if (sr->tod_active) {
            jce_light_env_set_ambient(sr->light_env,
                                       sr->tod_state.ambient_color, 1.0f);
        }

        if (camera) {
            jce_vec3 cp = jce_camera_get_position(camera);
            jce_light_env_set_camera_pos(sr->light_env, cp);
        }
        jce_light_env_apply(sr->light_env, sr->renderer);
    }

    JceDirLight sun = jce_dir_light_default();
    sun.direction = sr_resolve_shadow_light_direction(sr, scene, list);
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

    /* Optional broadphase frustum culling.  When disabled, every entity
     * is treated as visible (matches legacy behaviour). */
    bool visible_buf[SR_MAX_ENTITIES];
    bool *visible = visible_buf;
    sr->stat_total_entities  = (uint32_t)list->count;
    sr->stat_culling_enabled = cfg->frustum_culling;
    if (cfg->frustum_culling && camera && list->count > 0) {
        const jce_mat4 v = jce_camera_view(camera);
        const jce_mat4 p = jce_camera_proj(camera, 16.0f / 9.0f,
                                            sr->homogeneous_depth);
        const jce_mat4 vp = jce_m4_multiply(&p, &v);
        jce_vec4 planes[6];
        sr_extract_frustum_planes(&vp, planes);
        const uint32_t kept = sr_compute_visible(sr, scene, list, planes, visible);
        sr->stat_visible_entities = kept;
        sr->stat_culled_entities  = (uint32_t)list->count - kept;
    } else {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        sr->stat_visible_entities = (uint32_t)list->count;
        sr->stat_culled_entities  = 0;
    }

    /* Reset per-frame LOD pick counters. */
    sr->stat_lod_enabled = (sr->global_lod && sr->global_lod->count > 0);
    for (int li = 0; li < JCE_LOD_MAX_LEVELS; li++) sr->stat_lod_picks[li] = 0;
    sr->stat_lod_culled = 0;

    /* Reset per-frame render-queue stats (accumulated across all flushes). */
    memset(&sr->stat_rq, 0, sizeof(sr->stat_rq));
    sr->stat_rq_active = false;

    /* Begin occlusion culler frame (reads last-frame query results).
     * We compute view/proj here regardless of frustum culling so the
     * proxy depth-only pass sees the same camera as the main scene. */
    sr->stat_occlusion_enabled = (cfg->occlusion_culler != NULL);
    memset(&sr->stat_occlusion, 0, sizeof(sr->stat_occlusion));
    if (cfg->occlusion_culler && camera) {
        const jce_mat4 oc_v  = jce_camera_view(camera);
        const jce_mat4 oc_p  = jce_camera_proj(camera, 16.0f / 9.0f,
                                                sr->homogeneous_depth);
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler,
                                          JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
        /* Set the view transform for the proxy pre-pass view (id 254) so
         * the depth-only proxy draws are in the correct camera space. */
        bgfx_set_view_transform(254, JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
    } else if (cfg->occlusion_culler) {
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler, NULL, NULL);
    }
    jce_vec3 lod_cam_pos = (jce_vec3){0, 0, 0};
    if (sr->stat_lod_enabled && camera) lod_cam_pos = jce_camera_get_position(camera);

    /* ── Render-queue setup (Phase 3) ─────────────────────────────────
     * If the instanced PBR program is available we route non-terrain
     * mesh entities through the queue → auto-batched instanced submits.
     * Terrain, wireframe, sprite, skinned paths still submit inline. */
    JceShaderHandle prog_pbr_inst_h = jce_renderer_get_program_pbr_inst(sr->renderer);
    JceShaderHandle prog_pbr_h      = jce_renderer_get_program_pbr(sr->renderer);
    /* Queue path is on by default. Earlier PSO-compile glitches with
     * vs_pbr_inst on a few D3D12 setups have been resolved (see Wave 6/7);
     * leave an opt-out via JCE_USE_RQ=0 for forensic comparison. */
    static int s_use_rq_env = -1;
    if (s_use_rq_env < 0) {
        const char *v = getenv("JCE_USE_RQ");
        s_use_rq_env = (v && v[0] == '0') ? 0 : 1;
    }
    bool use_rq = s_use_rq_env && sr->render_queue && prog_pbr_inst_h.idx != UINT16_MAX;
    if (use_rq) {
        sr_reset_material_cache(sr);
        jce_rq_clear(sr->render_queue);
        sr->frame_view_id = view_id;
        sr->frame_scene = scene;
        sr->frame_shadow_vp_valid = false;
        if (sr->shadow_valid && !sr->shadow_use_csm) {
            jce_vec3 sd = sr_resolve_shadow_light_direction(sr, scene, list);
            sr_compute_shadow_vp(sr, &sd, sr->frame_shadow_vp);
            sr->frame_shadow_vp_valid = true;
        }
    }

    for (int i = 0; i < list->count; i++) {
        if (!visible[i]) continue;
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;

        /* ── Occlusion culling (GPU-query, two-pass coherence) ───────
         * Derive a world-space radius from the entity scale (uniform).
         * Skip the draw if the entity was fully occluded last frame and
         * submit a depth-only proxy instead so the culler can update. */
        float occlusion_radius = 0.5f;
        bool occlusion_skip = false;
        if (cfg->occlusion_culler) {
            float sx = model.col[0].x, sy = model.col[1].y, sz = model.col[2].z;
            float sc = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
            if (sc < 0.001f) sc = 0.001f;
            occlusion_radius = sc * 0.5f;

            jce_vec3 world_pos = { model.col[3].x, model.col[3].y, model.col[3].z };
            if (!jce_occlusion_culler_entity_visible(cfg->occlusion_culler,
                                                     (uint64_t)e, world_pos,
                                                     occlusion_radius)) {
                occlusion_skip = true;
                /* Still submit a proxy query so next frame is accurate. */
                jce_occlusion_culler_submit_query(cfg->occlusion_culler,
                                                  (uint64_t)e, world_pos,
                                                  occlusion_radius);
                continue;
            }
        }

        /* ── Global LOD substitution (MVP) ───────────────────────── */
        if (sr->stat_lod_enabled) {
            const float dx = model.col[3].x - lod_cam_pos.x;
            const float dy = model.col[3].y - lod_cam_pos.y;
            const float dz = model.col[3].z - lod_cam_pos.z;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            const uint32_t slot = (uint32_t)e & 1023u;
            const int prev = (int)sr->lod_prev[slot];
            const int lvl  = jce_lod_pick(sr->global_lod, dist,
                                          prev > 0 ? prev - 1 : -1);
            if (lvl < 0) {
                sr->stat_lod_culled++;
                /* Park at last level so a return swing snaps cleanly. */
                sr->lod_prev[slot] = (int8_t)(sr->global_lod->count);
                continue;
            }
            sr->lod_prev[slot] = (int8_t)(lvl + 1);  /* 0 == "no record" */
            sr->stat_lod_picks[lvl]++;
            JceMesh *lod_mesh = sr->global_lod->levels[lvl].mesh;
            if (lod_mesh) mesh = lod_mesh;
        }

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

        if (!wireframe_path) {
            JcePbrMaterial pbr = jce_pbr_material_default();
            bool use_checker_fallback = false;

            /* If this entity has no MeshRenderer component, fall back to a
             * neutral default material (mid-gray, fully rough, dielectric)
             * so the mesh still goes through the PBR shader and therefore
             * receives shadows / IBL exactly like fully-described entities.
             * Earlier this branch went through the legacy fs_mesh.sc which
             * has no shadow sampler, producing the well-known "object only
             * gets darker but never receives external shadow" symptom. */
            if (!mr_comp) {
                pbr.base_color_factor[0] = 0.7f;
                pbr.base_color_factor[1] = 0.7f;
                pbr.base_color_factor[2] = 0.7f;
                pbr.base_color_factor[3] = 1.0f;
                pbr.metallic_factor = 0.0f;
                pbr.roughness_factor = 0.85f;
                pbr.normal_scale = 1.0f;
                pbr.ao_strength = 1.0f;

                /* In editor TEXTURED mode the legacy path used a magenta
                 * checker to highlight missing assets; preserve that hint. */
                if (editor_mode && cfg->view_mode == JCE_SCENE_VIEW_TEXTURED)
                    use_checker_fallback = true;
            } else if (mr_comp->base_color[3] > 0.0f) {
                pbr.base_color_factor[0] = mr_comp->base_color[0];
                pbr.base_color_factor[1] = mr_comp->base_color[1];
                pbr.base_color_factor[2] = mr_comp->base_color[2];
                pbr.base_color_factor[3] = mr_comp->base_color[3];
            }
            if (mr_comp) {
                pbr.metallic_factor = mr_comp->metallic;
                pbr.roughness_factor = mr_comp->roughness;
                pbr.emissive_factor[0] = mr_comp->emissive[0];
                pbr.emissive_factor[1] = mr_comp->emissive[1];
                pbr.emissive_factor[2] = mr_comp->emissive[2];
                pbr.normal_scale = mr_comp->normal_scale;
                pbr.ao_strength = mr_comp->ao_strength;
                pbr.alpha_mode = (JceAlphaMode)mr_comp->alpha_mode;
                pbr.alpha_cutoff = mr_comp->alpha_cutoff;
                pbr.double_sided = mr_comp->double_sided;
            }

            /* Always load all texture maps in editor + runtime. The
             * previous "factors-only in SHADED" optimisation made every
             * baseColor=[1,1,1,1] material render full white because
             * jce_pbr_material_bind() falls back to s_white_tex when no
             * albedo handle is bound. We now keep textures loading in
             * SHADED too, and any missing/in-flight albedo triggers the
             * pink-black checker shader fallback below — never a flat
             * white "loading" surface. */
            bool load_tex = true;

            if (load_tex && mr_comp) {
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
                /* Editor + missing albedo (in-flight OR failed) → pink-black
                 * checker shader fallback. Applies in BOTH SHADED and
                 * TEXTURED so the editor never displays the white loading
                 * texture while the async loader is resolving the asset. */
                if (editor_mode &&
                    (cfg->view_mode == JCE_SCENE_VIEW_SHADED ||
                     cfg->view_mode == JCE_SCENE_VIEW_TEXTURED) &&
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

            /* ── Resolve terrain slot + layer textures (early; needed for
             *     both inline terrain submit and queue material key). */
            int terrain_slot = -1;
            JceTerrainComponent *tc_render = NULL;
            bgfx_texture_handle_t terrain_layer_tex[4];
            for (int li = 0; li < 4; li++) terrain_layer_tex[li] = sr->white_tex;
            if (jce_scene_has_terrain(scene, e)) {
                tc_render = jce_scene_get_terrain(scene, e);
                if (tc_render && tc_render->terrain_path[0]) {
                    for (int ti = 0; ti < 16; ti++) {
                        if (sr->terrain_cache[ti].used &&
                            !sr->terrain_cache[ti].failed &&
                            strncmp(sr->terrain_cache[ti].path,
                                    tc_render->terrain_path,
                                    sizeof sr->terrain_cache[ti].path) == 0) {
                            terrain_slot = ti; break;
                        }
                    }
                    for (int li = 0; li < 4; li++) {
                        if (!tc_render->layer_albedo_path[li][0]) continue;
                        JceTexture lt = sr_resolve_texture(sr,
                            tc_render->layer_albedo_path[li]);
                        if (jce_texture_valid(lt)) terrain_layer_tex[li].idx = lt.idx;
                    }
                }
            }
            bool is_terrain_entity = (terrain_slot >= 0 && tc_render != NULL);

            /* ── Queue path: non-terrain mesh entities go through the
             *     render queue → auto-batched instanced submit. */
            if (use_rq && !is_terrain_entity) {
                uint32_t mat_key = sr_compute_material_key(&pbr, false, -1, NULL);
                uint32_t reg_key = sr_register_material(sr, mat_key, &pbr,
                                                        false, -1, 0.0f, false, NULL);
                if (reg_key) {
                    JceDrawCmd cmd;
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.view_id      = view_id;
                    cmd.program        = (uint16_t)prog_pbr_inst_h.idx;
                    cmd.program_single = (prog_pbr_h.idx != UINT16_MAX)
                                            ? (uint16_t)prog_pbr_h.idx
                                            : (uint16_t)UINT16_MAX;
                    cmd.mesh_vbh     = jce_mesh_get_vbh(mesh);
                    cmd.mesh_ibh     = jce_mesh_get_ibh(mesh);
                    cmd.index_count  = jce_mesh_index_count(mesh);
                    cmd.transform    = model;
                    cmd.depth        = 0.0f;
                    cmd.material_key = reg_key;
                    jce_rq_push(sr->render_queue, &cmd);
                    continue;  /* handled by queue flush below */
                }
                /* Registry overflow → fall through to inline path below. */
            }

            /* ── Inline path (terrain, queue overflow, or queue disabled) ── */
            sr_inline_bind_pbr_global(sr, &pbr, view_id, scene, list);

            if (is_terrain_entity) {
                /* Lazy upload of the splat texture. */
                if (!sr->terrain_cache[terrain_slot].splat_uploaded &&
                    sr->terrain_cache[terrain_slot].terrain) {
                    JceTerrain *terr = sr->terrain_cache[terrain_slot].terrain;
                    int tw = jce_terrain_width(terr);
                    int th = jce_terrain_height(terr);
                    const uint32_t *splat = jce_terrain_splat(terr);
                    if (splat && tw > 0 && th > 0) {
                        const bgfx_memory_t *mem = bgfx_copy(splat,
                            (uint32_t)(tw * th * 4));
                        sr->terrain_cache[terrain_slot].splat_tex =
                            bgfx_create_texture_2d((uint16_t)tw, (uint16_t)th,
                                false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                mem);
                    }
                    sr->terrain_cache[terrain_slot].splat_uploaded = true;
                }

                /* Override stages 0 (s_albedo→layer0), 4 (s_emissive→layer3),
                 * 14 (s_layer1), 15 (s_layer2), and 13 (s_splatMap). */
                bgfx_set_texture(0,  sr->s_terrain_layer0, terrain_layer_tex[0], UINT32_MAX);
                bgfx_set_texture(4,  sr->s_terrain_layer3, terrain_layer_tex[3], UINT32_MAX);

                bgfx_texture_handle_t splat_h = sr->terrain_cache[terrain_slot].splat_tex;
                if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;
                bgfx_set_texture(13, sr->s_terrain_splat,  splat_h,            UINT32_MAX);
                bgfx_set_texture(14, sr->s_terrain_layer1, terrain_layer_tex[1], UINT32_MAX);
                bgfx_set_texture(15, sr->s_terrain_layer2, terrain_layer_tex[2], UINT32_MAX);

                float tparams[4] = {
                    tc_render->tile_scale > 0.0f ? tc_render->tile_scale : 10.0f,
                    tc_render->splat_enabled ? 1.0f : 0.0f,
                    0.0f, 0.0f
                };
                bgfx_set_uniform(sr->u_terrain_params, tparams, 1);

                jce_mesh_submit_terrain(mesh, sr->renderer, view_id);
            } else {
                jce_mesh_submit_pbr(mesh, sr->renderer, view_id);
            }
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

        /* Submit occlusion proxy query for this visible entity so the
         * culler can determine visibility for the NEXT frame. */
        if (cfg->occlusion_culler && !occlusion_skip) {
            jce_vec3 world_pos = { model.col[3].x, model.col[3].y, model.col[3].z };
            jce_occlusion_culler_submit_query(cfg->occlusion_culler,
                                              (uint64_t)e, world_pos,
                                              occlusion_radius);
        }
    }

    /* Collect occlusion stats from the culler after entity loop. */
    if (cfg->occlusion_culler)
        sr->stat_occlusion = jce_occlusion_culler_get_stats(cfg->occlusion_culler);

    /* ── Render-queue flush (Phase 3) ─────────────────────────────────
     * Issues auto-batched instanced submits for all PBR mesh entities
     * pushed during the loop. The binder rebinds material textures /
     * uniforms once per material run. */
    if (use_rq && jce_rq_count(sr->render_queue) > 0) {
        jce_rq_set_material_binder(sr->render_queue, sr_bind_material_cb, sr);
        jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
        sr_rq_flush_and_collect(sr);
    }

    /* Flush sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites &&
        jce_sprite_batch_count(sr->sprite_batch) > 0)
        jce_sprite_batch_flush(sr->sprite_batch, sr->renderer, view_id);
}

/* ── Public API ───────────────────────────────────────────────────── */

/* ── Material registry (Phase 2) ──────────────────────────────────────
 * Per-frame registry mapping a material_key → texture/uniform snapshot.
 * Built during scene_renderer's mesh walk, consumed by sr_bind_material_cb
 * once jce_render_queue starts a new material run. Wired into the queue
 * in Phase 3. */

static uint32_t sr_fnv1a_step(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex)
{
    uint32_t h = 2166136261u;
    /* Texture handles (idx is enough — invalid = UINT16_MAX). */
    h = sr_fnv1a_step(h, &pbr->albedo_map.idx,             sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->metallic_roughness_map.idx, sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->normal_map.idx,             sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->ao_map.idx,                 sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->emissive_map.idx,           sizeof(uint16_t));
    /* Factors. */
    h = sr_fnv1a_step(h, pbr->base_color_factor, sizeof(pbr->base_color_factor));
    h = sr_fnv1a_step(h, &pbr->metallic_factor,  sizeof(float));
    h = sr_fnv1a_step(h, &pbr->roughness_factor, sizeof(float));
    h = sr_fnv1a_step(h, pbr->emissive_factor,   sizeof(pbr->emissive_factor));
    h = sr_fnv1a_step(h, &pbr->normal_scale,     sizeof(float));
    h = sr_fnv1a_step(h, &pbr->ao_strength,      sizeof(float));
    /* State. */
    uint32_t am = (uint32_t)pbr->alpha_mode;
    h = sr_fnv1a_step(h, &am,                  sizeof(am));
    h = sr_fnv1a_step(h, &pbr->alpha_cutoff,   sizeof(float));
    uint8_t ds = pbr->double_sided ? 1u : 0u;
    h = sr_fnv1a_step(h, &ds,                  sizeof(ds));
    /* Terrain pseudo-fields (slot ensures distinct splat/layer textures). */
    uint8_t it = is_terrain ? 1u : 0u;
    h = sr_fnv1a_step(h, &it, sizeof(it));
    int32_t ts = (int32_t)terrain_slot;
    h = sr_fnv1a_step(h, &ts, sizeof(ts));
    /* Terrain layer texture handles also folded in so two terrain entities
     * with the same slot but different runtime layer textures still split
     * into separate batches (rare today, but keeps key correctness). */
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++)
            h = sr_fnv1a_step(h, &terrain_layer_tex[li].idx, sizeof(uint16_t));
    }
    return h ? h : 1u;
}

/* Resets the per-frame registry. Call at the top of each sr_render. */
static void sr_reset_material_cache(JceSceneRenderer *sr)
{
    sr->mat_count = 0;
    sr->frame_shadow_vp_valid = false;
}

/* Look up an existing entry by key, or append a new one. Returns the
 * resolved key (== input on success). On overflow returns 0 and emits
 * a one-shot warning per renderer; caller treats key=0 as "skip queue,
 * submit directly" (Phase 3 fallback). */
static uint32_t sr_register_material(JceSceneRenderer *sr,
                                     uint32_t key,
                                     const JcePbrMaterial *pbr,
                                     bool is_terrain,
                                     int terrain_slot,
                                     float terrain_tile_scale,
                                     bool terrain_splat_enabled,
                                     const bgfx_texture_handle_t *terrain_layer_tex)
{
    for (uint32_t i = 0; i < sr->mat_count; i++) {
        if (sr->mat_cache[i].key == key) return key;
    }
    if (sr->mat_count >= SR_MAT_CACHE_MAX) {
        static bool warned = false;
        if (!warned) {
            LOG_WARN(LOG_TAG, "material cache overflow (>%d unique materials/frame); "
                              "instancing disabled for excess", SR_MAT_CACHE_MAX);
            warned = true;
        }
        return 0u;
    }
    SrMaterialEntry *e = &sr->mat_cache[sr->mat_count++];
    e->key                   = key;
    e->pbr                   = *pbr;
    e->is_terrain            = is_terrain;
    e->terrain_slot          = terrain_slot;
    e->terrain_tile_scale    = terrain_tile_scale;
    e->terrain_splat_enabled = terrain_splat_enabled;
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++) e->terrain_layer_tex[li] = terrain_layer_tex[li];
    } else {
        for (int li = 0; li < 4; li++) e->terrain_layer_tex[li] = sr->white_tex;
    }
    return key;
}

/* Render-queue binder callback. Invoked once per material run during
 * jce_rq_flush. Re-binds all textures / uniforms that jce_mesh_submit_pbr
 * USED to bind inline before it was decomposed. Frame-level state
 * (shadow VP, IBL handles) lives on `sr` directly. */
static void sr_bind_material_cb(uint32_t material_key, void *user)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    if (!sr || material_key == 0u) return;

    SrMaterialEntry *e = NULL;
    for (uint32_t i = 0; i < sr->mat_count; i++) {
        if (sr->mat_cache[i].key == material_key) { e = &sr->mat_cache[i]; break; }
    }
    if (!e) return;

    /* PBR textures + factor uniforms. */
    jce_pbr_material_bind(&e->pbr, sr->renderer, sr->frame_view_id);

    /* Lighting uniforms (u_dirLights / u_pointLights / u_spotLights /
     * u_lightCounts / u_ambientColor / u_cameraPos). bgfx clears uniform
     * state after every submit, so we MUST re-apply the light env per
     * material run — otherwise only the first submitted entity in the
     * frame samples real lights and everything after renders unlit
     * (the symptom users report as "shaded looks identical to textured"). */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);

    /* Single-light shadow map. */
    if (sr->shadow_valid && !sr->shadow_use_csm && sr->frame_shadow_vp_valid) {
        bgfx_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
        bgfx_set_uniform(sr->u_shadowVP, sr->frame_shadow_vp, 1);
    }
    /* CSM cascade samplers. */
    if (sr->csm_valid && sr->shadow_use_csm) {
        for (uint32_t ci = 0;
             ci < sr->csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
            bgfx_set_texture((uint8_t)(9 + ci), sr->u_csm_samplers[ci],
                             sr->csm_tex[ci], UINT32_MAX);
    }

    /* IBL. */
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

    /* Terrain texture overrides + params (replaces stages 0/4 + adds 13/14/15). */
    if (e->is_terrain && e->terrain_slot >= 0 && e->terrain_slot < 16) {
        int ts = e->terrain_slot;
        bgfx_texture_handle_t splat_h = sr->terrain_cache[ts].splat_tex;
        if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;
        bgfx_texture_handle_t l0 = e->terrain_layer_tex[0];
        bgfx_texture_handle_t l1 = e->terrain_layer_tex[1];
        bgfx_texture_handle_t l2 = e->terrain_layer_tex[2];
        bgfx_texture_handle_t l3 = e->terrain_layer_tex[3];
        if (!BGFX_HANDLE_IS_VALID(l0)) l0 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l1)) l1 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l2)) l2 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l3)) l3 = sr->white_tex;
        bgfx_set_texture(0,  sr->s_terrain_layer0, l0,      UINT32_MAX);
        bgfx_set_texture(4,  sr->s_terrain_layer3, l3,      UINT32_MAX);
        bgfx_set_texture(13, sr->s_terrain_splat,  splat_h, UINT32_MAX);
        bgfx_set_texture(14, sr->s_terrain_layer1, l1,      UINT32_MAX);
        bgfx_set_texture(15, sr->s_terrain_layer2, l2,      UINT32_MAX);
        float tparams[4] = {
            e->terrain_tile_scale > 0.0f ? e->terrain_tile_scale : 10.0f,
            e->terrain_splat_enabled ? 1.0f : 0.0f,
            0.0f, 0.0f
        };
        bgfx_set_uniform(sr->u_terrain_params, tparams, 1);
    }
}

/* Suppress unused-static warnings until Phase 3 wires these in. */
static void sr_phase2_anchor(void) {
    (void)sr_phase2_anchor;
}

/* Inline binding helper extracted from the legacy mesh main loop. Used
 * by the non-queue path (terrain, queue overflow, queue-disabled). */
static void sr_inline_bind_pbr_global(JceSceneRenderer *sr,
                                      const JcePbrMaterial *pbr,
                                      uint16_t view_id,
                                      JceScene *scene, EntityList *list)
{
    jce_pbr_material_bind(pbr, sr->renderer, view_id);
    /* Re-apply lights per-entity — see sr_bind_material_cb for rationale. */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);
    if (sr->shadow_valid && !sr->shadow_use_csm) {
        bgfx_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
        float shadow_vp[16];
        jce_vec3 shadow_dir = sr_resolve_shadow_light_direction(sr, scene, list);
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
}

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
    c.shadow_distance  = 0.0f;
    c.csm_split_lambda = -1.0f;
    c.fog_enabled            = false;
    c.fog                    = jce_volumetric_fog_default_params();
    c.fog_depth_tex_handle   = UINT16_MAX;
    return c;
}

JceSceneRenderer *jce_scene_renderer_create(JceRenderer *renderer,
                                            const JcePakArchive *pak,
                                            const JceSceneRendererCallbacks *cbs)
{
    if (!renderer) return NULL;

    JceSceneRenderer *sr = (JceSceneRenderer *)JCE_CALLOC(1, sizeof(JceSceneRenderer));
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
    sr->shadow_depth_fmt = depth_fmt;

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

    LOG_INFO(LOG_TAG, "[init] sky shader + uniforms");
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

    LOG_INFO(LOG_TAG, "[init] procedural meshes");
    /* Procedural meshes. */
    sr->cube_mesh     = jce_mesh_create_cube(1.0f);
    sr->plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    sr->sphere_mesh   = jce_mesh_create_sphere(0.5f);
    sr->capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    sr->cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    LOG_INFO(LOG_TAG, "[init] fallback textures");
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

    LOG_INFO(LOG_TAG, "[init] shadow map (depth_fmt=%d)", (int)depth_fmt);
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
                             0, 1, 0, BGFX_RESOLVE_NONE);
        sr->shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        sr->u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                              BGFX_UNIFORM_TYPE_SAMPLER, 1);
        sr->u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                              BGFX_UNIFORM_TYPE_MAT4, 1);
        sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->shadow_fbo);
        LOG_INFO(LOG_TAG, "[init] shadow_valid=%d", (int)sr->shadow_valid);
    }

    LOG_INFO(LOG_TAG, "[init] CSM cascades");
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
                                 0, 1, 0, BGFX_RESOLVE_NONE);
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
        LOG_INFO(LOG_TAG, "[init] csm_valid=%d", (int)sr->csm_valid);
    }

    LOG_INFO(LOG_TAG, "[init] light env");
    /* Multi-light env. */
    sr->light_env = jce_light_env_create();

    LOG_INFO(LOG_TAG, "[init] IBL uniforms + BRDF LUT");
    /* IBL uniforms + BRDF LUT. */
    sr->u_ibl_irradiance = bgfx_create_uniform("s_irradiance",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_prefilter  = bgfx_create_uniform("s_prefilter",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_brdf_lut   = bgfx_create_uniform("s_brdfLUT",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_params     = bgfx_create_uniform("u_iblParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Terrain shader bindings (lazy: created here so submit-time has
     * valid handles even when no terrain is bound). */
    sr->u_terrain_params = bgfx_create_uniform("u_terrainParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->s_terrain_splat  = bgfx_create_uniform("s_splatMap",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer1 = bgfx_create_uniform("s_layer1",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer2 = bgfx_create_uniform("s_layer2",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer0 = bgfx_create_uniform("s_albedo",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer3 = bgfx_create_uniform("s_emissive",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    for (int ti = 0; ti < 16; ti++)
        sr->terrain_cache[ti].splat_tex.idx = UINT16_MAX;

    LOG_INFO(LOG_TAG, "[init] BRDF LUT");
    {
        JceTexture brdf = jce_ibl_create_brdf_lut(256);
        sr->brdf_lut.idx = brdf.idx;
        LOG_INFO(LOG_TAG, "[init] BRDF LUT done: idx=%u", (unsigned)brdf.idx);
    }
    sr->skybox = NULL;
    sr->ibl_data = NULL;
    sr->skybox_active = false;
    sr->skybox_hdr_path[0] = '\0';

    LOG_INFO(LOG_TAG, "[init] sprite batch + postfx");
    /* Sprite batch. */
    sr->sprite_batch = jce_sprite_batch_create(256);

    /* PostFX pipeline. */
    sr->postfx_pipeline = jce_postfx_create(jce_allocator_default(), 1, 1);
    if (sr->postfx_pipeline) {
        if (!jce_postfx_load_shaders(sr->postfx_pipeline, pak))
            LOG_WARN(LOG_TAG, "postfx shaders failed to load");
    }

    LOG_INFO(LOG_TAG, "[init] render queue");
    LOG_INFO(LOG_TAG, "scene renderer created");
    /* Phase 2: per-frame material registry for queue-based instancing.
     * Queue itself is created lazily on first use to keep the create
     * path lean; render path wires the binder in Phase 3. */
    sr->render_queue = jce_rq_create(4096);
    sr->mat_count = 0;
    sr->frame_view_id = 0;
    sr->frame_shadow_vp_valid = false;
    return sr;
}

/* Public accessors so editors/tools can read the live animation state
 * without keeping a second cache of their own. */
struct JceAnimPlayer *jce_scene_renderer_get_anim_player(
    JceSceneRenderer *sr, const char *skeleton_path)
{
    if (!sr || !skeleton_path || !skeleton_path[0]) return NULL;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, skeleton_path) == 0)
            return (struct JceAnimPlayer *)e->player;
    }
    return NULL;
}

struct JceModel *jce_scene_renderer_get_model(
    JceSceneRenderer *sr, const char *skeleton_path)
{
    if (!sr || !skeleton_path || !skeleton_path[0]) return NULL;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, skeleton_path) == 0)
            return (struct JceModel *)e->model;
    }
    return NULL;
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

    /* Terrain cache. */
    for (int i = 0; i < 16; i++) {
        if (!sr->terrain_cache[i].used) continue;
        if (sr->terrain_cache[i].mesh)    jce_mesh_destroy(sr->terrain_cache[i].mesh);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        sr->terrain_cache[i].used = false;
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_terrain_params)) bgfx_destroy_uniform(sr->u_terrain_params);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_splat))  bgfx_destroy_uniform(sr->s_terrain_splat);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer1)) bgfx_destroy_uniform(sr->s_terrain_layer1);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer2)) bgfx_destroy_uniform(sr->s_terrain_layer2);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer0)) bgfx_destroy_uniform(sr->s_terrain_layer0);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer3)) bgfx_destroy_uniform(sr->s_terrain_layer3);

    if (BGFX_HANDLE_IS_VALID(sr->white_tex))      bgfx_destroy_texture(sr->white_tex);
    if (BGFX_HANDLE_IS_VALID(sr->checker_tex))    bgfx_destroy_texture(sr->checker_tex);
    if (BGFX_HANDLE_IS_VALID(sr->prog_sky))       bgfx_destroy_program(sr->prog_sky);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_colors))   bgfx_destroy_uniform(sr->u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_params))   bgfx_destroy_uniform(sr->u_sky_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_equirect)) bgfx_destroy_uniform(sr->u_sky_equirect);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_dir))    bgfx_destroy_uniform(sr->u_light_dir);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_color))  bgfx_destroy_uniform(sr->u_light_color);

    sr_destroy_shadow_targets(sr);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowMap)) bgfx_destroy_uniform(sr->u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowVP))  bgfx_destroy_uniform(sr->u_shadowVP);

    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
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
    if (sr->vfog)      jce_volumetric_fog_destroy(sr->vfog);

    if (BGFX_HANDLE_IS_VALID(sr->brdf_lut))         bgfx_destroy_texture(sr->brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_irradiance)) bgfx_destroy_uniform(sr->u_ibl_irradiance);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_prefilter))  bgfx_destroy_uniform(sr->u_ibl_prefilter);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_brdf_lut))   bgfx_destroy_uniform(sr->u_ibl_brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_params))     bgfx_destroy_uniform(sr->u_ibl_params);

    if (sr->sprite_batch)    jce_sprite_batch_destroy(sr->sprite_batch);
    if (sr->postfx_pipeline) jce_postfx_destroy(sr->postfx_pipeline);

    if (sr->cull_space) jce_space_destroy(sr->cull_space);
    if (sr->cull_aabbs) JCE_FREE(sr->cull_aabbs);
    if (sr->render_queue) jce_rq_destroy(sr->render_queue);

    JCE_FREE(sr);
}

uint16_t jce_scene_renderer_render(JceSceneRenderer *sr, JceScene *scene,
                                   const JceCamera *camera,
                                   uint16_t view_id_base, float dt_sec,
                                   const JceSceneRenderConfig *config)
{
    if (!sr || !scene) return view_id_base;
    JCE_PROFILE_ZONE_N("SceneRenderer::Render");

    /* Label views for GPU profilers (RenderDoc / bgfx debug overlay).
     * bgfx accepts repeat sets; names persist for the lifetime of the
     * view ID, so this is effectively cheap. */
    bgfx_set_view_name(view_id_base,                           "Scene/Color",        INT32_MAX);
    bgfx_set_view_name((uint16_t)(view_id_base + 10),          "Scene/ShadowSimple", INT32_MAX);
    for (uint16_t c = 0; c < JCE_CSM_MAX_CASCADES; ++c) {
        char nm[32];
        snprintf(nm, sizeof(nm), "Scene/ShadowCSM%u", (unsigned)c);
        bgfx_set_view_name((uint16_t)(view_id_base + 11 + c), nm, INT32_MAX);
    }

    JceSceneRenderConfig defcfg = jce_scene_render_config_default();
    const JceSceneRenderConfig *cfg = config ? config : &defcfg;
    const JceSceneRenderingSettings *scene_rendering =
        jce_scene_get_rendering_settings(scene);

    /* Apply optional config overrides. */
    if (cfg->shadow_map_size != 0) {
        sr_ensure_shadow_map_size(sr, cfg->shadow_map_size);
    } else if (scene_rendering && scene_rendering->shadow_resolution > 0) {
        sr_ensure_shadow_map_size(sr,
                                  (uint16_t)scene_rendering->shadow_resolution);
    }
    if (cfg->csm_cascades != 0) {
        sr->csm_cascade_count =
            cfg->csm_cascades < JCE_CSM_MAX_CASCADES
            ? cfg->csm_cascades : JCE_CSM_MAX_CASCADES;
    } else if (scene_rendering && scene_rendering->cascade_count > 0) {
        uint8_t cascades = (uint8_t)scene_rendering->cascade_count;
        sr->csm_cascade_count =
            cascades < JCE_CSM_MAX_CASCADES ? cascades : JCE_CSM_MAX_CASCADES;
    }

    /* Ensure wireframe is OFF before sky draws (sky's fullscreen quad must
     * render solid). The previous frame may have left it ON. Editor mode
     * only — runtime games own their own wireframe state. */
    if (sr->has_cbs && sr->cbs.load_texture)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* Forward the editor's selected view mode to the PBR shader.
     *   SHADED              → 0 (lit + textured)
     *   WIREFRAME            → 1 (host fills the wireframe pass; shader
     *                            still treated as shaded for the few
     *                            entities that hit the PBR path)
     *   TEXTURED             → 2 (unlit albedo only — raw base color)
     *   WIREFRAME_TEXTURED   → 3 (unlit albedo + host wireframe overlay)
     * Previously this was hard-coded to 0, which made TEXTURED look
     * identical to SHADED. */
    int sm = (int)cfg->view_mode;
    if (sm < 0) sm = 0;
    if (sm > 3) sm = 0;
    jce_pbr_material_set_view_mode(sm);

    /* Scene rendering settings own PostFX defaults; render config remains
     * the fallback for callers that render scenes without authored settings. */
    JcePostFXParams active_postfx = cfg->postfx;
    if (scene_rendering) {
        active_postfx.exposure =
            scene_rendering->exposure;
        active_postfx.gamma =
            scene_rendering->gamma;
        active_postfx.bloom_threshold =
            scene_rendering->bloom_threshold;
        active_postfx.bloom_intensity =
            scene_rendering->bloom_intensity;
        active_postfx.fxaa_span_max =
            scene_rendering->fxaa_span_max;
        active_postfx.vignette_intensity =
            scene_rendering->vignette_intensity;
        active_postfx.vignette_smoothness =
            scene_rendering->vignette_smoothness;
        active_postfx.chromatic_strength =
            scene_rendering->chromatic_strength;

        if (sr->postfx_pipeline) {
            for (int i = 0; i < JCE_POSTFX_COUNT &&
                 i < JCE_SCENE_RENDERING_POSTFX_COUNT; i++) {
                jce_postfx_enable(sr->postfx_pipeline,
                                  (JcePostFXType)i,
                                  scene_rendering->postfx_enabled[i]);
            }
        }
    }

    /* Blend active Volume components into postfx params before pushing. */
    if (camera) {
        jce_vec3 cp = jce_camera_get_position(camera);
        jce_volume_system_tick(scene, cp, &active_postfx);
    }

    /* Push postfx params. */
    if (sr->postfx_pipeline)
        jce_postfx_set_params(sr->postfx_pipeline, &active_postfx);

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
        float shadow_distance = cfg->shadow_distance;
        float split_lambda = cfg->csm_split_lambda;
        if (shadow_distance <= 0.0f && scene_rendering)
            shadow_distance = scene_rendering->shadow_distance;
        if (split_lambda < 0.0f)
            split_lambda = scene_rendering ? scene_rendering->split_lambda : 0.5f;
        sr_draw_shadow_pass(sr, scene, camera, &list, view_id_base,
                            vp_w, vp_h, shadow_distance, split_lambda);
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

    /* ── Volumetric fog (Stage 1 wiring) ──────────────────────────────
     * Renders fog into a private RT.  Composite into the caller's color
     * RT is a deferred Stage 2 (composite shader pending).  The result
     * texture handle is exposed via jce_scene_renderer_get_fog_result_texture()
     * so callers can consume it once the composite pass is in place. */
    sr->vfog_last_rendered = false;
    if (cfg->fog_enabled
        && jce_render_pipeline_is_feature_enabled("volumetric_fog")
        && cfg->fog_depth_tex_handle != UINT16_MAX
        && cfg->fog_rt_width > 0 && cfg->fog_rt_height > 0
        && sr->pak)
    {
        if (!sr->vfog) {
            JceVolumetricFogDesc d = { sr->pak, cfg->fog_rt_width, cfg->fog_rt_height };
            sr->vfog   = jce_volumetric_fog_create(&d);
            sr->vfog_w = cfg->fog_rt_width;
            sr->vfog_h = cfg->fog_rt_height;
        } else if (sr->vfog_w != cfg->fog_rt_width
                || sr->vfog_h != cfg->fog_rt_height) {
            jce_volumetric_fog_resize(sr->vfog, cfg->fog_rt_width, cfg->fog_rt_height);
            sr->vfog_w = cfg->fog_rt_width;
            sr->vfog_h = cfg->fog_rt_height;
        }
        if (sr->vfog) {
            JceVolumetricFogParams p = cfg->fog;
            p.near_plane = camera ? jce_camera_get_near(camera) : 0.1f;
            p.far_plane  = camera ? jce_camera_get_far(camera)  : 200.0f;
            jce_volumetric_fog_set_params(sr->vfog, &p);

            const jce_mat4 vmat = camera ? jce_camera_view(camera) : jce_m4_identity();
            const float aspect  = (float)cfg->fog_rt_width / (float)cfg->fog_rt_height;
            const jce_mat4 pmat = camera
                ? jce_camera_proj(camera, aspect, sr->homogeneous_depth)
                : jce_m4_identity();

            jce_volumetric_fog_render(sr->vfog,
                                      cfg->fog_depth_tex_handle,
                                      &vmat, &pmat,
                                      (uint16_t)(view_id_base + 15));
            sr->vfog_last_rendered = true;
        }
    }

    JCE_PROFILE_ZONE_END;
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

uint16_t jce_scene_renderer_get_fog_result_texture(const JceSceneRenderer *sr)
{
    if (!sr || !sr->vfog || !sr->vfog_last_rendered) return UINT16_MAX;
    return jce_volumetric_fog_get_result_texture(sr->vfog);
}

void jce_scene_renderer_composite_fog(JceSceneRenderer *sr, uint16_t view_id,
                                      uint16_t dst_fb_idx)
{
    if (!sr || !sr->vfog || !sr->vfog_last_rendered) return;
    jce_volumetric_fog_composite(sr->vfog, view_id, dst_fb_idx);
}

bool jce_scene_renderer_is_skybox_active(const JceSceneRenderer *sr)
{
    return sr ? sr->skybox_active : false;
}

void jce_scene_renderer_get_cull_stats(const JceSceneRenderer *sr,
                                        JceSceneCullStats *out)
{
    if (!out) return;
    if (!sr) {
        out->total = out->visible = out->culled = 0;
        out->enabled = false;
        return;
    }
    out->total   = sr->stat_total_entities;
    out->visible = sr->stat_visible_entities;
    out->culled  = sr->stat_culled_entities;
    out->enabled = sr->stat_culling_enabled;
}

void jce_scene_renderer_set_global_lod(JceSceneRenderer *sr,
                                        const JceLodGroup *group)
{
    if (!sr) return;
    if (sr->global_lod != group)
        memset(sr->lod_prev, 0, sizeof(sr->lod_prev));
    sr->global_lod = group;
}

JceMesh *jce_scene_renderer_get_builtin_mesh(JceSceneRenderer *sr, int shape)
{
    if (!sr) return NULL;
    switch (shape) {
    case 0: return sr->cube_mesh;
    case 1: return sr->sphere_mesh;
    case 2: return sr->plane_mesh;
    case 3: return sr->capsule_mesh;
    case 4: return sr->cylinder_mesh;
    default: return NULL;
    }
}

void jce_scene_renderer_invalidate_terrain(JceSceneRenderer *sr,
                                            const char *path)
{
    if (!sr) return;
    for (int i = 0; i < 16; i++) {
        if (!sr->terrain_cache[i].used) continue;
        if (path && *path &&
            strncmp(sr->terrain_cache[i].path, path,
                    sizeof sr->terrain_cache[i].path) != 0)
            continue;
        if (sr->terrain_cache[i].mesh)    jce_mesh_destroy(sr->terrain_cache[i].mesh);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        memset(&sr->terrain_cache[i], 0, sizeof sr->terrain_cache[i]);
    }
}

void jce_scene_renderer_get_lod_stats(const JceSceneRenderer *sr,
                                       JceSceneLodStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    for (int i = 0; i < JCE_LOD_MAX_LEVELS; i++)
        out->picks[i] = sr->stat_lod_picks[i];
    out->culled      = sr->stat_lod_culled;
    out->enabled     = sr->stat_lod_enabled;
    out->level_count = sr->global_lod ? sr->global_lod->count : 0;
}

void jce_scene_renderer_get_rq_stats(const JceSceneRenderer *sr,
                                      JceSceneRqStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    out->commands_in     = sr->stat_rq.commands_in;
    out->submits_out     = sr->stat_rq.submits_out;
    out->batches_merged  = sr->stat_rq.batches_merged;
    out->instances_total = sr->stat_rq.instances_total;
    out->enabled         = sr->stat_rq_active;
}

void jce_scene_renderer_get_occlusion_stats(const JceSceneRenderer *sr,
                                             JceSceneOcclusionStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    out->total    = sr->stat_occlusion.total_entities;
    out->visible  = sr->stat_occlusion.visible;
    out->occluded = sr->stat_occlusion.occluded;
    out->warm_up  = sr->stat_occlusion.warm_up;
    out->enabled  = sr->stat_occlusion_enabled;
}

void jce_scene_renderer_set_time_of_day(JceSceneRenderer        *sr,
                                         const JceTimeOfDayState *state)
{
    if (!sr) return;
    if (state) {
        sr->tod_state  = *state;
        sr->tod_active = true;
    } else {
        sr->tod_active = false;
    }
}

const JceTimeOfDayState *jce_scene_renderer_get_time_of_day(
    const JceSceneRenderer *sr)
{
    if (!sr || !sr->tod_active) return NULL;
    return &sr->tod_state;
}

void jce_scene_renderer_set_ambient_override(JceSceneRenderer *sr,
                                              const float       color_rgb[3],
                                              float             intensity)
{
    if (!sr) return;
    if (color_rgb) {
        sr->ambient_override_color     = jce_v3(color_rgb[0], color_rgb[1], color_rgb[2]);
        sr->ambient_override_intensity = intensity;
        sr->ambient_override_active    = true;
    } else {
        sr->ambient_override_active    = false;
    }
}
