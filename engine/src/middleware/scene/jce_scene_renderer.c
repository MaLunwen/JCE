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

#include "jce_sr_internal.h"   /* JceSceneRenderer struct + Sr* types (split foundation) */

/* ── Entity collection ────────────────────────────────────────────── */

/* EntityList is defined in jce_sr_internal.h so the split jce_sr_*.c modules
 * share one definition. */

/* User-data for collect_entity_cb. Carries the focus-bounded ("draw distance")
 * cull state in addition to the output list. When `enabled` is false (the
 * default — see jce_scene_render_config_default / memset'd configs), the
 * collect is byte-identical to the original "collect all" behaviour. */
typedef struct {
    EntityList *list;
    JceScene   *scene;
    float       fx, fz;   /* focus point (XZ); y is intentionally ignored */
    float       r2;       /* (radius + margin) squared; horizontal cull */
    bool        enabled;
} SrCollectCtx;

static void collect_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    SrCollectCtx *ctx = (SrCollectCtx *)ud;
    EntityList   *list = ctx->list;
    if (list->count >= SR_MAX_ENTITIES) return;

    if (ctx->enabled && jce_scene_has_transform(ctx->scene, e)) {
        /* Horizontal (XZ) squared distance from the focus point. A high
         * survey camera looking at the ground must still collect the ground
         * area, so Y differences must NOT cull. Entities with no transform
         * fall through and are always collected. */
        JceTransform *t = jce_scene_get_transform(ctx->scene, e);
        if (t) {
            float dx = t->position.x - ctx->fx;
            float dz = t->position.z - ctx->fz;
            if (dx * dx + dz * dz > ctx->r2)
                return; /* outside draw distance — skip */
        }
    }

    list->entities[list->count++] = e;
}

/* ── Forward declarations for Phase 3 helpers ─────────────────────── */
/* The material-registry helpers (sr_reset_material_cache / sr_compute_material_key
 * / sr_register_material / sr_bind_material_cb), sr_inline_bind_pbr_global and
 * sr_extract_frustum_planes are now declared in jce_sr_internal.h (cross-module
 * helpers consumed by jce_sr_draw.c). */

void sr_rq_flush_queue_and_collect(JceSceneRenderer *sr,
                                          JceRenderQueue *q)
{
    if (!sr || !q) return;
    jce_rq_flush(q, sr->renderer);
    JceRenderQueueStats s;
    jce_rq_last_stats(q, &s);
    sr->stat_rq.commands_in     += s.commands_in;
    sr->stat_rq.submits_out     += s.submits_out;
    sr->stat_rq.batches_merged  += s.batches_merged;
    sr->stat_rq.instances_total += s.instances_total;
    sr->stat_rq_active           = true;
}

void sr_rq_flush_and_collect(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr_rq_flush_queue_and_collect(sr, sr->render_queue);
}

bool entity_enabled(JceScene *scene, JceEntity e)
{
    if (jce_scene_has_editor_meta(scene, e)) {
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
        if (m && !m->enabled) return false;
    }
    return true;
}

/* ── Texture cache (resolved paths) ───────────────────────────────── */

/* Async decode job: worker writes `result` + flips `done`; the render
 * thread (sr_tex_poll) uploads `result` and frees the job. */
struct SrTexJob {
    const JcePakArchive *pak;
    char                 path[256];
    int                  sampler;
    JceTextureCpu       *result;   /* worker → main */
    JceAtomicI32        *done;     /* 0 working, 1 finished */
};

static void sr_tex_worker(void *arg)
{
    struct SrTexJob *j = (struct SrTexJob *)arg;
    j->result = jce_texture_decode_cpu(j->pak, j->path, j->sampler);
    jce_atomic_i32_store(j->done, 1);
}

/* RENDER thread, per-frame: upload any finished async texture decodes. */
static void sr_tex_poll(JceSceneRenderer *sr)
{
    if (!sr || sr->tex_inflight == 0) return;
    for (int i = 0; i < sr->tex_cache_count; i++) {
        if (!sr->tex_cache[i].pending) continue;
        struct SrTexJob *j = sr->tex_cache[i].job;
        if (!j || jce_atomic_i32_load(j->done) == 0) continue;

        if (sr->tex_cache[i].thr) {
            jce_thread_join(sr->tex_cache[i].thr);
            sr->tex_cache[i].thr = NULL;
        }
        /* Upload on this (render) thread; consumes j->result. */
        JceTexture tex = jce_texture_upload_cpu(j->result);
        sr->tex_cache[i].tex     = tex;
        sr->tex_cache[i].failed  = !jce_texture_valid(tex);
        sr->tex_cache[i].pending = false;
        sr->tex_cache[i].job     = NULL;
        jce_atomic_i32_destroy(j->done);
        JCE_FREE(j);
        if (sr->tex_inflight > 0) sr->tex_inflight--;
    }
}

/* sr_resolve_texture / sr_resolve_texture2 are declared in jce_sr_internal.h
 * (cross-module core helpers). */

JceTexture sr_resolve_texture(JceSceneRenderer *sr, const char *path)
{
    return sr_resolve_texture2(sr, path, NULL);
}

JceTexture sr_resolve_texture2(JceSceneRenderer *sr,
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
            /* Pending: tex is still invalid → caller falls back to white
             * until sr_tex_poll() uploads the finished decode. */
            return sr->tex_cache[i].failed ? invalid : sr->tex_cache[i].tex;
        }
    }

    /* Cache miss.  Decode off the render thread (PAK decompress + image
     * decode), upload later in sr_tex_poll().  Bounded concurrency keeps
     * a scene full of first-seen textures from spawning a thread storm —
     * over the cap we leave the path uncached so it retries next frame. */
    if (!sr->pak ||
        sr->tex_cache_count >= SR_TEX_CACHE_MAX ||
        sr->tex_inflight >= SR_TEX_MAX_INFLIGHT)
        return invalid;

    struct SrTexJob *j = (struct SrTexJob *)JCE_MALLOC(sizeof(*j));
    if (!j) return invalid;
    memset(j, 0, sizeof(*j));
    j->pak     = sr->pak;
    j->sampler = JCE_TEX_CLAMP;   /* matches the former jce_texture_load() */
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->done = jce_atomic_i32_create(0);

    int idx = sr->tex_cache_count++;
    snprintf(sr->tex_cache[idx].path, sizeof(sr->tex_cache[idx].path),
             "%s", path);
    sr->tex_cache[idx].tex     = invalid;
    sr->tex_cache[idx].used    = true;
    sr->tex_cache[idx].failed  = false;
    sr->tex_cache[idx].pending = true;
    sr->tex_cache[idx].job     = j;
    sr->tex_cache[idx].thr     = jce_thread_create(sr_tex_worker, j,
                                                   "jce_sr_tex");
    if (!sr->tex_cache[idx].thr) {
        /* No worker thread: decode + upload inline this frame. */
        sr_tex_worker(j);
        JceTexture tex = jce_texture_upload_cpu(j->result);
        sr->tex_cache[idx].tex     = tex;
        sr->tex_cache[idx].failed  = !jce_texture_valid(tex);
        sr->tex_cache[idx].pending = false;
        sr->tex_cache[idx].job     = NULL;
        jce_atomic_i32_destroy(j->done);
        JCE_FREE(j);
        return tex;
    }
    sr->tex_inflight++;
    return invalid;   /* white this frame; pops in when the decode lands */
}

/* ── Shader Graph custom-program cache ─────────────────────────────── */

static bool sr_ends_with_ci(const char *s, const char *suffix);

/* Resolve (and cache) the custom shader program declared by a material
 * file's Shader Graph reference.  Returns UINT16_MAX when the material has
 * no custom shader (the common case) or cannot be resolved.  Cached by
 * path so the .mat.json is parsed at most once per unique material. */
JceShaderHandle sr_resolve_custom_program(JceSceneRenderer *sr,
                                                 const char *material_path)
{
    JceShaderHandle none = { UINT16_MAX };
    if (!sr || !material_path || !material_path[0]) return none;
    if (!sr_ends_with_ci(material_path, ".mat.json")) return none;

    for (int i = 0; i < sr->prog_cache_count; i++) {
        if (sr->prog_cache[i].used &&
            strncmp(sr->prog_cache[i].path, material_path,
                    sizeof(sr->prog_cache[i].path)) == 0)
            return sr->prog_cache[i].program;
    }

    /* Parse the material file; the loader links any persisted graph shader
     * into custom_program for us.  Only the program handle is kept here. */
    JceShaderHandle prog = none;
    if (jce_fs_host_exists_file(material_path)) {
        JcePbrMaterial m;
        char tex_paths[5][256];
        if (jce_pbr_material_load_json(material_path, &m, tex_paths) &&
            m.custom_program != UINT16_MAX)
            prog.idx = m.custom_program;
    }

    if (sr->prog_cache_count < SR_MAT_PROG_CACHE_MAX) {
        int idx = sr->prog_cache_count++;
        snprintf(sr->prog_cache[idx].path, sizeof(sr->prog_cache[idx].path),
                 "%s", material_path);
        sr->prog_cache[idx].program  = prog;
        sr->prog_cache[idx].used     = true;
        sr->prog_cache[idx].resolved = true;
    }
    return prog;
}

/* ── Model cache ──────────────────────────────────────────────────── */

static char sr_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool sr_ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t tlen = strlen(suffix);
    if (tlen > slen) return false;
    s += slen - tlen;
    for (size_t i = 0; i < tlen; i++) {
        if (sr_ascii_lower(s[i]) != sr_ascii_lower(suffix[i]))
            return false;
    }
    return true;
}

bool sr_is_gltf_model_path(const char *path)
{
    return sr_ends_with_ci(path, ".gltf") || sr_ends_with_ci(path, ".glb");
}

/* Async glTF decode job: worker writes `cpu` + flips `done`; the render
 * thread (sr_model_poll) uploads `cpu` to a JceModel and frees the job. */
struct SrModelJob {
    const JcePakArchive *pak;
    char                 path[256];
    JceModelCpu         *cpu;      /* worker → main */
    JceAtomicI32        *done;     /* 0 working, 1 finished */
};

static void sr_model_worker(void *arg)
{
    struct SrModelJob *j = (struct SrModelJob *)arg;
    j->cpu = jce_model_decode_gltf_cpu(j->pak, j->path);   /* no bgfx */
    jce_atomic_i32_store(j->done, 1);
}

/* RENDER thread, per-frame: upload any finished async model decodes. */
static void sr_model_poll(JceSceneRenderer *sr)
{
    if (!sr || sr->model_inflight == 0) return;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (!e->pending) continue;
        struct SrModelJob *j = e->job;
        if (!j || jce_atomic_i32_load(j->done) == 0) continue;

        if (e->thr) { jce_thread_join(e->thr); e->thr = NULL; }

        JceModel *model = jce_model_upload_gltf_cpu(j->cpu);  /* consumes cpu */
        e->model   = model;
        e->failed  = (model == NULL);
        e->pending = false;
        e->job     = NULL;
        jce_atomic_i32_destroy(j->done);
        JCE_FREE(j);
        if (sr->model_inflight > 0) sr->model_inflight--;
        if (!model)
            LOG_WARN(LOG_TAG, "model cache: cannot load %s (will not retry)", e->path);
    }
}

static uint32_t sr_path_hash(const char *s)
{
    return jce_fnv1a32_str(s);   /* shared FNV-1a (jce_hash.h) */
}

SrModelCache *sr_get_model(JceSceneRenderer *sr, const char *path,
                           uint32_t entity_id)
{
    (void)entity_id;   /* model is shared by path; instance state is per-entity */
    if (!path || path[0] == '\0') return NULL;

    /* O(1) open-addressing lookup keyed by path hash.  This runs tens of
     * thousands of times per frame at full-load (color + 4 shadow cascades x
     * every entity) — a linear strcmp scan here was the full-load CPU wall.
     * The cache NEVER deletes entries (a loaded model stays for the renderer's
     * life), so linear probing with break-on-empty is correct: no tombstones
     * can break a key's probe chain, and the first empty slot on the chain is
     * exactly where the key would be inserted. */
    uint32_t h = sr_path_hash(path);
    int free_slot = -1;
    for (int probe = 0; probe < SR_MODEL_CACHE_MAX; probe++) {
        int i = (int)((h + (uint32_t)probe) % (uint32_t)SR_MODEL_CACHE_MAX);
        SrModelCache *e = &sr->model_cache[i];
        if (!e->used) { free_slot = i; break; }   /* empty → not cached; insert here */
        if (e->path_hash == h && strcmp(e->path, path) == 0) {
            /* Pending: model is still NULL → callers skip until sr_model_poll
             * uploads it.  Failed: never retry. */
            return e->failed ? NULL : e;
        }
    }
    if (free_slot < 0) return NULL;   /* table full */

    /* Editor mode: the callback owns its own (async) asset cache — keep the
     * synchronous handoff. */
    if (sr->has_cbs && sr->cbs.load_model) {
        JceModel *model = sr->cbs.load_model(path, sr->cbs.userdata);
        SrModelCache *e = &sr->model_cache[free_slot];
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->path_hash = h;
        e->model  = model;
        e->used   = true;
        e->failed = (model == NULL);
        if (!model)
            LOG_WARN(LOG_TAG, "model cache: cannot load %s (will not retry)", path);
        return model ? e : NULL;
    }

    /* Runtime mode: decode off the render thread (parse + vertex extraction +
     * image decode), upload later in sr_model_poll().  Bounded concurrency;
     * over the cap we leave the path uncached so it retries next frame. */
    if (!sr->pak || sr->model_inflight >= SR_MODEL_MAX_INFLIGHT)
        return NULL;

    struct SrModelJob *j = (struct SrModelJob *)JCE_MALLOC(sizeof(*j));
    if (!j) return NULL;
    memset(j, 0, sizeof(*j));
    j->pak = sr->pak;
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->done = jce_atomic_i32_create(0);

    SrModelCache *e = &sr->model_cache[free_slot];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->path_hash = h;
    e->model   = NULL;
    e->used    = true;
    e->failed  = false;
    e->pending = true;
    e->job     = j;
    e->thr     = jce_thread_create(sr_model_worker, j, "jce_sr_model");
    if (!e->thr) {
        /* No worker thread: decode + upload inline this frame. */
        sr_model_worker(j);
        e->model   = jce_model_upload_gltf_cpu(j->cpu);
        e->failed  = (e->model == NULL);
        e->pending = false;
        e->job     = NULL;
        jce_atomic_i32_destroy(j->done);
        JCE_FREE(j);
        return e->model ? e : NULL;
    }
    sr->model_inflight++;
    return NULL;   /* pending; entity skipped until the decode lands */
}



/* ── Mesh resolution ──────────────────────────────────────────────── */

JceMesh *sr_resolve_mesh(JceSceneRenderer *sr, const JceMeshRenderer *mr)
{
    if (!mr) return NULL;
    JceMesh *mesh = NULL;
    if (mr->mesh_path[0] != '\0') {
        if (sr_is_gltf_model_path(mr->mesh_path))
            return NULL;
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

/* cull_idx: when >= 0 and sr->ecull[cull_idx].world_valid, reuse the world
 * matrix cached during the per-frame ecull build (#8) instead of re-walking the
 * parent chain.  Pass -1 from sites with no per-frame entity index (identical
 * result, just no cache). */
bool sr_build_entity_model(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, int cull_idx, jce_mat4 *out_model,
                                  JceMesh **out_mesh)
{
    if (!scene || e == JCE_ENTITY_INVALID) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    /* Compose the full world matrix up the parent chain (roots → local), or
     * reuse the per-frame cached one (#8 — byte-identical, same source). */
    if (cull_idx >= 0 && sr->ecull && sr->ecull[cull_idx].world_valid)
        *out_model = sr->ecull[cull_idx].world;
    else
        *out_model = jce_scene_get_world_matrix(scene, e);

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_RENDERER)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            *out_mesh = sr_resolve_mesh(sr, mr);
        }
        /* Terrain entities are NOT merged into a single mesh any more — they
         * are drawn chunk-by-chunk (sr_draw_terrain_chunks) with per-chunk
         * frustum culling + distance LOD.  Here we only ensure the cache slot
         * is loaded; *out_mesh stays NULL so the caller routes to the terrain
         * branch instead of the generic mesh path. */
        if (!*out_mesh && jce_scene_has_terrain(scene, e)) {
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
            if (tc && tc->visible && tc->terrain_path[0])
                (void)sr_terrain_find_or_load_slot(sr, tc->terrain_path);
        }
    }
    return true;
}

/* ── Terrain per-chunk draw (P1-terrain-lod) ──────────────────────────
 *
 * Replaces the old "merge every chunk into one ~16M-vert mesh" draw.  For each
 * terrain entity we walk its chunk grid and, per chunk:
 *   1. transform the chunk's local AABB by the entity world matrix,
 *   2. frustum-cull it against the camera (skip if fully outside),
 *   3. pick a LOD from the camera→chunk-centre distance,
 *   4. build/cache the chunk mesh (with skirts) at that LOD,
 *   5. re-bind transform + terrain textures/params and submit one draw.
 * Step 5 is repeated per chunk because jce_mesh_submit_terrain discards all
 * bound state (BGFX_DISCARD_ALL) after each submit. */

/* AABB-vs-frustum: returns true if the box is at least partially inside. */
bool sr_aabb_in_frustum(const jce_vec4 planes[6],
                        jce_vec3 mn, jce_vec3 mx)
{
    return jce_aabb_in_frustum(planes, mn, mx);   /* shared jce_frustum.h */
}

/* World-space AABB of a local box transformed by `m`. */
void sr_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                       jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    jce_transform_aabb(m, lmn, lmx, out_mn, out_mx);   /* shared jce_frustum.h */
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

jce_vec3 sr_light_world_shine_direction(const jce_vec3 *comp_dir,
                                               const JceTransform *xf)
{
    jce_vec3 dir = sr_light_shine_direction(comp_dir);
    if (xf) {
        jce_quat q = jce_q_normalize(xf->rotation);
        dir = jce_q_rotate(q, dir);
    }
    return sr_light_shine_direction(&dir);
}

bool sr_resolve_primary_dir_light(JceSceneRenderer *sr,
                                         JceScene *scene,
                                         EntityList *list,
                                         bool shadow_only,
                                         jce_vec3 *out_to_light,
                                         jce_vec3 *out_color,
                                         float *out_intensity)
{
    if (sr && sr->tod_active) {
        /* Time-of-day is an authored scene driver, not a fallback preview:
         * when enabled, its sun is the primary directional light for both
         * PBR and CSM shadows. */
        if (out_to_light) *out_to_light = sr->tod_state.sun_direction;
        if (out_color) *out_color = sr->tod_state.sun_color;
        if (out_intensity) *out_intensity = 1.0f;
        return true;
    }

    bool have_any = false; uint32_t dir_seen = 0;

    if (scene && list) {
        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            if (!jce_scene_has_dir_light(scene, e)) continue;
            if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DIR_LIGHT)) continue;
            if (dir_seen++ >= JCE_MAX_DIR_LIGHTS) continue;
            JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
            if (!dl) continue;
            have_any = true;
            if (shadow_only && !dl->casts_shadow) continue;

            JceTransform *xf = jce_scene_get_transform(scene, e);
            jce_vec3 shine = sr_light_world_shine_direction(&dl->direction, xf);
            if (out_to_light) *out_to_light = jce_v3_scale(shine, -1.0f);
            if (out_color) *out_color = dl->color;
            if (out_intensity) *out_intensity = dl->intensity > 0.0f ? dl->intensity : 1.0f;
            return true;
        }
    }

    return false;
}



/* ── Public API ───────────────────────────────────────────────────── */

/* ── Material registry (Phase 2) ──────────────────────────────────────
 * Per-frame registry mapping a material_key → texture/uniform snapshot.
 * Built during scene_renderer's mesh walk, consumed by sr_bind_material_cb
 * once jce_render_queue starts a new material run. Wired into the queue
 * in Phase 3. */

uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex)
{
    uint32_t h = JCE_FNV1A32_INIT;
    /* Texture handles (idx is enough — invalid = UINT16_MAX). */
    h = jce_fnv1a32_append(h, &pbr->albedo_map.idx,             sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->metallic_roughness_map.idx, sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->normal_map.idx,             sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->ao_map.idx,                 sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->emissive_map.idx,           sizeof(uint16_t));
    /* Factors. */
    h = jce_fnv1a32_append(h, pbr->base_color_factor, sizeof(pbr->base_color_factor));
    h = jce_fnv1a32_append(h, &pbr->metallic_factor,  sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->roughness_factor, sizeof(float));
    h = jce_fnv1a32_append(h, pbr->emissive_factor,   sizeof(pbr->emissive_factor));
    h = jce_fnv1a32_append(h, &pbr->normal_scale,     sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->ao_strength,      sizeof(float));
    /* State. */
    uint32_t am = (uint32_t)pbr->alpha_mode;
    h = jce_fnv1a32_append(h, &am,                  sizeof(am));
    h = jce_fnv1a32_append(h, &pbr->alpha_cutoff,   sizeof(float));
    uint8_t ds = pbr->double_sided ? 1u : 0u;
    h = jce_fnv1a32_append(h, &ds,                  sizeof(ds));
    /* Terrain pseudo-fields (slot ensures distinct splat/layer textures). */
    uint8_t it = is_terrain ? 1u : 0u;
    h = jce_fnv1a32_append(h, &it, sizeof(it));
    int32_t ts = (int32_t)terrain_slot;
    h = jce_fnv1a32_append(h, &ts, sizeof(ts));
    /* Terrain layer texture handles also folded in so two terrain entities
     * with the same slot but different runtime layer textures still split
     * into separate batches (rare today, but keeps key correctness). */
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++)
            h = jce_fnv1a32_append(h, &terrain_layer_tex[li].idx, sizeof(uint16_t));
    }
    return h ? h : 1u;
}

/* Resets the per-frame registry. Call at the top of each sr_render. */
void sr_reset_material_cache(JceSceneRenderer *sr)
{
    sr->mat_count = 0;
    sr->bind_memo_key = 0;
    sr->bind_memo_idx = 0;
}

/* Look up an existing entry by key, or append a new one. Returns the
 * resolved key (== input on success). On overflow returns 0 and emits
 * a one-shot warning per renderer; caller treats key=0 as "skip queue,
 * submit directly" (Phase 3 fallback). */
uint32_t sr_register_material(JceSceneRenderer *sr,
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

/* Render-queue binder callback. Invoked once per SUBMIT during
 * jce_rq_flush (bind-per-submit — see the rationale in jce_rq_flush).
 * Re-binds all textures / uniforms that jce_mesh_submit_pbr USED to bind
 * inline before it was decomposed. Frame-level state (shadow VP, IBL
 * handles) lives on `sr` directly. */
void sr_bind_material_cb(uint32_t material_key, void *user)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    if (!sr || material_key == 0u) return;

    SrMaterialEntry *e = NULL;
    if (sr->bind_memo_key == material_key &&
        sr->bind_memo_idx < sr->mat_count &&
        sr->mat_cache[sr->bind_memo_idx].key == material_key) {
        e = &sr->mat_cache[sr->bind_memo_idx];
    } else {
        for (uint32_t i = 0; i < sr->mat_count; i++) {
            if (sr->mat_cache[i].key == material_key) {
                e = &sr->mat_cache[i];
                sr->bind_memo_key = material_key;
                sr->bind_memo_idx = i;
                break;
            }
        }
    }
    if (!e) return;

    /* PBR textures + factor uniforms. */
    jce_pbr_material_bind(&e->pbr, sr->renderer, sr->frame_view_id);

    /* Lighting uniforms (u_dirLights / u_pointLights / u_spotLights /
     * u_lightCounts / u_ambientColor / u_cameraPos). Each bgfx draw item
     * records only the uniform updates issued since the last
     * state-discarding submit and replays them in view-SORTED draw order,
     * so every submit must carry the full light state in its own update
     * range — otherwise a draw inherits whatever the previously sorted
     * draw set (the historic symptom: only the first submitted entity
     * sampled real lights and everything after rendered unlit). The env
     * packs once per frame; this call mostly replays cached bytes. */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);

    sr_bind_frame_shadow_state(sr);

    /* IBL. */
    float ibl_params[4] = {
        0.0f, 4.0f, 0.0f,
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
            /* y = max prefilter mip LEVEL = (mip count - 1); shader scales
             * perceptual roughness [0,1] by this. Use the ACTUAL count, not
             * a hardcoded 5.0 (the prefilter caps mips at 8 / by face size). */
            uint32_t mips = jce_ibl_get_prefilter_mips(sr->ibl_data);
            if (mips > 1) ibl_params[1] = (float)(mips - 1);
        }
    }
    /* Baked GI override: reflection-probe cubemap (stages 6/7) + SH9 ambient.
     * Must run after the sky-IBL bind so a local probe wins, and before
     * u_iblParams upload since it can force IBL on. */
    sr_bind_baked_gi(sr, ibl_params);
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

    /* Forward+ per-submit cluster bind: stage 14 = s_cluster + the 3 cluster
     * uniforms, ONLY when the clustered path is active this frame AND this is
     * a PBR (non-terrain) draw using the fs_pbr_fwdplus variant.  Terrain uses
     * its own shader and binds stage 14 to a layer texture above, so it is
     * excluded.  No-op when inactive -> default program's s_iesLut(14) bind
     * (from the cookie/IES path) is left untouched. */
    if (sr->fp_active_frame && !e->is_terrain)
        jce_forwardplus_bind(sr->forwardplus);
}

/* Suppress unused-static warnings until Phase 3 wires these in. */
/* Inline binding helper extracted from the legacy mesh main loop. Used
 * by the non-queue path (terrain, queue overflow, queue-disabled). */
void sr_inline_bind_pbr_global(JceSceneRenderer *sr,
                               const JcePbrMaterial *pbr,
                               uint16_t view_id,
                               JceScene *scene, EntityList *list)
{
    (void)scene;
    (void)list;

    jce_pbr_material_bind(pbr, sr->renderer, view_id);
    /* Re-apply lights per-entity — see sr_bind_material_cb for rationale. */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);
    sr_bind_frame_shadow_state(sr);
    float ibl_params[4] = {
        0.0f, 4.0f, 0.0f,
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
            /* y = max prefilter mip LEVEL = (mip count - 1); see binder cb. */
            uint32_t mips = jce_ibl_get_prefilter_mips(sr->ibl_data);
            if (mips > 1) ibl_params[1] = (float)(mips - 1);
        }
    }
    /* Baked GI override (see sr_bind_material_cb). */
    sr_bind_baked_gi(sr, ibl_params);
    bgfx_set_uniform(sr->u_ibl_params, ibl_params, 1);
}

JceSceneRenderConfig jce_scene_render_config_default(void)
{
    JceSceneRenderConfig c;
    memset(&c, 0, sizeof(c));
    /* #1 — default frustum culling ON.  The two-pass parallel culler
     * (sr_compute_visible) is already proven in the editor scene viewport, but
     * the shipped runtime / jce_default_main path never set this, so the played
     * game submitted every collected entity to the color + shadow + prepass
     * passes with no view-frustum reject.  The editor opts in separately, so
     * this is a no-op there and a fix for the shipped game. */
    c.frustum_culling  = true;
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
    c.ssr_color_tex_handle   = UINT16_MAX;   /* SSR off unless caller sets it */
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

    /* Per-frame per-entity cull cache (SrEntityCull).  Heap-allocated once here
     * (32768 entries ≈ 1 MB — too big for the stack) and rebuilt each frame; the
     * shadow/prepass cull sites read it instead of recomputing the world AABB. */
    sr->ecull = (SrEntityCull *)JCE_CALLOC(SR_MAX_ENTITIES, sizeof(SrEntityCull));
    if (!sr->ecull) { JCE_FREE(sr); return NULL; }

    /* Invalidate handles. */
    sr->white_tex.idx       = UINT16_MAX;
    sr->checker_tex.idx     = UINT16_MAX;
    sr->prog_sky.idx        = UINT16_MAX;
    sr->u_sky_colors.idx    = UINT16_MAX;
    sr->u_sky_params.idx    = UINT16_MAX;
    sr->u_sky_equirect.idx  = UINT16_MAX;
    sr->u_sky_perez.idx     = UINT16_MAX;
    sr->u_sky_zenith.idx    = UINT16_MAX;
    sr->u_sky_sun_dir.idx   = UINT16_MAX;
    sr->u_light_dir.idx     = UINT16_MAX;
    sr->u_light_color.idx   = UINT16_MAX;
    sr->shadow_tex.idx      = UINT16_MAX;
    sr->shadow_fbo.idx      = UINT16_MAX;
    sr->u_shadowMap.idx     = UINT16_MAX;
    sr->u_shadowVP.idx      = UINT16_MAX;
    sr->local_atlas_tex.idx       = UINT16_MAX;
    sr->local_atlas_fbo.idx       = UINT16_MAX;
    sr->u_local_shadow_map.idx    = UINT16_MAX;
    sr->u_local_shadow_vp.idx     = UINT16_MAX;
    sr->u_local_shadow_params.idx = UINT16_MAX;
    sr->u_local_shadow_bias.idx   = UINT16_MAX;
    sr->u_spot_shadow_slot.idx    = UINT16_MAX;
    sr->u_point_shadow_slot.idx   = UINT16_MAX;
    sr->u_csm_vp.idx        = UINT16_MAX;
    sr->u_csm_splits.idx    = UINT16_MAX;
    sr->u_csm_params.idx    = UINT16_MAX;
    sr->u_csm_bias_scales.idx = UINT16_MAX;
    sr->u_shadow_quality.idx  = UINT16_MAX;
    sr->brdf_lut.idx        = UINT16_MAX;
    sr->u_ibl_irradiance.idx = UINT16_MAX;
    sr->u_ibl_prefilter.idx  = UINT16_MAX;
    sr->u_ibl_brdf_lut.idx   = UINT16_MAX;
    sr->u_ibl_params.idx     = UINT16_MAX;
    sr->u_sh9.idx            = UINT16_MAX;
    sr->u_gi_params.idx      = UINT16_MAX;
    sr->gi_probe_spec.idx    = UINT16_MAX;
    sr->gi_probe_irr.idx     = UINT16_MAX;
    sr->tilemap_shared_ib.idx = UINT16_MAX;
    sr->tilemap_s_tex.idx     = UINT16_MAX;
    /* SSAO depth RT + result handle: invalidate (JCE_CALLOC zeros to idx 0 =
     * a VALID handle). u_ssao_params is created explicitly below. */
    sr->ssao_depth_tex.idx    = UINT16_MAX;
    sr->ssao_depth_fbo.idx    = UINT16_MAX;
    sr->ssao_ao_idx           = UINT16_MAX;
    sr->ssr_result_idx        = UINT16_MAX;
    sr->ssao_normal_tex.idx   = UINT16_MAX;
    sr->prog_gbuffer.idx      = UINT16_MAX;
    /* TAA motion-vector G-buffer: invalidate handles (lazy-created). */
    sr->ssao_velocity_tex.idx        = UINT16_MAX;
    sr->prog_gbuffer_vel.idx         = UINT16_MAX;
    sr->prog_gbuffer_vel_skinned.idx = UINT16_MAX;
    sr->u_prevModel.idx              = UINT16_MAX;
    sr->u_prevViewProj.idx           = UINT16_MAX;
    sr->u_curViewProj.idx            = UINT16_MAX;
    sr->u_prevBones.idx              = UINT16_MAX;
    /* Water program + uniforms are created lazily on first water submit;
     * invalidate here because JCE_CALLOC zeros to idx 0 (a VALID bgfx
     * handle), which would make BGFX_HANDLE_IS_VALID falsely true. */
    sr->prog_water.idx           = UINT16_MAX;
    sr->u_water_wave_a.idx        = UINT16_MAX;
    sr->u_water_wave_b.idx        = UINT16_MAX;
    sr->u_water_params.idx        = UINT16_MAX;
    sr->u_water_time.idx          = UINT16_MAX;
    sr->u_water_color_shallow.idx = UINT16_MAX;
    sr->u_water_color_deep.idx    = UINT16_MAX;
    sr->u_water_shading.idx       = UINT16_MAX;
    sr->u_water_mode.idx          = UINT16_MAX;
    sr->s_water_disp.idx          = UINT16_MAX;
    for (int i = 0; i < SR_WATER_SLOT_MAX; i++)
        sr->water_cache[i].fft_tex.idx = UINT16_MAX;
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
        if (sz < 512)  sz = 512;   /* honor the LOW-tier 512 recommendation
                                      (was floored to 1024 → 4x shadow VRAM on
                                      the 512MB / no-GPU baseline) */
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
    sr->u_sky_perez    = bgfx_create_uniform("u_sky_perez",
                                             BGFX_UNIFORM_TYPE_VEC4, 4);
    sr->u_sky_zenith   = bgfx_create_uniform("u_sky_zenith",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_sky_sun_dir  = bgfx_create_uniform("u_sky_sun_dir",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
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
        /* Tier-scale the default cascade count: each cascade is a full-scene
           depth pass, so old / integrated GPUs render fewer. A per-scene or
           per-config override (jce_scene_renderer_render, below) still wins. */
        switch (jce_renderer_get_tier()) {
        case JCE_GPU_TIER_LOW:    sr->csm_cascade_count = 1; break;
        case JCE_GPU_TIER_MEDIUM: sr->csm_cascade_count = 2; break;
        default:                  sr->csm_cascade_count = JCE_CSM_MAX_CASCADES; break;
        }
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

    /* Local (spot/point) shadow atlas uniforms — P1. The atlas texture +
       FBO are (re)created in sr_create_shadow_targets so they track the
       shadow resolution; the uniforms live for the renderer's lifetime. */
    sr->u_local_shadow_map = bgfx_create_uniform("s_localShadowMap",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_local_shadow_vp = bgfx_create_uniform("u_localShadowVP",
        BGFX_UNIFORM_TYPE_MAT4, JCE_MAX_LOCAL_SHADOWS);
    sr->u_local_shadow_params = bgfx_create_uniform("u_localShadowParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_local_shadow_bias = bgfx_create_uniform("u_localShadowBias",
        BGFX_UNIFORM_TYPE_VEC4, 1);   /* 4 lanes = per-slot bias for JCE_MAX_LOCAL_SHADOWS=4 */
    sr->u_spot_shadow_slot = bgfx_create_uniform("u_spotShadowSlot",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_point_shadow_slot = bgfx_create_uniform("u_pointShadowSlot",
        BGFX_UNIFORM_TYPE_VEC4, 4);   /* 16 point lanes */
    /* Shadow filter tier — unconditional (gates local shadows even when
       the CSM resource block above was skipped). Default tier matches the
       full-quality shaders until the first frame caches the pipeline knob. */
    sr->u_shadow_quality = bgfx_create_uniform("u_shadowQuality",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->shadow_filter_tier = 2.0f;

    LOG_INFO(LOG_TAG, "[init] light env");
    /* Multi-light env. */
    sr->light_env = jce_light_env_create();

    /* Forward+ clustered lighting (ROUND A — dormant by default).  Desktop
     * froxel grid 24x16x24 (≈9216 cells); max_lights 256 lifts the brute-
     * force 16-light cap the cluster path will eventually replace.  Created
     * disabled so it builds nothing and uploads nothing until explicitly
     * toggled on — guaranteeing byte-identical rendering this round. */
    sr->fp_max_lights = 256u;
    sr->forwardplus = jce_forwardplus_create(24u, 16u, 24u, 64u,
                                             sr->fp_max_lights);
    sr->fp_proxies = (JceLightProxy *)JCE_CALLOC(
        sr->fp_max_lights, sizeof(JceLightProxy));
    sr->fp_params = (JceForwardPlusLightParam *)JCE_CALLOC(
        sr->fp_max_lights, sizeof(JceForwardPlusLightParam));

    /* r.forwardplus console cvar — OPT-IN clustered lighting (default OFF).
     * Read each frame so the console toggles it live.  Registering a cvar
     * that already exists returns the existing handle (idempotent across
     * multiple scene renderers / engine restarts within a process). */
    sr->cv_forwardplus = jce_cvar_register_bool(
        "r.forwardplus", false, JCE_CVAR_FLAG_NONE,
        "Forward+ clustered lighting (opt-in; lifts the 16-point-light cap, "
        "drops per-spot IES). Default off = brute-force path.");

    /* r.taa console cvar — temporal anti-aliasing, ON by default.  Read each
     * frame by the caller's TAA begin/end driver.  r.taa 0 falls back to the
     * FXAA path (byte-identical to the pre-TAA chain).  Idempotent across
     * renderers.  Per-object + per-bone motion vectors (skinned palette) + a
     * per-viewport previous camera give ghost-free reprojection.
     *
     * The velocity pre-pass that feeds TAA is now frustum-culled (see
     * sr_try_submit_model_velocity / prepass_cull_planes), so its per-frame
     * uniform writes stay bounded — this fixes the bgfx Vulkan uniform
     * scratch-buffer overflow that used to crash large scenes (hundreds+ of
     * entities) when TAA was on.  Verified crash-free under ASAN at 1500
     * entities. */
    sr->cv_taa = jce_cvar_register_bool(
        "r.taa", true, JCE_CVAR_FLAG_NONE,
        "Temporal anti-aliasing (default on; r.taa 0 -> FXAA).  Per-object + "
        "per-bone motion vectors (skinned palette) + a per-viewport previous "
        "camera give ghost-free reprojection.  The velocity pre-pass is "
        "frustum-culled so it scales to large scenes.");
    memset(&sr->taa_state, 0, sizeof(sr->taa_state));

    /* r.gpu_driven console cvar — GPU-driven color-pass instancing (roadmap
     * #18, Phase 0+1).  Default OFF: the existing CPU instancing path runs
     * byte-identical.  When ON (this cvar OR the per-frame config field) AND
     * the GPU exposes compute AND the cull program loaded, the color-pass
     * instance batch is routed through a persistent GPUScene buffer + a compute
     * frustum cull.  Idempotent across renderers. */
    sr->cv_gpu_driven = jce_cvar_register_bool(
        "r.gpu_driven", false, JCE_CVAR_FLAG_NONE,
        "GPU-driven color-pass instancing (persistent GPUScene buffer + compute "
        "frustum cull, roadmap #18).  Default off = CPU instancing path "
        "(byte-identical).  On engages the GPU cull for opaque instanced meshes; "
        "shadow/prepass stay on the CPU.");
    /* GPUScene helper: loads cs_cull_frustum from the engine shader pak (with
     * the embedded-engine-pak fallback).  Returns a no-op handle on devices
     * without compute — is_supported() then gates the GPU path off. */
    sr->gpu_scene = jce_gpu_scene_create(sr->pak, jce_allocator_default());
    sr->gpu_driven_frame = false;

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

    /* Baked GI: SH9 ambient (9 vec4 RGB coeffs) + GI params. */
    sr->u_sh9       = bgfx_create_uniform("u_sh9", BGFX_UNIFORM_TYPE_VEC4, 9);
    sr->u_gi_params = bgfx_create_uniform("u_giParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_ssao_params = bgfx_create_uniform("u_ssaoParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_gbuffer_mat = bgfx_create_uniform("u_gbufferMat", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* TAA motion-vector pass uniforms (mat4s): prev/cur un-jittered view*proj +
     * the static prev-frame world matrix and the previous skinned bone palette.
     * Created up front so submit-time always has valid handles even when TAA
     * is off (then they're simply never set). */
    sr->u_prevModel    = bgfx_create_uniform("u_prevModel",    BGFX_UNIFORM_TYPE_MAT4, 1);
    sr->u_prevViewProj = bgfx_create_uniform("u_prevViewProj", BGFX_UNIFORM_TYPE_MAT4, 1);
    sr->u_curViewProj  = bgfx_create_uniform("u_curViewProj",  BGFX_UNIFORM_TYPE_MAT4, 1);
    sr->u_prevBones    = bgfx_create_uniform("u_prevBones",    BGFX_UNIFORM_TYPE_MAT4, JCE_MAX_BONES);

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
    /* Transparent queue: smaller (transparency is the minority) and never
     * auto-instances so its back-to-front order is preserved exactly. */
    sr->transparent_queue = jce_rq_create(1024);
    if (sr->transparent_queue)
        jce_rq_set_no_batch(sr->transparent_queue, true);
    sr->mat_count = 0;
    sr->frame_view_id = 0;
    sr->frame_shadow_vp_valid = false; sr->frame_shadow_active = false;
    return sr;
}

/* Public accessors so editors/tools can read the live animation state
 * without keeping a second cache of their own. */
struct JceAnimPlayer *jce_scene_renderer_get_anim_player(
    JceSceneRenderer *sr, const char *skeleton_path)
{
    if (!sr || !skeleton_path || !skeleton_path[0]) return NULL;
    /* Players are per-entity now; return the first live instance using this
       path's model (enough for the editor's single progress-bar/timeline view). */
    JceModel *model = NULL;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, skeleton_path) == 0) { model = e->model; break; }
    }
    if (!model) return NULL;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->model == model && a->player)
            return (struct JceAnimPlayer *)a->player;
    }
    return NULL;
}

struct JceAnimSmBinding *jce_scene_renderer_get_anim_sm(
    JceSceneRenderer *sr, uint32_t entity)
{
    if (!sr) return NULL;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->entity == entity)
            return a->sm_binding;
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

    /* Per-entity animation instances (players reference, but don't own, the
       shared skeletons in model_cache — destroy them before the models). */
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->player) jce_anim_player_destroy(a->player);
        if (a->sm_binding) { jce_anim_sm_binding_destroy(a->sm_binding); a->sm_binding = NULL; }
        if (a->blend_tree) { jce_anim_blend_tree_destroy(a->blend_tree); a->blend_tree = NULL; }
        if (a->ev_pool)    { JCE_FREE(a->ev_pool); a->ev_pool = NULL; }
        if (a->avatar_mask) { jce_avatar_mask_unload(a->avatar_mask); a->avatar_mask = NULL; }
        for (int li = 0; li < SR_AVATAR_MAX_LAYERS; li++)
            if (a->layer_mask[li]) { jce_avatar_mask_unload(a->layer_mask[li]); a->layer_mask[li] = NULL; }
        /* Retarget map borrows skeletons in model_cache — free before the
           models (mirrors the player teardown ordering above). */
        if (a->retarget_map) { jce_anim_retarget_map_destroy(a->retarget_map); a->retarget_map = NULL; }
        /* Morph VBs are owned by the instance — destroy before the shared
           model cache is torn down (handle-leak guard, 3rd lifecycle site). */
        sr_free_morph_vbs(a);
        a->used = false;
    }

    /* Vegetation scatter caches (own their scattered-instance buffers). */
    for (int i = 0; i < (int)(sizeof sr->foliage_cache / sizeof sr->foliage_cache[0]); i++) {
        JCE_FREE(sr->foliage_cache[i].insts);
        sr->foliage_cache[i].insts = NULL;
    }

    /* Per-frame per-entity cull cache. */
    JCE_FREE(sr->ecull);        sr->ecull = NULL;

    /* Color-pass GPU-instancing batch buffers. */
    JCE_FREE(sr->inst_batch);   sr->inst_batch = NULL;   sr->inst_batch_cap = 0;
    JCE_FREE(sr->sh_batch);     sr->sh_batch = NULL;     sr->sh_batch_cap = 0;
    JCE_FREE(sr->inst_gather);  sr->inst_gather = NULL;  sr->inst_gather_cap = 0;

    /* GPU-driven rendering (roadmap #18): destroy the GPUScene helper (owns its
     * compute program + persistent GPU buffers) + the per-flush record scratch. */
    if (sr->gpu_scene) { jce_gpu_scene_destroy(sr->gpu_scene); sr->gpu_scene = NULL; }
    JCE_FREE(sr->gpu_rec);      sr->gpu_rec = NULL;      sr->gpu_rec_cap = 0;
    JCE_FREE(sr->gpu_draw);     sr->gpu_draw = NULL;     sr->gpu_draw_cap = 0;

    /* 2D sprite-animator instances (own their sheet + player). */
    for (int i = 0; i < SR_SPRITE_ANIM_MAX; i++) {
        SrSpriteAnim *s = &sr->sprite_anim[i];
        if (s->player) { jce_sprite_player_destroy(s->player); s->player = NULL; }
        if (s->sheet)  { jce_sprite_sheet_destroy(s->sheet);   s->sheet  = NULL; }
        s->used = false;
    }

    /* Model cache.  Join any in-flight async decode first and drop its CPU
     * result (no GPU upload at teardown). */
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->pending) {
            if (e->thr) { jce_thread_join(e->thr); e->thr = NULL; }
            if (e->job) {
                jce_model_gltf_cpu_free(e->job->cpu);
                if (e->job->done) jce_atomic_i32_destroy(e->job->done);
                JCE_FREE(e->job);
                e->job = NULL;
            }
            e->pending = false;
        }
        if (!e->used) continue;
        if (e->model)  jce_model_destroy(e->model);
        e->used = false;
    }
    sr->model_inflight = 0;

    /* Async texture decodes still in flight: join the workers and drop
     * their decoded results (no GPU upload at teardown). */
    for (int i = 0; i < sr->tex_cache_count; i++) {
        if (!sr->tex_cache[i].pending) continue;
        if (sr->tex_cache[i].thr) {
            jce_thread_join(sr->tex_cache[i].thr);
            sr->tex_cache[i].thr = NULL;
        }
        struct SrTexJob *j = sr->tex_cache[i].job;
        if (j) {
            jce_texture_cpu_free(j->result);
            if (j->done) jce_atomic_i32_destroy(j->done);
            JCE_FREE(j);
            sr->tex_cache[i].job = NULL;
        }
        sr->tex_cache[i].pending = false;
    }
    sr->tex_inflight = 0;

    /* Shader Graph custom-program cache: lookup-only.  The programs are
     * owned by the process-wide cache in jce_pbr_material.c (freed by
     * jce_pbr_material_shutdown during renderer teardown), so we only drop
     * our references here — no destroy, to avoid a double-free. */
    sr->prog_cache_count = 0;

    if (sr->cube_mesh)     jce_mesh_destroy(sr->cube_mesh);
    if (sr->plane_mesh)    jce_mesh_destroy(sr->plane_mesh);
    if (sr->sphere_mesh)   jce_mesh_destroy(sr->sphere_mesh);
    if (sr->capsule_mesh)  jce_mesh_destroy(sr->capsule_mesh);
    if (sr->cylinder_mesh) jce_mesh_destroy(sr->cylinder_mesh);

    /* Terrain cache. */
    for (int i = 0; i < 16; i++) {
        if (!sr->terrain_cache[i].used) continue;
        sr_terrain_free_chunks(sr, i);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        sr->terrain_cache[i].used = false;
    }
    /* Tilemap cache (chunk VBs + CPU assets) + shared IB/sampler. */
    for (int i = 0; i < SR_TILEMAP_SLOT_MAX; i++)
        sr_tilemap_free_slot(sr, i);
    if (BGFX_HANDLE_IS_VALID(sr->tilemap_shared_ib))
        bgfx_destroy_index_buffer(sr->tilemap_shared_ib);
    if (BGFX_HANDLE_IS_VALID(sr->tilemap_s_tex))
        bgfx_destroy_uniform(sr->tilemap_s_tex);

    /* Water cache (grid meshes) + program + uniforms. */
    for (int i = 0; i < SR_WATER_SLOT_MAX; i++)
        sr_water_slot_free(sr, i);   /* grid mesh + FFT core + FFT texture */
    if (BGFX_HANDLE_IS_VALID(sr->prog_water))           bgfx_destroy_program(sr->prog_water);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_wave_a))        bgfx_destroy_uniform(sr->u_water_wave_a);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_wave_b))        bgfx_destroy_uniform(sr->u_water_wave_b);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_params))        bgfx_destroy_uniform(sr->u_water_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_time))          bgfx_destroy_uniform(sr->u_water_time);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_color_shallow)) bgfx_destroy_uniform(sr->u_water_color_shallow);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_color_deep))    bgfx_destroy_uniform(sr->u_water_color_deep);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_shading))       bgfx_destroy_uniform(sr->u_water_shading);
    if (BGFX_HANDLE_IS_VALID(sr->u_water_mode))          bgfx_destroy_uniform(sr->u_water_mode);
    if (BGFX_HANDLE_IS_VALID(sr->s_water_disp))          bgfx_destroy_uniform(sr->s_water_disp);

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
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_perez))    bgfx_destroy_uniform(sr->u_sky_perez);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_zenith))   bgfx_destroy_uniform(sr->u_sky_zenith);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_sun_dir))  bgfx_destroy_uniform(sr->u_sky_sun_dir);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_dir))    bgfx_destroy_uniform(sr->u_light_dir);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_color))  bgfx_destroy_uniform(sr->u_light_color);

    sr_destroy_shadow_targets(sr);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowMap)) bgfx_destroy_uniform(sr->u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowVP))  bgfx_destroy_uniform(sr->u_shadowVP);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_map))    bgfx_destroy_uniform(sr->u_local_shadow_map);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_vp))     bgfx_destroy_uniform(sr->u_local_shadow_vp);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_params)) bgfx_destroy_uniform(sr->u_local_shadow_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_bias))   bgfx_destroy_uniform(sr->u_local_shadow_bias);
    if (BGFX_HANDLE_IS_VALID(sr->u_spot_shadow_slot))    bgfx_destroy_uniform(sr->u_spot_shadow_slot);
    if (BGFX_HANDLE_IS_VALID(sr->u_point_shadow_slot))   bgfx_destroy_uniform(sr->u_point_shadow_slot);

    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_samplers[i]))
            bgfx_destroy_uniform(sr->u_csm_samplers[i]);
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_vp))         bgfx_destroy_uniform(sr->u_csm_vp);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_splits))     bgfx_destroy_uniform(sr->u_csm_splits);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_params))     bgfx_destroy_uniform(sr->u_csm_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_bias_scales))bgfx_destroy_uniform(sr->u_csm_bias_scales);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadow_quality)) bgfx_destroy_uniform(sr->u_shadow_quality);

    if (sr->light_env) jce_light_env_destroy(sr->light_env);
    if (sr->forwardplus) jce_forwardplus_destroy(sr->forwardplus);
    JCE_FREE(sr->fp_proxies);
    JCE_FREE(sr->fp_params);
    /* Join any in-flight async IBL bake before tearing down (the worker uses a
     * private pixel copy, so the skybox below is safe to destroy regardless). */
    if (sr->ibl_thread) { jce_thread_join(sr->ibl_thread); sr->ibl_thread = NULL; }
    if (sr->ibl_job) {
        if (sr->ibl_job->result) jce_ibl_cpu_free(sr->ibl_job->result);
        JCE_FREE(sr->ibl_job);
        sr->ibl_job = NULL;
    }
    if (sr->ibl_data)  jce_ibl_destroy(sr->ibl_data);
    if (sr->skybox)    jce_skybox_destroy(sr->skybox);
    if (sr->vfog)      jce_volumetric_fog_destroy(sr->vfog);
    if (sr->ssao)      jce_ssao_destroy(sr->ssao);
    if (sr->ssr)       jce_ssr_destroy(sr->ssr);
    if (BGFX_HANDLE_IS_VALID(sr->prog_gbuffer)) bgfx_destroy_program(sr->prog_gbuffer);
    if (BGFX_HANDLE_IS_VALID(sr->prog_gbuffer_vel)) bgfx_destroy_program(sr->prog_gbuffer_vel);
    if (BGFX_HANDLE_IS_VALID(sr->prog_gbuffer_vel_skinned)) bgfx_destroy_program(sr->prog_gbuffer_vel_skinned);
    if (BGFX_HANDLE_IS_VALID(sr->u_prevModel))    bgfx_destroy_uniform(sr->u_prevModel);
    if (BGFX_HANDLE_IS_VALID(sr->u_prevViewProj)) bgfx_destroy_uniform(sr->u_prevViewProj);
    if (BGFX_HANDLE_IS_VALID(sr->u_curViewProj))  bgfx_destroy_uniform(sr->u_curViewProj);
    if (BGFX_HANDLE_IS_VALID(sr->u_prevBones))    bgfx_destroy_uniform(sr->u_prevBones);
    if (BGFX_HANDLE_IS_VALID(sr->ssao_depth_fbo)) bgfx_destroy_frame_buffer(sr->ssao_depth_fbo);
    if (BGFX_HANDLE_IS_VALID(sr->ssao_depth_tex)) bgfx_destroy_texture(sr->ssao_depth_tex);
    if (BGFX_HANDLE_IS_VALID(sr->ssao_normal_tex)) bgfx_destroy_texture(sr->ssao_normal_tex);
    if (BGFX_HANDLE_IS_VALID(sr->ssao_velocity_tex)) bgfx_destroy_texture(sr->ssao_velocity_tex);

    /* Weather / decals (P2-weather-decals-tod). */
    if (sr->weather)         jce_weather_destroy(sr->weather);
    if (sr->decals)          jce_decals_destroy(sr->decals);
    if (sr->decals_authored) jce_decals_destroy(sr->decals_authored);

    /* GPU particle pools (one per GPU-flagged emitter). */
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++) {
        if (sr->gpu_particles[i].sys)
            jce_gpu_particles_destroy(sr->gpu_particles[i].sys);
        sr->gpu_particles[i].sys  = NULL;
        sr->gpu_particles[i].used = false;
    }

    if (BGFX_HANDLE_IS_VALID(sr->brdf_lut))         bgfx_destroy_texture(sr->brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_irradiance)) bgfx_destroy_uniform(sr->u_ibl_irradiance);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_prefilter))  bgfx_destroy_uniform(sr->u_ibl_prefilter);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_brdf_lut))   bgfx_destroy_uniform(sr->u_ibl_brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_params))     bgfx_destroy_uniform(sr->u_ibl_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_sh9))            bgfx_destroy_uniform(sr->u_sh9);
    if (BGFX_HANDLE_IS_VALID(sr->u_gi_params))      bgfx_destroy_uniform(sr->u_gi_params);

    /* Release baked reflection-probe cubemaps loaded this session. */
    for (int i = 0; i < sr->rprobe_cache_count; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->rprobe_cache[i].spec))
            bgfx_destroy_texture(sr->rprobe_cache[i].spec);
        if (BGFX_HANDLE_IS_VALID(sr->rprobe_cache[i].irr))
            bgfx_destroy_texture(sr->rprobe_cache[i].irr);
    }

    if (sr->sprite_batch)    jce_sprite_batch_destroy(sr->sprite_batch);
    if (sr->postfx_pipeline) jce_postfx_destroy(sr->postfx_pipeline);

    if (sr->cull_space) jce_space_destroy(sr->cull_space);
    if (sr->cull_aabbs) JCE_FREE(sr->cull_aabbs);
    if (sr->cull_prep)  JCE_FREE(sr->cull_prep);
    if (sr->shadow_space) jce_space_destroy(sr->shadow_space);
    if (sr->shadow_noaabb) JCE_FREE(sr->shadow_noaabb);
    if (sr->shadow_query)  JCE_FREE(sr->shadow_query);
    if (sr->render_queue) jce_rq_destroy(sr->render_queue);
    if (sr->transparent_queue) jce_rq_destroy(sr->transparent_queue);

    JCE_FREE(sr);
}


uint16_t jce_scene_renderer_render(JceSceneRenderer *sr, JceScene *scene,
                                   const JceCamera *camera,
                                   uint16_t view_id_base, float dt_sec,
                                   const JceSceneRenderConfig *config)
{
    if (!sr || !scene) return view_id_base;
    JCE_PROFILE_ZONE_N("SceneRenderer::Render");

    /* Upload any textures / models whose async decode finished last frame. */
    sr_tex_poll(sr);
    sr_model_poll(sr);

    /* Start a fresh world-matrix cache generation for this render. The
       multiple sub-passes below (shadow producers + color + terrain) each
       read jce_scene_get_world_matrix; memoizing per render means a parent
       shared by K drawables is composed once instead of K times. Bumping
       here (not only in jce_scene_update) keeps edit-mode renders — which
       never call jce_scene_update — reading transforms as edited this frame
       rather than a stale cached pose. */
    jce_scene_invalidate_world_cache(scene);

    /* Label views for GPU profilers (RenderDoc / bgfx debug overlay).
     * bgfx accepts repeat sets; names persist for the lifetime of the
     * view ID, so this is effectively cheap. */
    bgfx_set_view_name(view_id_base,                           "Scene/Color",        INT32_MAX);
    bgfx_set_view_name((uint16_t)(view_id_base + JCE_VIEW_GPU_PARTICLE_OFFSET),
                                                               "Scene/ParticleCS",   INT32_MAX);
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

    /* ── GPU-driven rendering gate (roadmap #18) ──────────────────────────
     * Latch the effective gpu_driven for THIS frame from the config field OR
     * the r.gpu_driven cvar, AND the GPUScene helper actually supporting it
     * (compute + cull program).  Read once here so the divergence in
     * sr_inst_flush and the view-order builder agree.  When false, the GPU
     * module is never touched and the CPU instancing path is byte-identical. */
    {
        bool want = cfg->gpu_driven;
        if (sr->cv_gpu_driven && jce_cvar_get_bool(sr->cv_gpu_driven)) want = true;
        bool supported = want && jce_gpu_scene_is_supported(sr->gpu_scene);
        /* Per-bgfx-frame guard (mirrors the GPU-particle path): the editor
         * renders >1 viewport per bgfx frame on a SHARED renderer/gpu_scene.
         * All bgfx_update_dynamic_* land in m_cmdPre (applied before any view),
         * so a 2nd render would clobber the 1st's GPUScene buffers — and on an
         * ensure_capacity growth, destroy buffers the 1st's already-queued draws
         * still reference (use-after-destroy -> render-thread AV). So only the
         * FIRST render of each bgfx frame takes the GPU path; later viewports
         * fall back to the byte-identical CPU instancing path. The single-
         * viewport runtime is unaffected (always "first"). */
        uint32_t gpu_fi = jce_renderer_get_frame_index(sr->renderer);
        bool gpu_first = !(sr->gpu_scene_frame_valid && sr->gpu_scene_frame == gpu_fi);
        sr->gpu_driven_frame = supported && gpu_first;
        if (sr->gpu_driven_frame) {
            sr->gpu_scene_frame = gpu_fi;
            sr->gpu_scene_frame_valid = true;
        }
        sr->gpu_cull_view = (uint16_t)(view_id_base + JCE_VIEW_GPU_PARTICLE_OFFSET);
    }

    sr->frame_shadow_active = false; sr->frame_shadow_vp_valid = false;
    sr->shadow_use_csm = false; sr->last_csm_valid = false;

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

    sr_apply_view_order(view_id_base, cfg, sr->csm_cascade_count,
                        sr->gpu_driven_frame);

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
            /* Data-driven custom post pass (engine stays style-agnostic). */
            jce_postfx_set_custom_shader(sr->postfx_pipeline,
                                         scene_rendering->custom_post_shader,
                                         scene_rendering->custom_post_needs_depth);
            jce_postfx_set_custom_params(sr->postfx_pipeline,
                                         scene_rendering->custom_post_params,
                                         scene_rendering->custom_post_param_count);
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

    /* Shadow filter tier — read the render-pipeline quality knob once per
       frame; sr_bind_frame_shadow_state uploads it with every material
       bind. (jce_render_pipeline_get is a struct copy, not a lookup.) */
    {
        JceRenderPipelineDesc rp_desc;
        jce_render_pipeline_get(&rp_desc);
        sr->shadow_filter_tier = (float)rp_desc.shadow_filter_quality;
    }

    /* Time-of-day driver (P2-weather-decals-tod): advance the day clock and
     * push the lighting snapshot into sr->tod_state BEFORE the sky + lighting
     * passes consume it.  No-op (and releases any override) when disabled. */
    if (scene_rendering)
        sr_drive_time_of_day(sr, scene_rendering, dt_sec);

    /* Capture the authored sky mode + turbidity for the sky pass.  Default
     * (no settings) → gradient, so the legacy sky path is byte-identical. */
    if (scene_rendering) {
        sr->sky_mode      = scene_rendering->sky_mode;
        sr->sky_turbidity = scene_rendering->sky_turbidity;
    } else {
        sr->sky_mode      = JCE_SCENE_SKY_GRADIENT;
        sr->sky_turbidity = 2.5f;
    }

    /* Collect entities.  STATIC (BSS, not stack): the 32768-entry list is 256KB,
     * which must not live on the stack; rendering is single-threaded/sequential
     * (scene + game viewports render one after another, never concurrently), so
     * a function-static is safe and reused across frames. */
    static EntityList list;
    list.count = 0;
    /* Focus-bounded collect ("draw distance"): when enabled with a positive
     * radius, only entities within (radius + ~70m margin) HORIZONTAL of the
     * focus point are collected, so every downstream pass becomes O(near).
     * Default (cfg memset/zero) → enabled=false → collect all (no regression). */
    SrCollectCtx collect_ctx = {
        .list    = &list,
        .scene   = scene,
        .fx      = cfg->cull_focus_x,
        .fz      = cfg->cull_focus_z,
        .r2      = 0.0f,
        .enabled = false,
    };
    if (cfg->cull_focus_enabled && cfg->cull_radius > 0.0f) {
        float eff = cfg->cull_radius + 70.0f; /* footprint margin */
        collect_ctx.r2      = eff * eff;
        collect_ctx.enabled = true;
    }
    jce_scene_each_entity(scene, collect_entity_cb, &collect_ctx);

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

    /* Skinned animation: advance every skeletal pose ONCE here, before the
       shadow pass records draws, caching each world-bone palette so the
       shadow and color passes share the identical pose (no double-advance). */
    sr_update_skinned_anims(sr, scene, &list, dt_sec);

    /* 2D sprite animation: advance every SpriteAnimator frame time ONCE here;
       the entity draw loop consumes the cached player's current frame. */
    sr_update_sprite_anims(sr, scene, &list, dt_sec);

    /* Distance/importance light selection (Unity-style): choose the most
       important point/spot lights for this view BEFORE the shadow producer and
       the light gather, so both consume the SAME selected set (index-aligned). */
    sr_select_lights(sr, scene, &list, camera);

    /* Build the per-frame per-entity cull cache ONCE here (after the collect,
     * before the shadow pass).  Every shadow caster loop + the depth/velocity
     * prepass cull would otherwise recompute the SAME world AABB + model resolve
     * (sr_shadow_caster_aabb) ~6×/frame; caching it once and reading sr->ecull[i]
     * at each cull site removes that redundancy (the measured full-load hot path).
     * Indexed identically to list.entities[i].  list.count <= SR_MAX_ENTITIES
     * (collect_entity_cb clamps), so ecull (sized SR_MAX_ENTITIES) never overruns. */
    for (int eci = 0; eci < list.count; eci++) {
        JceEntity e = list.entities[eci];
        jce_vec3 mn, mx;
        /* #8 — compose the world matrix ONCE here (identical source to every
         * later jce_scene_get_world_matrix call) and cache it.  The shadow
         * cascades / depth prepass / color submit read sr->ecull[eci].world
         * instead of re-walking the parent chain ~5×/entity/frame. */
        const jce_mat4 *wptr = NULL;
        if (jce_scene_has_transform(scene, e)) {
            sr->ecull[eci].world       = jce_scene_get_world_matrix(scene, e);
            sr->ecull[eci].world_valid = true;
            wptr = &sr->ecull[eci].world;
        } else {
            sr->ecull[eci].world_valid = false;
        }
        sr->ecull[eci].has_aabb     = sr_shadow_caster_aabb(sr, scene, e, wptr, &mn, &mx);
        sr->ecull[eci].wmin         = mn;
        sr->ecull[eci].wmax         = mx;
        sr->ecull[eci].casts_shadow = sr_entity_casts_shadow(scene, e);
    }

    /* Shadow passes (do them BEFORE entity pass so PBR can sample). */
    if (cfg->draw_shadows) {
        /* Determine viewport from camera bounds — if absent, assume 16:9. */
        uint32_t vp_w = cfg->viewport_width ? cfg->viewport_width : 1920, vp_h = cfg->viewport_height ? cfg->viewport_height : 1080;
        float shadow_distance = cfg->shadow_distance;
        float split_lambda = cfg->csm_split_lambda;
        if (shadow_distance <= 0.0f && scene_rendering)
            shadow_distance = scene_rendering->shadow_distance;
        if (split_lambda < 0.0f)
            split_lambda = scene_rendering ? scene_rendering->split_lambda : 0.5f;
        sr_draw_shadow_pass(sr, scene, camera, &list, view_id_base,
                            vp_w, vp_h, shadow_distance, split_lambda);
        /* P1 — local (spot) shadow atlas producer, after the directional/CSM
           pass and before the entity/color pass that samples it. */
        sr_draw_local_shadow_pass(sr, scene, &list, view_id_base);
    } else {
        sr->shadow_use_csm = false; sr->last_csm_valid = false;
        sr->frame_local_active = false;
    }

    /* SSAO: camera depth pre-pass + screen-space AO (gated on the scene's
     * authored ssao_enabled + the pipeline feature flag; OFF => byte-identical).
     * Runs after shadows, before the entity/color pass that samples it.  The
     * result is bound into the PBR AO sampler stage in the material build, and
     * u_ssaoParams (set before sr_draw_entities) switches the shader to
     * screen-UV AO sampling. */
    sr->ssao_active_frame = false;
    sr->ssr_active_frame  = false;
    sr->velocity_valid_frame = false;
    {
        bool want_ssao = scene_rendering && scene_rendering->ssao_enabled &&
                         jce_render_pipeline_is_feature_enabled("ssao");
        bool want_ssr  = scene_rendering && scene_rendering->ssr_enabled &&
                         jce_render_pipeline_is_feature_enabled("ssr") &&
                         cfg->ssr_color_tex_handle != UINT16_MAX;
        /* TAA per-object motion: the caller requested a velocity buffer this
         * frame (jce_scene_renderer_set_taa_velocity_enabled).  Extend the
         * pre-pass gate so the depth/G-buffer pass runs (producing velocity)
         * even when SSAO/SSR are both off. */
        bool want_velocity = sr->taa_want_velocity;
        /* SSAO (base+2/+3) and SSR (base+18 ray-march, base+19 composite) now
         * use disjoint view slots, so they coexist in the same frame.  Both
         * share the base+1 depth/normal G-buffer pre-pass.  For the scene view
         * the editor relocates postfx above base+19 so base+18/+19 stay free;
         * the game view already has them free between its UI (base+17) and
         * postfx (base+20). */

        if ((want_ssao || want_ssr || want_velocity) && camera && sr->pak) {
            uint32_t sw = cfg->viewport_width  ? cfg->viewport_width  : 1920;
            uint32_t sh = cfg->viewport_height ? cfg->viewport_height : 1080;
            float    aspect = (sw && sh) ? ((float)sw / (float)sh) : (16.0f / 9.0f);
            sr_ensure_ssao_target(sr, (uint16_t)sw, (uint16_t)sh, want_velocity);
            if (sr->ssao_valid) {
                /* TAA velocity: this render's un-jittered view*proj for the
                 * velocity shaders, plus THIS VIEWPORT's previous-frame view*proj
                 * (prev) for the reprojection.  Computed from the CLEAN camera
                 * (no jitter), matching the prepass's jce_camera_view/proj.
                 * prev is read from this viewport's own slot (so the other
                 * viewport's camera can't clobber it -> correct per-view motion,
                 * order-independent), THEN the slot is rolled to this frame's VP
                 * for next frame. */
                if (want_velocity) {
                    int vp_slot = cfg->viewport_id;
                    if (vp_slot < 0 || vp_slot >= JCE_SR_VIEWPORT_SLOTS) vp_slot = 0;
                    jce_mat4 cvw = jce_camera_view(camera);
                    jce_mat4 cpj = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
                    jce_mat4 cvp = jce_m4_multiply(&cpj, &cvw);
                    memcpy(sr->cur_view_proj, cvp.raw[0], sizeof(float) * 16);
                    /* prev = this slot's last frame VP (first frame: prev=cur). */
                    memcpy(sr->frame_prev_vp,
                           sr->prev_view_proj_valid[vp_slot]
                               ? sr->prev_view_proj_cache[vp_slot]
                               : cvp.raw[0],
                           sizeof(float) * 16);
                    /* Roll this slot for next frame. */
                    memcpy(sr->prev_view_proj_cache[vp_slot], cvp.raw[0],
                           sizeof(float) * 16);
                    sr->prev_view_proj_valid[vp_slot] = true;
                }
                /* Shared camera depth pre-pass (base+1).  Renders after the
                 * color pass (base+0) in id order: SSAO is 1-frame-late (color
                 * samples last frame's AO); SSR reads THIS frame's lit color
                 * (it samples the color RT that base+0 already wrote). */
                sr_draw_depth_prepass(sr, scene, camera, &list, view_id_base, sw, sh);
                if (want_velocity) {
                    /* Bind the velocity buffer into the shared scene postfx so
                     * its TAA resolve reprojects with per-object motion (the
                     * scene view path uses sr->postfx_pipeline directly; the
                     * game view path binds it onto its own pipeline via
                     * jce_scene_renderer_get_velocity_texture()). */
                    if (sr->postfx_pipeline && sr->velocity_valid_frame) {
                        JceTextureHandle vt = { sr->ssao_velocity_tex.idx };
                        jce_postfx_set_taa_motion_tex(sr->postfx_pipeline, vt);
                    }
                }

                if (want_ssao) {
                    if (!sr->ssao) {
                        JceSsaoDesc sd; memset(&sd, 0, sizeof sd);
                        sd.pak = sr->pak; sd.width = (int)sw; sd.height = (int)sh;
                        sr->ssao = jce_ssao_create(&sd);
                    } else {
                        jce_ssao_resize(sr->ssao, (int)sw, (int)sh);
                    }
                    if (sr->ssao) {
                        JceSsaoParams sp = jce_ssao_default_params();
                        if (scene_rendering->ssao_radius    > 0.0f) sp.radius    = scene_rendering->ssao_radius;
                        if (scene_rendering->ssao_intensity > 0.0f) sp.intensity = scene_rendering->ssao_intensity;
                        sp.near_plane = jce_camera_get_near(camera);
                        sp.far_plane  = jce_camera_get_far(camera);
                        jce_ssao_set_params(sr->ssao, &sp);
                        jce_ssao_render(sr->ssao, sr->ssao_depth_tex.idx,
                                        (uint16_t)(view_id_base + 2));
                        sr->ssao_ao_idx = jce_ssao_get_result_texture(sr->ssao);
                        sr->ssao_active_frame = (sr->ssao_ao_idx != UINT16_MAX);
                    }
                }

                if (want_ssr) {
                    if (!sr->ssr) {
                        JceSsrDesc rd; memset(&rd, 0, sizeof rd);
                        rd.pak = sr->pak; rd.width = (int)sw; rd.height = (int)sh;
                        sr->ssr = jce_ssr_create(&rd);
                    } else {
                        jce_ssr_resize(sr->ssr, (int)sw, (int)sh);
                    }
                    if (sr->ssr) {
                        JceSsrParams rp = jce_ssr_default_params();
                        if (scene_rendering->ssr_intensity    > 0.0f) rp.intensity    = scene_rendering->ssr_intensity;
                        if (scene_rendering->ssr_max_distance > 0.0f) rp.max_distance = scene_rendering->ssr_max_distance;
                        rp.near_plane = jce_camera_get_near(camera);
                        rp.far_plane  = jce_camera_get_far(camera);
                        jce_ssr_set_params(sr->ssr, &rp);
                        jce_mat4 vmat = jce_camera_view(camera);
                        jce_mat4 pmat = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
                        /* SSR ray-march at base+2 (renders after color base+0 =>
                         * reads this frame's lit color).  Normal is reconstructed
                         * from depth in fs_ssr, so the depth tex is passed as the
                         * normal input too. */
                        jce_ssr_render(sr->ssr, cfg->ssr_color_tex_handle,
                                       sr->ssao_depth_tex.idx, sr->ssao_normal_tex.idx,
                                       &vmat, &pmat, (uint16_t)(view_id_base + 18));
                        sr->ssr_result_idx = jce_ssr_get_result_texture(sr->ssr);
                        sr->ssr_active_frame = (sr->ssr_result_idx != UINT16_MAX);
                    }
                }
            }
        }
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

    /* SSAO uniform for the PBR pass: x = enabled, yz = 1/screen (screen UV).
     * Set once before the entity pass (bgfx retains uniform values across
     * submits); 0 when inactive => shader uses the material-AO path. */
    {
        float ssaoP[4] = {
            sr->ssao_active_frame ? 1.0f : 0.0f,
            (sr->ssao_active_frame && sr->ssao_w) ? 1.0f / (float)sr->ssao_w : 0.0f,
            (sr->ssao_active_frame && sr->ssao_h) ? 1.0f / (float)sr->ssao_h : 0.0f,
            0.0f
        };
        bgfx_set_uniform(sr->u_ssao_params, ssaoP, 1);
    }

    /* Entity rendering. */
    sr_draw_entities(sr, scene, camera, &list, view_id_base, dt_sec, cfg);

    /* Cloth/soft-body grids (after entities so lighting uniforms are live). */
    sr_draw_cloth(sr, scene, view_id_base);

    /* Reset wireframe so subsequent overlay passes (grid, selection) draw
     * solid. Mirrors 0.5.7 line 914 exactly. bgfx_set_debug() takes effect
     * per-submission, so submits between set(true) and set(false) draw as
     * wireframe; submits after set(false) draw solid. */
    if (editor_wf_set)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* Particle emitters: draw alive particles from the scene-owned
     * JceParticleSystem (simulated each runtime step by
     * jce_scene_particles_update) as depth-tested debug billboards. */
    sr_draw_particles(sr, scene, view_id_base);

    /* GPU-flagged emitters: compute simulate/emit on base+9 (ordered before
     * the color view) + one instanced billboard draw per pool, at the same
     * transparency position as the CPU particle pass above. */
    sr_drive_gpu_particles(sr, scene, view_id_base, dt_sec);

    /* Decals (P2-weather-decals-tod): depth-tested projected quads from
     * authored JceDecalComponent projectors + runtime-stamped decals.  Drawn
     * after opaque geometry so they composite over the surfaces they hug. */
    sr_drive_decals(sr, scene, view_id_base, dt_sec);

    /* Weather overlay (P2-weather-decals-tod): screen-space rain/snow driven
     * by the scene's weather settings.  Drawn last (after the scene + decals)
     * so the precipitation layers over everything in the color view. */
    if (scene_rendering)
        sr_drive_weather(sr, scene_rendering, view_id_base, dt_sec);

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

/* ── TAA driver (caller-cooperative; see header contract) ─────────────── */

bool jce_scene_renderer_taa_begin_frame(JceSceneRenderer *sr,
                                        uint32_t target_w, uint32_t target_h,
                                        const jce_mat4 *clean_view,
                                        const jce_mat4 *clean_proj,
                                        jce_mat4 *out_jittered_proj)
{
    /* Default behaviour (OFF / bad args): hand back the clean proj unchanged
       so the caller's colour pass is byte-identical to today. */
    if (out_jittered_proj && clean_proj)
        *out_jittered_proj = *clean_proj;

    if (!sr || !clean_view || !clean_proj || !out_jittered_proj)
        return false;

    bool on = sr->cv_taa ? jce_cvar_get_bool(sr->cv_taa) : false;
    if (!on || !sr->postfx_pipeline)
        return false;

    /* Advance the Halton jitter sequence for this frame. */
    jce_taa_advance(&sr->taa_state, target_w, target_h);

    /* Jitter ONLY the colour pass projection (a copy of the clean proj). */
    jce_mat4 jittered = *clean_proj;
    jce_taa_apply_jitter(&jittered, sr->taa_state.current_jitter);
    *out_jittered_proj = jittered;

    /* Motion-vec / reproject matrices come from the CLEAN (un-jittered)
       camera so reprojection is jitter-free:
         inv_view_proj = inverse(clean_proj * clean_view)
         prev_view_proj = prev_proj * prev_view (when a previous frame exists;
                          otherwise reuse this frame's so the on-screen test in
                          fs_taa rejects the empty history → output ≈ current). */
    jce_mat4 view_proj   = jce_m4_multiply(clean_proj, clean_view);
    jce_mat4 inv_vp      = jce_m4_inverse(&view_proj);
    jce_mat4 prev_vp;
    if (sr->taa_state.prev_valid)
        prev_vp = jce_m4_multiply(&sr->taa_state.prev_proj,
                                  &sr->taa_state.prev_view);
    else
        prev_vp = view_proj;

    jce_postfx_set_taa_matrices(sr->postfx_pipeline, &inv_vp, &prev_vp);
    jce_postfx_set_taa(sr->postfx_pipeline, true, 0.9f, 1.0f, 1.0f);
    return true;
}

void jce_scene_renderer_set_taa_velocity_enabled(JceSceneRenderer *sr, bool enabled)
{
    if (sr) sr->taa_want_velocity = enabled;
}

void jce_scene_renderer_begin_velocity_frame(JceSceneRenderer *sr)
{
    /* Call EXACTLY ONCE per displayed frame, before any viewport renders.  The
     * editor draws the Scene + Game viewports through this one shared renderer
     * each frame; bumping the generation here lets the skinned-anim pass and the
     * prev-view-proj roll run only on the FIRST viewport of the frame, so the
     * previous-frame palette/camera used for TAA motion vectors stays last
     * frame's instead of being clobbered (zeroed) by the 2nd viewport. */
    if (sr) { sr->vel_frame_gen++; sr->velocity_frame_driven = true; }
}

uint16_t jce_scene_renderer_get_velocity_texture(const JceSceneRenderer *sr)
{
    if (!sr || !sr->velocity_valid_frame) return UINT16_MAX;
    return sr->ssao_velocity_tex.idx;
}

void jce_scene_renderer_taa_end_frame(JceSceneRenderer *sr,
                                      const jce_mat4 *clean_view,
                                      const jce_mat4 *clean_proj)
{
    if (!sr) return;

    bool on = sr->cv_taa ? jce_cvar_get_bool(sr->cv_taa) : false;
    if (on && clean_view && clean_proj) {
        /* Stash UN-JITTERED matrices for next frame's reproject. */
        jce_taa_record_camera(&sr->taa_state, clean_view, clean_proj);
    }
    /* ALWAYS disable TAA on the shared pipeline after the scene pass so it
       never leaks into the editor's pick / preview / thumbnail postfx
       invocations (mirrors the forwardplus flag-reset discipline). */
    if (sr->postfx_pipeline)
        jce_postfx_set_taa(sr->postfx_pipeline, false, 0.9f, 1.0f, 1.0f);
    /* Reset the per-frame velocity request so a later render() (game view /
       pick / preview) does not produce velocity unless it asks again. */
    sr->taa_want_velocity = false;
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

void jce_scene_renderer_composite_ssr(JceSceneRenderer *sr, uint16_t view_id,
                                      uint16_t dst_fb_idx)
{
    if (!sr || !sr->ssr || !sr->ssr_active_frame) return;
    jce_ssr_composite(sr->ssr, view_id, dst_fb_idx);
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

void jce_scene_renderer_set_anim_sm_active(JceSceneRenderer *sr, bool active)
{
    if (sr) sr->anim_sm_active = active;
}

void jce_scene_renderer_set_anim_event_fn(JceSceneRenderer *sr,
                                          JceSceneRendererAnimEventFn fn,
                                          void *user)
{
    if (!sr) return;
    sr->anim_event_fn   = fn;
    sr->anim_event_user = user;
}

void jce_scene_renderer_set_anim_state_fn(JceSceneRenderer *sr,
                                          JceSceneRendererAnimStateFn fn,
                                          void *user)
{
    if (!sr) return;
    sr->anim_state_fn   = fn;
    sr->anim_state_user = user;
}

void jce_scene_renderer_set_ground_query_fn(JceSceneRenderer *sr,
                                            JceSceneRendererGroundQueryFn fn,
                                            void *user)
{
    if (!sr) return;
    sr->ground_query_fn   = fn;
    sr->ground_query_user = user;
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
        sr_terrain_free_chunks(sr, i);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        memset(&sr->terrain_cache[i], 0, sizeof sr->terrain_cache[i]);
    }
}

void jce_scene_renderer_invalidate_tilemap(JceSceneRenderer *sr,
                                            const char *path)
{
    if (!sr) return;
    for (int i = 0; i < SR_TILEMAP_SLOT_MAX; i++) {
        if (!sr->tilemap_cache[i].used) continue;
        if (path && *path &&
            strncmp(sr->tilemap_cache[i].path, path,
                    sizeof sr->tilemap_cache[i].path) != 0)
            continue;
        sr_tilemap_free_slot(sr, i);
    }
}

void jce_scene_renderer_invalidate_model_cache(JceSceneRenderer *sr)
{
    if (!sr) return;
    /* Drop EVERY cached model so the next frame reloads from disk.  Used on
     * scene-switch: the model cache never evicts and caches load FAILURES
     * (failed flag) keyed by path, so a model that exists in the NEW scene but
     * shared a path/name with a missing asset in the OLD scene would keep its
     * stale failed flag and never be retried (only an editor restart cleared
     * it).  Mirrors the model-cache teardown in jce_scene_renderer_destroy()
     * (join async decode, free the CPU job, destroy the GPU model) but fully
     * resets each slot via memset so it stays reusable. */
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->pending) {
            if (e->thr) { jce_thread_join(e->thr); e->thr = NULL; }
            if (e->job) {
                jce_model_gltf_cpu_free(e->job->cpu);
                if (e->job->done) jce_atomic_i32_destroy(e->job->done);
                JCE_FREE(e->job);
                e->job = NULL;
            }
        }
        if (e->model) jce_model_destroy(e->model);
        memset(e, 0, sizeof(*e));
    }
    sr->model_inflight = 0;
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

bool jce_scene_renderer_spawn_decal(JceSceneRenderer    *sr,
                                    const JceDecalSpawn *spawn)
{
    if (!sr || !spawn || !sr->pak) return false;
    if (!sr->decals) {
        JceDecalPoolDesc d = { 1024u, sr->pak };
        sr->decals = jce_decals_create(&d);
        if (!sr->decals) return false;   /* shader missing — fail soft */
    }
    return jce_decals_spawn(sr->decals, spawn);
}
