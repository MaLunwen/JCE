/*
 * jce_sr_draw.c  Scene-renderer draw module (split from
 * jce_scene_renderer.c).
 *
 * The color-pass + shadow-pass GPU-instancing batches, the GPU-driven
 * instanced draw, the per-entity model / material draw + shadow-submit
 * helpers, and the main entity-rendering pass (sr_draw_entities).  Pure move
 * from the monolithic renderer: cross-module entry points are declared in
 * jce_sr_internal.h, everything else stays file-static here.  No behaviour
 * change.
 */

#include "jce_sr_internal.h"

/* If entity e is a skeletal-animator model, draw its skinned silhouette into
 * the given shadow view using the palette cached by sr_update_skinned_anims,
 * and return true so the caller skips the static-mesh shadow path (mirrors
 * the color pass, which routes such entities through jce_model_draw and skips
 * their MeshRenderer).  Returns false for non-skinned entities. */
bool sr_try_submit_skinned_shadow(JceSceneRenderer *sr, JceScene *scene,
                                         JceEntity e, int cull_idx,
                                         uint16_t view_id)
{
    if (!jce_scene_has_skeletal_animator(scene, e)) return false;
    JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
    if (!sa || !sa->skeleton_path[0]) return false;

    SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    if (!jce_scene_has_transform(scene, e)) return false;
    /* Same matrix the skinned color submit uses (sr_build_entity_model →
     * world matrix), so the cast shadow stays attached for parented
     * entities and for entities with an edited pivot.  #8 — reuse the
     * per-frame cached world matrix when available (byte-identical source). */
    jce_mat4 model = (cull_idx >= 0 && sr->ecull && sr->ecull[cull_idx].world_valid)
                       ? sr->ecull[cull_idx].world
                       : jce_scene_get_world_matrix(scene, e);

    SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
    const jce_mat4 *pal = (ai && ai->skin_palette_count > 0) ? ai->skin_palette : NULL;
    uint32_t pal_n = ai ? ai->skin_palette_count : 0;
    /* FEATURE 3.1: shadow MUST consume the SAME per-instance morph VBs as the
     * color pass (same callback + same ai) so the cast silhouette matches the
     * morphed mesh; else the plain shadow draw (byte-identical). */
    if (ai && ai->morph_vb_count > 0)
        jce_model_draw_morphed_shadow(mc->model, sr->renderer, view_id, &model,
                                      pal, pal_n, sr_morph_vb_cb, ai);
    else
        jce_model_draw_shadow(mc->model, sr->renderer, view_id, &model, pal, pal_n);
    return true;
}

const char *sr_mesh_renderer_model_path(JceScene *scene, JceEntity e)
{
    if (!jce_scene_has_mesh_renderer(scene, e)) return NULL;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_RENDERER)) return NULL;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    if (!mr || !mr->visible || !mr->mesh_path[0]) return NULL;
    return sr_is_gltf_model_path(mr->mesh_path) ? mr->mesh_path : NULL;
}

/* Shadow-pass instancing batch helpers — defined below alongside the color
 * batch (sr_inst_*); forward-declared here for the shadow submit path. */
bool sr_sh_batch_add(JceSceneRenderer *sr, JceModel *model,
                            const jce_mat4 *world);
void sr_sh_flush(JceSceneRenderer *sr, uint16_t view_id);

bool sr_try_submit_mesh_renderer_model_shadow(JceSceneRenderer *sr,
                                                     JceScene *scene,
                                                     JceEntity e,
                                                     int cull_idx,
                                                     uint16_t view_id)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path || !jce_scene_has_transform(scene, e)) return false;

    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    /* #8 — reuse the per-frame cached world matrix (byte-identical source) to
     * skip a parent-chain walk; this helper is the city's per-cascade hot path. */
    jce_mat4 model = (cull_idx >= 0 && sr->ecull && sr->ecull[cull_idx].world_valid)
                       ? sr->ecull[cull_idx].world
                       : jce_scene_get_world_matrix(scene, e);

    /* GPU-instancing fast path (mirrors the color pass): batch instanceable
     * static glb casters per shadow view, flushing when the target view changes
     * (and once before the color pass), so a cascade's thousands of repeated
     * buildings collapse to ~unique-mesh depth submits — the full-load shadow
     * bottleneck.  Skinned models are not instanceable and fall through. */
    if (jce_model_is_instanceable(mc->model)) {
        if (sr->sh_batch_count > 0 && view_id != sr->sh_batch_view)
            sr_sh_flush(sr, sr->sh_batch_view);
        sr->sh_batch_view = view_id;
        if (sr_sh_batch_add(sr, mc->model, &model))
            return true;
    }

    jce_model_draw_shadow(mc->model, sr->renderer, view_id, &model, NULL, 0);
    return true;
}

/* Build a PBR material override from an authored MeshRenderer so a glTF MODEL
 * (drawn via jce_model_draw, which otherwise uses the model's embedded
 * materials) honours the scene-assigned material/textures — standard "renderer
 * material slot overrides imported-mesh default" behaviour.  Returns true +
 * fills *out only when the MeshRenderer authored an albedo texture (directly or
 * via its material/mesh sidecar) that actually resolved; returns false to keep
 * the model's own materials (e.g. a self-textured glb whose MeshRenderer has no
 * authored albedo). */
static bool sr_mr_override_material(JceSceneRenderer *sr, const JceMeshRenderer *mr,
                                    JcePbrMaterial *out)
{
    if (!mr) return false;
    if (!mr->albedo_tex[0] && !mr->material_path[0]) return false;  /* not authored */

    JcePbrMaterial pbr = jce_pbr_material_default();
    if (mr->base_color[3] > 0.0f) {
        pbr.base_color_factor[0] = mr->base_color[0];
        pbr.base_color_factor[1] = mr->base_color[1];
        pbr.base_color_factor[2] = mr->base_color[2];
        pbr.base_color_factor[3] = mr->base_color[3];
    }
    pbr.metallic_factor     = mr->metallic;
    pbr.roughness_factor    = mr->roughness;
    pbr.emissive_factor[0]  = mr->emissive[0];
    pbr.emissive_factor[1]  = mr->emissive[1];
    pbr.emissive_factor[2]  = mr->emissive[2];
    pbr.normal_scale        = fabsf(mr->normal_scale);
    pbr.ao_strength         = mr->ao_strength;
    pbr.alpha_mode          = (JceAlphaMode)mr->alpha_mode;
    pbr.alpha_cutoff        = mr->alpha_cutoff;
    pbr.double_sided        = mr->double_sided;
    pbr.receive_shadows_off = mr->shadow_receive_off;

    if (mr->albedo_tex[0]) {
        JceTexture t = sr_resolve_texture(sr, mr->albedo_tex);
        if (jce_texture_valid(t)) pbr.albedo_map = t;
    }
    if (!jce_texture_valid(pbr.albedo_map) &&
        (mr->material_path[0] || mr->mesh_path[0])) {
        JceTexture t = sr_resolve_texture2(sr,
            mr->material_path[0] ? mr->material_path : NULL,
            mr->mesh_path[0]     ? mr->mesh_path     : NULL);
        if (jce_texture_valid(t)) pbr.albedo_map = t;
    }
    /* Only override once the authored albedo actually resolved — otherwise keep
     * the model's embedded material (avoids a white flash while the async
     * texture load is still in flight). */
    if (!jce_texture_valid(pbr.albedo_map)) return false;

    if (mr->mr_tex[0]) {
        JceTexture t = sr_resolve_texture(sr, mr->mr_tex);
        if (jce_texture_valid(t)) pbr.metallic_roughness_map = t;
    }
    if (mr->normal_tex[0]) {
        JceTexture t = sr_resolve_texture(sr, mr->normal_tex);
        if (jce_texture_valid(t)) pbr.normal_map = t;
    }
    if (mr->ao_tex[0]) {
        JceTexture t = sr_resolve_texture(sr, mr->ao_tex);
        if (jce_texture_valid(t)) pbr.ao_map = t;
    }
    if (mr->emissive_tex[0]) {
        JceTexture t = sr_resolve_texture(sr, mr->emissive_tex);
        if (jce_texture_valid(t)) pbr.emissive_map = t;
    }
    /* SSAO: when active, bind the screen-space AO result into the AO sampler
     * stage (the shader samples it by screen UV via u_ssaoParams) — replacing
     * the material AO map while SSAO is on. */
    if (sr->ssao_active_frame && sr->ssao_ao_idx != UINT16_MAX) {
        JceTexture st; st.idx = sr->ssao_ao_idx;
        pbr.ao_map = st;
    }
    *out = pbr;
    return true;
}

/* ── Color-pass GPU-instancing batch (see SrInstEntry) ──────────────── */
static bool sr_inst_batch_add(JceSceneRenderer *sr, JceModel *model,
                              const jce_mat4 *world)
{
    if (sr->inst_batch_count >= sr->inst_batch_cap) {
        uint32_t nc = sr->inst_batch_cap ? sr->inst_batch_cap * 2u : 256u;
        SrInstEntry *nb = (SrInstEntry *)JCE_REALLOC(
            sr->inst_batch, (size_t)nc * sizeof(SrInstEntry));
        if (!nb) return false;
        sr->inst_batch = nb;
        sr->inst_batch_cap = nc;
    }
    sr->inst_batch[sr->inst_batch_count].model = model;
    sr->inst_batch[sr->inst_batch_count].world = *world;
    sr->inst_batch_count++;
    return true;
}

static int sr_inst_cmp(const void *a, const void *b)
{
    uintptr_t ma = (uintptr_t)((const SrInstEntry *)a)->model;
    uintptr_t mb = (uintptr_t)((const SrInstEntry *)b)->model;
    return (ma > mb) - (ma < mb);
}

/* ── GPU-driven instanced draw (roadmap #18, Phase 0+1) ─────────────────── */

/* GPU-driven pass-1 for one model-run: build the per-instance GPUScene records
 * (render matrix folded with the single primitive's node-local transform + the
 * world-space AABB derived from the model's local AABB) and append them to the
 * frame's cull batch (one dispatch covers all runs).  On success records the
 * (model, partition base, count) for the pass-2 draw and returns true.  Returns
 * false when the run is not GPU-eligible (caller falls back to the CPU
 * instanced path for it). */
static bool sr_gpu_add_run(JceSceneRenderer *sr,
                           JceModel *m, uint32_t base, uint32_t run)
{
    if (!sr->gpu_driven_frame || !sr->gpu_frame_planes_valid) return false;

    jce_mat4 node_lt;
    if (!jce_model_gpu_instanceable(m, &node_lt)) return false;

    float lmn[3], lmx[3];
    if (!jce_model_get_aabb(m, lmn, lmx)) return false;

    /* Grow the per-run record scratch. */
    if (sr->gpu_rec_cap < run) {
        JceGpuSceneRecord *nr = (JceGpuSceneRecord *)JCE_REALLOC(
            sr->gpu_rec, (size_t)run * sizeof(JceGpuSceneRecord));
        if (!nr) return false;
        sr->gpu_rec = nr;
        sr->gpu_rec_cap = run;
    }

    /* Local-AABB corners (model space; already includes node_lt baking — see
     * the glTF loader).  Transformed by the ENTITY world (not folded) for the
     * cull AABB; the render matrix folds node_lt so it matches the CPU path
     * (compute_static_node_world returns root*node_lt for non-joint nodes). */
    const jce_vec3 corners[8] = {
        { lmn[0], lmn[1], lmn[2] }, { lmx[0], lmn[1], lmn[2] },
        { lmn[0], lmx[1], lmn[2] }, { lmx[0], lmx[1], lmn[2] },
        { lmn[0], lmn[1], lmx[2] }, { lmx[0], lmn[1], lmx[2] },
        { lmn[0], lmx[1], lmx[2] }, { lmx[0], lmx[1], lmx[2] },
    };

    for (uint32_t k = 0; k < run; k++) {
        const jce_mat4 *ew = &sr->inst_batch[base + k].world;  /* entity world */
        JceGpuSceneRecord *rec = &sr->gpu_rec[k];

        /* Render matrix = entity_world * node_lt (column vectors). */
        rec->world = jce_m4_multiply(ew, &node_lt);

        /* World AABB from the model-local AABB transformed by entity_world. */
        jce_vec3 bmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
        jce_vec3 bmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int ci = 0; ci < 8; ci++) {
            const jce_vec4 cv = { corners[ci].x, corners[ci].y, corners[ci].z, 1.0f };
            const jce_vec4 wv = jce_m4_mul_v4(ew, cv);
            if (wv.x < bmn.x) bmn.x = wv.x; if (wv.x > bmx.x) bmx.x = wv.x;
            if (wv.y < bmn.y) bmn.y = wv.y; if (wv.y > bmx.y) bmx.y = wv.y;
            if (wv.z < bmn.z) bmn.z = wv.z; if (wv.z > bmx.z) bmx.z = wv.z;
        }
        rec->center[0] = (bmn.x + bmx.x) * 0.5f;
        rec->center[1] = (bmn.y + bmx.y) * 0.5f;
        rec->center[2] = (bmn.z + bmx.z) * 0.5f;
        rec->extent[0] = (bmx.x - bmn.x) * 0.5f;
        rec->extent[1] = (bmx.y - bmn.y) * 0.5f;
        rec->extent[2] = (bmx.z - bmn.z) * 0.5f;
        float r2 = rec->extent[0]*rec->extent[0] + rec->extent[1]*rec->extent[1]
                 + rec->extent[2]*rec->extent[2];
        rec->radius = sqrtf(r2);
        rec->_pad_extent = 0.0f;
        /* run_base / run_index filled by jce_gpu_scene_add_run. */
        rec->run_base = 0.0f; rec->run_index = 0.0f;
        rec->_id_z = 0.0f; rec->_id_w = 0.0f;
    }

    uint32_t run_base = 0;
    if (!jce_gpu_scene_add_run(sr->gpu_scene, sr->gpu_rec, run, &run_base))
        return false;

    /* Record the run for the pass-2 draw. */
    if (sr->gpu_draw_count >= sr->gpu_draw_cap) {
        uint32_t nc = sr->gpu_draw_cap ? sr->gpu_draw_cap * 2u : 256u;
        void *nb = JCE_REALLOC(sr->gpu_draw, (size_t)nc * sizeof(*sr->gpu_draw));
        if (!nb) return false;   /* keep already-added records; CPU-draw this run */
        sr->gpu_draw = nb;
        sr->gpu_draw_cap = nc;
    }
    sr->gpu_draw[sr->gpu_draw_count].model = m;
    sr->gpu_draw[sr->gpu_draw_count].base  = run_base;  /* GPU partition base */
    sr->gpu_draw[sr->gpu_draw_count].src   = base;      /* inst_batch index   */
    sr->gpu_draw[sr->gpu_draw_count].count = run;
    sr->gpu_draw_count++;
    return true;
}

/* Sort the batch by model and issue one jce_model_draw_instanced per model run.
 * Each model's repeated copies become a single instanced submit, so the PBR
 * material uniforms are written once per model instead of once per copy.
 *
 * GPU-driven (roadmap #18, r.gpu_driven on): each eligible single-primitive run
 * is routed through the GPUScene buffer + compute frustum cull — pass 1 builds
 * the records into one batch, a SINGLE dispatch culls/compacts them all (bgfx
 * orders the compute view before the color view, so every survivor is resident
 * before any draw), then pass 2 draws each run from its compacted partition.
 * Ineligible runs (and the whole batch when gpu_driven is off) take the EXACT
 * CPU path, byte-identical to before. */
static void sr_inst_flush(JceSceneRenderer *sr, uint16_t view_id)
{
    if (sr->inst_batch_count == 0) return;
    qsort(sr->inst_batch, sr->inst_batch_count, sizeof(SrInstEntry), sr_inst_cmp);
    if (sr->inst_gather_cap < sr->inst_batch_count) {
        jce_mat4 *ng = (jce_mat4 *)JCE_REALLOC(
            sr->inst_gather, (size_t)sr->inst_batch_count * sizeof(jce_mat4));
        if (!ng) { sr->inst_batch_count = 0; return; }
        sr->inst_gather = ng;
        sr->inst_gather_cap = sr->inst_batch_count;
    }

    const bool gpu = sr->gpu_driven_frame && sr->gpu_frame_planes_valid;
    if (gpu) {
        jce_gpu_scene_begin(sr->gpu_scene);
        sr->gpu_draw_count = 0;
    }

    /* Pass 1: per model-run, either queue it into the GPU cull batch or draw it
     * immediately via the CPU instanced path (ineligible / gpu off). */
    uint32_t i = 0;
    while (i < sr->inst_batch_count) {
        JceModel *m = sr->inst_batch[i].model;
        uint32_t run = 0;
        while (i + run < sr->inst_batch_count &&
               sr->inst_batch[i + run].model == m) {
            run++;
        }
        if (!(gpu && sr_gpu_add_run(sr, m, i, run))) {
            for (uint32_t k = 0; k < run; k++)
                sr->inst_gather[k] = sr->inst_batch[i + k].world;
            jce_model_draw_instanced(m, sr->renderer, view_id, sr->inst_gather, run);
        }
        i += run;
    }

    /* Pass 2: one cull dispatch, then draw each GPU run from its compacted
     * partition.  If the dispatch fails (e.g. buffer alloc), fall back to a CPU
     * instanced draw of each queued run so nothing is dropped. */
    if (gpu && sr->gpu_draw_count > 0) {
        bool ok = jce_gpu_scene_dispatch(sr->gpu_scene, sr->gpu_cull_view,
                                         sr->gpu_frame_planes);
        uint16_t vis = jce_gpu_scene_visible_vb(sr->gpu_scene);
        for (uint32_t d = 0; d < sr->gpu_draw_count; d++) {
            JceModel *m  = sr->gpu_draw[d].model;
            uint32_t bse = sr->gpu_draw[d].base;
            uint32_t src = sr->gpu_draw[d].src;
            uint32_t cnt = sr->gpu_draw[d].count;
            if (ok && vis != UINT16_MAX) {
                jce_model_draw_instanced_from_buffer(m, sr->renderer, view_id,
                                                     vis, bse, cnt);
            } else {
                /* Dispatch unavailable (rare GPU OOM): CPU-draw the queued run
                 * from its still-intact inst_batch entries. */
                for (uint32_t k = 0; k < cnt; k++)
                    sr->inst_gather[k] = sr->inst_batch[src + k].world;
                jce_model_draw_instanced(m, sr->renderer, view_id,
                                         sr->inst_gather, cnt);
            }
        }
    }

    sr->inst_batch_count = 0;
}

/* ── Shadow-pass GPU-instancing batch (depth-only; see sh_batch) ────── */
bool sr_sh_batch_add(JceSceneRenderer *sr, JceModel *model,
                            const jce_mat4 *world)
{
    if (sr->sh_batch_count >= sr->sh_batch_cap) {
        uint32_t nc = sr->sh_batch_cap ? sr->sh_batch_cap * 2u : 256u;
        SrInstEntry *nb = (SrInstEntry *)JCE_REALLOC(
            sr->sh_batch, (size_t)nc * sizeof(SrInstEntry));
        if (!nb) return false;
        sr->sh_batch = nb;
        sr->sh_batch_cap = nc;
    }
    sr->sh_batch[sr->sh_batch_count].model = model;
    sr->sh_batch[sr->sh_batch_count].world = *world;
    sr->sh_batch_count++;
    return true;
}

/* Sort the shadow batch by model and issue one jce_model_draw_shadow_instanced
 * per model run into `view_id` (depth-only).  Mirrors sr_inst_flush; reuses the
 * color batch's inst_gather scratch (shadow flushes are sequenced before the
 * color flush, so there is no overlap). */
void sr_sh_flush(JceSceneRenderer *sr, uint16_t view_id)
{
    if (sr->sh_batch_count == 0) return;
    qsort(sr->sh_batch, sr->sh_batch_count, sizeof(SrInstEntry), sr_inst_cmp);
    if (sr->inst_gather_cap < sr->sh_batch_count) {
        jce_mat4 *ng = (jce_mat4 *)JCE_REALLOC(
            sr->inst_gather, (size_t)sr->sh_batch_count * sizeof(jce_mat4));
        if (!ng) { sr->sh_batch_count = 0; return; }
        sr->inst_gather = ng;
        sr->inst_gather_cap = sr->sh_batch_count;
    }
    uint32_t i = 0;
    while (i < sr->sh_batch_count) {
        JceModel *m = sr->sh_batch[i].model;
        uint32_t run = 0;
        while (i + run < sr->sh_batch_count &&
               sr->sh_batch[i + run].model == m) {
            sr->inst_gather[run] = sr->sh_batch[i + run].world;
            run++;
        }
        jce_model_draw_shadow_instanced(m, sr->renderer, view_id, sr->inst_gather, run);
        i += run;
    }
    sr->sh_batch_count = 0;
}

static bool sr_try_draw_mesh_renderer_model(JceSceneRenderer *sr,
                                            JceScene *scene,
                                            JceEntity e,
                                            uint16_t view_id,
                                            const jce_mat4 *model,
                                            const JceSceneRenderConfig *cfg)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path) return false;

    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    /* Standard material-slot override: a MeshRenderer that authored a material/
     * albedo texture wins over the model's embedded materials.  Fixes
     * geometry-only models converted to glTF whose texture lives in the scene. */
    JcePbrMaterial ov;
    bool have_ov = sr_mr_override_material(sr,
        jce_scene_get_mesh_renderer(scene, e), &ov);

    /* Editor missing-albedo checker — ONLY in the TEXTURED debug view, so the
     * normal SHADED view (and runtime) renders an untextured model with its
     * lit base color instead of the pink-black hint. */
    bool editor_mode = (sr->has_cbs && sr->cbs.load_texture);
    bool checker = editor_mode && cfg &&
        cfg->view_mode == JCE_SCENE_VIEW_TEXTURED;

    /* GPU-instancing fast path: a model with no per-entity material override and
     * no skinned primitives, outside the checker debug view, is deferred into
     * the per-model instance batch (flushed once per model at pass end).  This
     * collapses the streamed city's thousands of repeated-mesh draws into a
     * handful of instanced submits, keeping per-frame uniform writes under
     * bgfx's fixed VK uniform scratch.  Falls through to the immediate draw on
     * batch-OOM. */
    if (!have_ov && !checker && jce_model_is_instanceable(mc->model)) {
        if (sr_inst_batch_add(sr, mc->model, model))
            return true;
    }

    if (have_ov) jce_model_set_material_override(&ov);
    if (checker) jce_model_set_albedo_checker(true);

    jce_model_draw(mc->model, sr->renderer, view_id, model, NULL, 0);

    if (checker) jce_model_set_albedo_checker(false);
    if (have_ov) jce_model_set_material_override(NULL);
    return true;
}

/* ── Entity rendering ─────────────────────────────────────────────── */

/* jce_model_draw pre-submit hook: re-bind the Forward+ cluster texture +
 * uniforms (stage 14 = s_cluster) before EVERY primitive submit a skinned /
 * LOD / static glTF model issues inside jce_model_draw.  jce_model_draw emits
 * one bgfx_submit per primitive and bgfx clears stage/uniform state between
 * submits, so binding once before the call would only cover the first
 * primitive; the hook fires per submit so a multi-primitive character is fully
 * lit by the clustered point/spot lights.  Armed only while
 * sr->fp_active_frame is true (see the entity loop), so when r.forwardplus is
 * off the hook is never installed and the path is byte-identical.  Mirrors the
 * mesh paths' jce_forwardplus_bind, which is itself a no-op when the module is
 * disabled / not yet uploaded. */
static void sr_model_fwdplus_presubmit_cb(void *user, uint16_t view_id)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    (void)view_id;
    if (sr && sr->fp_active_frame)
        jce_forwardplus_bind(sr->forwardplus);
}

void sr_draw_entities(JceSceneRenderer *sr, JceScene *scene,
                             const JceCamera *camera, EntityList *list,
                             uint16_t view_id, float dt_sec,
                             const JceSceneRenderConfig *cfg)
{
    /* Animation is advanced in sr_update_skinned_anims (before the shadow
       pass); the color pass only consumes the cached palette. */

    /* Advance the water phase clock once per render (drives vs_water's
     * u_water_time).  dt_sec is 0 in still editor previews, so the surface
     * holds its pose there and animates in Play. */
    if (dt_sec > 0.0f) {
        sr->water_time += dt_sec;
        /* Keep the accumulator bounded so float precision stays sharp over
         * long sessions; wrap on a large multiple of 2*PI-ish period. */
        if (sr->water_time > 100000.0f) sr->water_time -= 100000.0f;
    }

    if (list->count == 0) return;

    /* Gather lights. */
    if (sr->light_env) {
        jce_light_env_clear(sr->light_env);
        /* Forward+ (ROUND A): accumulate the same point+spot lights into the
         * cluster scratch arrays as we add them to light_env.  Counts up to
         * fp_max_lights (256), independent of the brute-force env caps. */
        uint32_t fp_n = 0;
        if (sr->ambient_override_active) {
            jce_vec3 amb = sr->ambient_override_color;
            float    ai  = sr->ambient_override_intensity;
            /* Standard-engine sky fill: the editor passes the scene ambient
             * through this override.  A procedural sky (no HDR skybox supplying
             * IBL) contributes no environment ambient, so shadowed / back-lit
             * surfaces fall to this flat ambient and go near-black ("全部影").
             * Lift it toward the sky colour (cheap IBL-ambient stand-in) via the
             * same light_env path that already reaches every model submit.
             * max() so an already-bright authored ambient is never dimmed; HDR
             * skybox scenes (real IBL) are left untouched. */
            if (!sr->skybox_active) {
                jce_vec3 st, sh, sg;
                if (sr->tod_active) {
                    st = sr->tod_state.sky_top;
                    sh = sr->tod_state.sky_horizon;
                    sg = sr->tod_state.sky_ground;
                } else {
                    st = jce_v3(0.25f, 0.45f, 0.80f);
                    sh = jce_v3(0.65f, 0.78f, 0.92f);
                    sg = jce_v3(0.22f, 0.22f, 0.28f);
                }
                jce_vec3 sky = jce_v3((st.x*2.0f + sh.x*2.0f + sg.x) / 5.0f,
                                      (st.y*2.0f + sh.y*2.0f + sg.y) / 5.0f,
                                      (st.z*2.0f + sh.z*2.0f + sg.z) / 5.0f);
                const float fill = 0.5f;
                amb = jce_v3(fmaxf(amb.x*ai, sky.x*fill),
                             fmaxf(amb.y*ai, sky.y*fill),
                             fmaxf(amb.z*ai, sky.z*fill));
                ai = 1.0f;
            }
            jce_light_env_set_ambient(sr->light_env, amb, ai);
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

        if (sr->tod_active) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3_scale(sr->tod_state.sun_direction, -1.0f);
            dl.color     = sr->tod_state.sun_color;
            dl.intensity = 1.0f;
            dl.casts_shadow = true;
            jce_light_env_add_dir_light(sr->light_env, &dl);
        }
        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            JceTransform *xf = jce_scene_get_transform(scene, e);

            if (!sr->tod_active &&
                jce_scene_has_dir_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DIR_LIGHT)) {
                JceDirectionalLight *dlc = jce_scene_get_dir_light(scene, e);
                if (dlc) {
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = dlc->color;
                    dl.intensity = dlc->intensity > 0.0f ? dlc->intensity : 1.0f; dl.casts_shadow = dlc->casts_shadow;
                    dl.direction =
                        sr_light_world_shine_direction(&dlc->direction, xf);
                    /* P3-E.5 — propagate optional cookie only when authored.
                     * Legacy zero-initialized scene components may contain
                     * texture handle 0, which is a valid bgfx handle index but
                     * not an authored cookie. */
                    dl.cookie_texture =
                        (dlc->cookie_path[0] != '\0'
                         && jce_texture_valid(dlc->cookie_texture))
                        ? dlc->cookie_texture : JCE_TEXTURE_INVALID;
                    dl.cookie_strength =
                        (dlc->cookie_path[0] != '\0') ? dlc->cookie_strength : 0.0f;
                    jce_light_env_add_dir_light(sr->light_env, &dl);
                }
            }
            if (jce_scene_has_point_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_POINT_LIGHT) &&
                sr_light_selected(sr->frame_sel_point, sr->frame_sel_point_n, e)) {
                JcePointLight *plc = jce_scene_get_point_light(scene, e);
                if (plc) {
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = plc->color;
                    pl.intensity = plc->intensity > 0.0f ? plc->intensity : 1.0f;
                    pl.radius    = plc->radius    > 0.0f ? plc->radius    : 10.0f;
                    pl.position  = xf ? xf->position : plc->position;
                    pl.casts_shadow = plc->casts_shadow;
                    pl.shadow_bias  = plc->shadow_bias;
                    jce_light_env_add_point_light(sr->light_env, &pl);

                    /* Forward+ proxy + param (point light). */
                    if (sr->fp_proxies && fp_n < sr->fp_max_lights) {
                        JceLightProxy *gp = &sr->fp_proxies[fp_n];
                        JceForwardPlusLightParam *pp = &sr->fp_params[fp_n];
                        gp->position_ws = pl.position;
                        gp->radius      = pl.radius;
                        gp->user_id     = fp_n;
                        pp->color          = pl.color;
                        pp->intensity      = pl.intensity;
                        pp->type           = 0.0f; /* point */
                        pp->spot_dir       = jce_v3(0.0f, 0.0f, 0.0f);
                        pp->inner_cone_cos = 1.0f;
                        pp->outer_cone_cos = -1.0f;
                        fp_n++;
                    }
                }
            }
            if (jce_scene_has_spot_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPOT_LIGHT) &&
                sr_light_selected(sr->frame_sel_spot, sr->frame_sel_spot_n, e)) {
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
                    /* P3-E.5 — propagate cookie + IES only when authored.
                     * This prevents default/legacy spot lights from sampling
                     * arbitrary texture handle 0 as a projector. */
                    sl.cookie_texture =
                        (slc->cookie_path[0] != '\0'
                         && jce_texture_valid(slc->cookie_texture))
                        ? slc->cookie_texture : JCE_TEXTURE_INVALID;
                    sl.ies_lut_texture =
                        (slc->ies_path[0] != '\0'
                         && jce_texture_valid(slc->ies_lut_texture))
                        ? slc->ies_lut_texture : JCE_TEXTURE_INVALID;
                    sl.cookie_strength =
                        (slc->cookie_path[0] != '\0') ? slc->cookie_strength : 0.0f;
                    sl.casts_shadow = slc->casts_shadow;
                    sl.shadow_bias  = slc->shadow_bias;
                    jce_light_env_add_spot_light(sr->light_env, &sl);

                    /* Forward+ proxy + param (spot light).  The cluster
                     * broad-phase treats the spot as a bounding sphere
                     * (radius); the contract's per-light cone params let the
                     * shader evaluate the cone afterwards. */
                    if (sr->fp_proxies && fp_n < sr->fp_max_lights) {
                        JceLightProxy *gp = &sr->fp_proxies[fp_n];
                        JceForwardPlusLightParam *pp = &sr->fp_params[fp_n];
                        gp->position_ws = sl.position;
                        gp->radius      = sl.radius;
                        gp->user_id     = fp_n;
                        pp->color          = sl.color;
                        pp->intensity      = sl.intensity;
                        pp->type           = 1.0f; /* spot */
                        pp->spot_dir       = jce_v3_normalize(sl.direction);
                        pp->inner_cone_cos = sl.inner_cone_cos;
                        pp->outer_cone_cos = sl.outer_cone_cos;
                        fp_n++;
                    }
                }
            }
        }

        if (sr->tod_active)
            jce_light_env_set_ambient(sr->light_env, sr->tod_state.ambient_color, 1.0f);

        if (camera) {
            jce_vec3 cp = jce_camera_get_position(camera);
            jce_light_env_set_camera_pos(sr->light_env, cp);
        }
        jce_light_env_apply(sr->light_env, sr->renderer);

        /* Forward+ clustered lighting (ROUND B): read the r.forwardplus cvar
         * (default off), drive the module's enable + the renderer's program
         * swap, build the froxel cluster from the gathered point+spot lights,
         * and upload it to the SINGLE combined GPU data texture.  When the
         * cvar is off everything below is skipped and the renderer keeps the
         * non-variant programs -> byte-identical to the brute-force path. */
        bool fp_on = sr->cv_forwardplus
                   ? jce_cvar_get_bool(sr->cv_forwardplus) : false;
        if (sr->forwardplus)
            jce_forwardplus_set_enabled(sr->forwardplus, fp_on);

        if (fp_on && sr->forwardplus && jce_forwardplus_is_enabled(sr->forwardplus)
            && camera) {
            float aspect = (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                ? (float)cfg->viewport_width / (float)cfg->viewport_height
                : 16.0f / 9.0f;
            jce_mat4 fp_view = jce_camera_view(camera);
            jce_mat4 fp_inv_view = jce_m4_inverse(&fp_view);
            jce_mat4 fp_proj = jce_camera_proj(camera, aspect,
                                               sr->homogeneous_depth);
            float fp_near = jce_camera_get_near(camera);
            float fp_far  = jce_camera_get_far(camera);
            jce_forwardplus_update(sr->forwardplus,
                                   sr->fp_proxies, sr->fp_params, fp_n,
                                   &fp_inv_view, &fp_proj,
                                   fp_near, fp_far, sr->renderer);
        }
    }

    /* Drive the renderer program swap + per-frame "active" flag for the PBR
     * submit paths.  Active ONLY when the cvar is on, the module is enabled,
     * AND the fs_pbr_fwdplus variant program actually loaded (an older pak
     * without the variant bins keeps the brute-force path).  When inactive,
     * the renderer returns the non-variant programs and the bind callbacks
     * skip jce_forwardplus_bind -> nothing touches sampler stage 14. */
    {
        bool fp_on = sr->cv_forwardplus
                   ? jce_cvar_get_bool(sr->cv_forwardplus) : false;
        bool variant_ok = sr->renderer &&
            jce_shader_valid(jce_renderer_get_program_pbr_fwdplus(sr->renderer));
        sr->fp_active_frame = fp_on && sr->forwardplus &&
            jce_forwardplus_is_enabled(sr->forwardplus) && variant_ok;
        if (sr->renderer)
            jce_renderer_set_forwardplus_program_active(sr->renderer,
                                                        sr->fp_active_frame);
    }

    /* Baked GI: pick the dominant reflection probe + light-probe SH9 for
     * this frame; the per-material bind cbs consume sr->gi_*. */
    sr_gather_baked_gi(sr, scene, camera, list);

    jce_vec3 legacy_dir, legacy_color;
    float legacy_intensity = 1.0f;
    bool legacy_has_dir = sr_resolve_primary_dir_light(sr, scene, list, false,
        &legacy_dir, &legacy_color, &legacy_intensity);
    if (legacy_has_dir) {
        JceDirLight sun = jce_dir_light_default();
        sun.direction = legacy_dir;
        sun.color = jce_v3_scale(legacy_color, legacy_intensity);
        jce_lighting_apply(sr->renderer, &sun);
    } else {
        float raw_dir[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        float raw_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        bgfx_set_uniform(sr->u_light_dir, raw_dir, 1);
        bgfx_set_uniform(sr->u_light_color, raw_color, 1);
    }

    /* WIREFRAME_TEXTURED debug view: trigger fs_mesh.sc hue-Lambert branch by
     * setting u_lightDir.w = 0.25 (matches 0.5.7 behaviour). Done after
     * jce_lighting_apply since that overwrites these uniforms. */
    if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED &&
        sr->has_cbs && sr->cbs.load_texture)
    {
        jce_vec3 sd = legacy_has_dir ? legacy_dir : jce_v3(0.0f, 1.0f, 0.0f);
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
        /* Use the REAL viewport aspect (matches the velocity/SSAO pre-pass cull)
         * — a hardcoded 16:9 here disagreed with the pre-pass in a docked,
         * non-16:9 editor viewport, so edge streamed objects were drawn (and
         * TAA-jittered) in the color pass but had no motion vector from the
         * velocity pass => the whole streamed set shimmered every frame. */
        const float cull_aspect =
            (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                ? (float)cfg->viewport_width / (float)cfg->viewport_height
                : (16.0f / 9.0f);
        const jce_mat4 p = jce_camera_proj(camera, cull_aspect,
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

    /* GPU-driven cull: stash the color-pass camera frustum planes for the
     * compute cull dispatch in sr_inst_flush.  Computed independently of the
     * CPU frustum_culling gate (the GPU performs the cull for instanced meshes),
     * with the SAME aspect/proj as the CPU cull above so visibility agrees. */
    sr->gpu_frame_planes_valid = false;
    if (sr->gpu_driven_frame && camera && list->count > 0) {
        const jce_mat4 gv = jce_camera_view(camera);
        const float ga =
            (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                ? (float)cfg->viewport_width / (float)cfg->viewport_height
                : (16.0f / 9.0f);
        const jce_mat4 gp = jce_camera_proj(camera, ga, sr->homogeneous_depth);
        const jce_mat4 gvp = jce_m4_multiply(&gp, &gv);
        sr_extract_frustum_planes(&gvp, sr->gpu_frame_planes);
        sr->gpu_frame_planes_valid = true;
    }

    /* Reset per-frame LOD pick counters. */
    sr->stat_lod_enabled = (sr->global_lod && sr->global_lod->count > 0);
    for (int li = 0; li < JCE_LOD_MAX_LEVELS; li++) sr->stat_lod_picks[li] = 0;
    sr->stat_lod_culled = 0;

    /* Reset per-frame terrain chunk counters. */
    sr->stat_terrain_chunks_total  = 0;
    sr->stat_terrain_chunks_drawn  = 0;
    sr->stat_terrain_chunks_culled = 0;

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
        const float oc_aspect =
            (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                ? (float)cfg->viewport_width / (float)cfg->viewport_height
                : (16.0f / 9.0f);
        const jce_mat4 oc_p  = jce_camera_proj(camera, oc_aspect,
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

    /* LOD far-cull FLOOR for world-streamed scenes.  Streaming loads/unloads by
     * camera->chunk-CENTER distance (unload_radius), so an entity in a resident
     * chunk can sit up to ~(unload_radius + chunk_half_diagonal) from the
     * camera.  If a per-entity LODGroup cull distance is shorter than that, the
     * chunk stays loaded but the color pass culls the entity — a stable,
     * camera-following band where streamed objects "disappear at certain
     * distances" (and cast ghost shadows, since shadow/velocity passes don't LOD
     * cull).  Clamp every LOD level distance up to this floor so nothing is
     * LOD-culled while its chunk is still resident.  0 (no clamp) when the scene
     * isn't streaming. */
    float sr_lod_far_floor = 0.0f;
    {
        const JceSceneStreamingSettings *lst = jce_scene_get_streaming_settings(scene);
        if (lst && lst->enabled && lst->chunk_count > 0) {
            /* Streaming already bounds WHICH entities are resident (by camera→
             * chunk distance), so a per-entity LOD far-cull by camera distance is
             * redundant AND wrong: when the user pulls the camera back to survey,
             * every loaded object would exceed the cull distance and vanish.
             * Disable the far-cull for streamed scenes — render all resident
             * objects at any camera distance (instancing keeps it cheap). */
            sr_lod_far_floor = 1.0e9f;
        }
    }

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
        if (sr->transparent_queue) jce_rq_clear(sr->transparent_queue);
        sr->frame_view_id = view_id;
        sr->frame_scene = scene;
    }

    /* Camera view matrix — used to compute real view-space depth so the
     * transparent queue can sort back-to-front (farthest drawn first). */
    const jce_mat4 cam_view_mat = camera ? jce_camera_view(camera)
                                         : jce_m4_identity();

    /* Arm the Forward+ per-submit bind hook for jce_model_draw (skinned / LOD /
     * static glTF models) ONLY when the clustered path is active this frame.
     * The hook re-binds stage 14 = s_cluster + the cluster uniforms before each
     * primitive submit so multi-primitive characters are fully lit by clustered
     * point/spot lights — the mesh paths get the same bind inline (see
     * sr_bind_material_cb / the inline PBR submit).  Disarmed after the loop so
     * it never leaks into later passes.  When fp_active_frame is false the hook
     * is never installed -> byte-identical to today. */
    if (sr->fp_active_frame)
        jce_model_set_pre_submit_cb(sr_model_fwdplus_presubmit_cb, sr);

    /* Flush any shadow-pass instancing batch left pending by the last shadow
     * view (cascades/lights auto-flush on view change; this drains the final
     * one before the color pass reuses inst_gather). */
    if (sr->sh_batch_count > 0)
        sr_sh_flush(sr, sr->sh_batch_view);

    /* Reset the per-model GPU-instancing batch; batchable static glTF entities
     * accumulate during the loop and flush once after it. */
    sr->inst_batch_count = 0;

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;

        /* ── Terrain path (P1-terrain-lod) ───────────────────────────
         * Terrain is drawn chunk-by-chunk with its OWN per-chunk frustum
         * culling + distance LOD, so it bypasses the entity-level cull
         * (whose position+scale AABB is meaningless for a large heightmap
         * whose origin may sit off-screen).  Preserves the legacy priority:
         * a resolvable MeshRenderer mesh still wins over the terrain fallback. */
        if (cfg->draw_opaque && jce_scene_has_terrain(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TERRAIN) &&
            sr_resolve_mesh(sr, jce_scene_has_mesh_renderer(scene, e)
                                ? jce_scene_get_mesh_renderer(scene, e)
                                : NULL) == NULL) {
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
            if (tc && tc->visible && tc->terrain_path[0] &&
                jce_scene_has_transform(scene, e)) {
                int tslot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
                if (tslot >= 0) {
                    jce_mat4 tmodel = jce_scene_get_world_matrix(scene, e);
                    JcePbrMaterial tpbr = jce_pbr_material_default();
                    bgfx_texture_handle_t tlayers[4];
                    for (int li = 0; li < 4; li++) tlayers[li] = sr->white_tex;
                    for (int li = 0; li < 4; li++) {
                        if (!tc->layer_albedo_path[li][0]) continue;
                        JceTexture lt =
                            sr_resolve_texture(sr, tc->layer_albedo_path[li]);
                        if (jce_texture_valid(lt)) tlayers[li].idx = lt.idx;
                    }
                    sr_draw_terrain_chunks(sr, scene, list, camera, view_id,
                                           tslot, tc, &tmodel, &tpbr, tlayers);
                }
            }
            continue;  /* terrain fully handled (or skipped) */
        }

        /* ── Tilemap path (P5-tilemap) ───────────────────────────────
         * Drawn chunk-by-chunk with its OWN per-chunk frustum culling
         * (the entity-level position+scale AABB is meaningless for a
         * large grid whose origin may sit off-screen), so like terrain
         * it bypasses the generic visibility check. */
        if (cfg->draw_sprites && jce_scene_has_tilemap(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TILEMAP)) {
            JceTilemapComponent *tmc = jce_scene_get_tilemap(scene, e);
            if (tmc && tmc->visible && tmc->tilemap_path[0] &&
                jce_scene_has_transform(scene, e)) {
                int tslot = sr_tilemap_find_or_load_slot(sr, tmc);
                if (tslot >= 0) {
                    if (tmc->orientation != 0 &&
                        !sr->tilemap_cache[tslot].warned_iso) {
                        LOG_WARN(LOG_TAG, "tilemap '%s': isometric "
                                 "orientation not implemented; rendering "
                                 "orthogonal", tmc->tilemap_path);
                        sr->tilemap_cache[tslot].warned_iso = true;
                    }
                    sr_tilemap_build_chunks(sr, tslot,
                                            sr_tilemap_color_abgr(tmc->color));
                    jce_mat4 tmodel = jce_scene_get_world_matrix(scene, e);
                    sr_draw_tilemap_chunks(sr, tslot, camera, view_id,
                                           &tmodel);
                }
                continue;  /* tilemap fully handled (or skipped) */
            }
        }

        /* ── Vegetation scatter (P0 foliage) ─────────────────────────
         * The scatter area can be large, so (like terrain/tilemap) it
         * bypasses the per-entity visibility AABB. */
        if (cfg->draw_opaque && jce_scene_has_vegetation_scatter(scene, e)) {
            static int s_veg_cid = -2;
            if (s_veg_cid == -2) s_veg_cid = jce_component_find("VegetationScatter");
            if (s_veg_cid < 0 || jce_scene_comp_enabled(scene, e, s_veg_cid))
                sr_draw_foliage(sr, scene, list, e, view_id);
            continue;
        }

        /* ── Water surface (Gerstner, roadmap 2.3) ───────────────────
         * Translucent animated plane.  Sized to size_x*size_z and centred
         * on the entity, so (like terrain/foliage) it bypasses the per-
         * entity visibility AABB.  Drawn alpha-blended in the color view. */
        if (cfg->draw_transparent && jce_scene_has_water(scene, e)) {
            static int s_water_cid = -2;
            if (s_water_cid == -2) s_water_cid = jce_component_find("Water");
            if (s_water_cid < 0 || jce_scene_comp_enabled(scene, e, s_water_cid))
                sr_draw_water(sr, scene, list, e, view_id);
            continue;
        }

        if (!visible[i]) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;

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

        /* ── Per-entity LOD group (distance cull) ─────────────────────
         * The authored JceLodGroupComponent was previously never consumed by
         * the renderer (only the debug global LOD was).  Honour its far cull
         * here — the common "same mesh, just cull past range" case the
         * component's empty-path convention targets.  jce_lod_pick supplies the
         * hysteresis dead-zone; a non-empty per-level mesh path swaps the
         * drawn model (below).  (Per-entity LOD ranges here; the global debug
         * LOD below is off by default so the shared lod_prev slot is safe.) */
        if (camera && jce_scene_has_lod_group(scene, e)) {
            static int s_lodg_cid = -2;
            if (s_lodg_cid == -2) s_lodg_cid = jce_component_find("LODGroup");
            if (s_lodg_cid < 0 || jce_scene_comp_enabled(scene, e, s_lodg_cid)) {
                JceLodGroupComponent *lg = jce_scene_get_lod_group(scene, e);
                if (lg && lg->level_count > 0) {
                    jce_vec3 cp = jce_camera_get_position(camera);
                    float dx = model.col[3].x - cp.x;
                    float dy = model.col[3].y - cp.y;
                    float dz = model.col[3].z - cp.z;
                    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    JceLodGroup g;
                    jce_lod_init(&g);
                    int n = lg->level_count;
                    if (n > JCE_LOD_MAX_LEVELS) n = JCE_LOD_MAX_LEVELS;
                    g.count = n;
                    g.hysteresis = lg->hysteresis > 0.0f ? lg->hysteresis : 0.05f;
                    for (int li = 0; li < n; ++li) {
                        g.levels[li].mesh = NULL;            /* selection uses distances only */
                        /* Never cull/step closer than the streaming residency
                         * reach, so resident streamed objects always render. */
                        g.levels[li].distance =
                            fmaxf(lg->distances[li], sr_lod_far_floor);
                    }
                    uint32_t lslot = (uint32_t)e & 1023u;
                    int prev = (int)sr->lod_prev[lslot];
                    int lvl = jce_lod_pick(&g, dist, prev > 0 ? prev - 1 : -1);
                    if (lvl < 0) {
                        sr->lod_prev[lslot] = (int8_t)g.count;
                        if (lg->cull_when_too_far) continue;  /* beyond range → cull */
                    } else {
                        sr->lod_prev[lslot] = (int8_t)(lvl + 1);
                        /* Distinct per-level mesh: draw that model in place of
                         * the base.  Empty path = reuse base (fall through).
                         * Skinned entities keep their rig (skip the static
                         * swap).  Shadow pass still uses the base mesh. */
                        const char *lpath = lg->level_mesh_paths[lvl];
                        if (lpath[0] && !jce_scene_has_skeletal_animator(scene, e)) {
                            SrModelCache *lmc = sr_get_model(sr, lpath, (uint32_t)e);
                            if (lmc && lmc->model) {
                                jce_model_draw(lmc->model, sr->renderer,
                                               view_id, &model, NULL, 0);
                                continue;  /* drew LOD level model; skip base */
                            }
                        }
                    }
                }
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
        /* Pose was already advanced ONCE this frame by
           sr_update_skinned_anims (before the shadow pass); here we only
           consume the cached palette so the lit mesh and its cast shadow
           share the exact same pose.  skin_palette_count == 0 => bind pose. */
        if (jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
            if (sa && sa->skeleton_path[0]) {
                SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
                if (mc && mc->model) {
                    SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
                    const jce_mat4 *pal =
                        (ai && ai->skin_palette_count > 0) ? ai->skin_palette : NULL;
                    uint32_t pal_n = ai ? ai->skin_palette_count : 0;
                    /* FEATURE 3.1: when this instance has live morph VBs, route
                     * through the morph-aware draw so the deformed per-instance
                     * verts are bound; else the plain draw (byte-identical). */
                    if (ai && ai->morph_vb_count > 0)
                        jce_model_draw_morphed(mc->model, sr->renderer, view_id,
                                               &model, pal, pal_n,
                                               sr_morph_vb_cb, ai);
                    else
                        jce_model_draw(mc->model, sr->renderer, view_id, &model,
                                       pal, pal_n);
                    continue;
                }
            }
        }

        if (cfg->draw_opaque &&
            !jce_scene_has_skeletal_animator(scene, e) &&
            sr_try_draw_mesh_renderer_model(sr, scene, e, view_id, &model, cfg)) {
            continue;
        }

        /* ── Sprite animator path (2D, P1 #16) ───────────────────── */
        /* Frame time was advanced once this frame by sr_update_sprite_anims;
           here we consume the cached player's current frame, convert its pixel
           rect to a UV sub-rect, and submit the quad to the sprite batch. */
        if (cfg->draw_sprites && sr->sprite_batch &&
            jce_scene_has_sprite_animator(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_ANIMATOR)) {
            int slot = sr_find_sprite_anim(sr, (uint32_t)e);
            JceSpritePlayer *pl =
                (slot >= 0) ? sr->sprite_anim[slot].player : NULL;
            const JceSpriteSheet *sheet =
                (slot >= 0) ? sr->sprite_anim[slot].sheet : NULL;
            const JceSpriteFrame *fr =
                pl ? jce_sprite_player_current_frame(pl) : NULL;
            if (fr) {
                JceSpriteAnimatorComponent *sac =
                    jce_scene_get_sprite_animator(scene, e);
                /* Resolve the sheet image: the component's explicit image path
                   wins; fall back to the atlas's meta.image. */
                const char *img = (sac && sac->sheet_path[0])
                    ? sac->sheet_path
                    : (sheet ? jce_sprite_sheet_image_path(sheet) : NULL);
                bgfx_texture_handle_t st = { UINT16_MAX };
                uint32_t iw = 0, ih = 0;
                if (img && img[0]) {
                    JceTexture t = sr_resolve_texture(sr, img);
                    if (jce_texture_valid(t)) {
                        st.idx = t.idx;
                        jce_texture_get_size(t, &iw, &ih);
                    }
                }
                if (!BGFX_HANDLE_IS_VALID(st)) { st = sr->white_tex; iw = ih = 1; }

                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                if (iw > 0 && ih > 0 && fr->w > 0 && fr->h > 0) {
                    u0 = (float)fr->x / (float)iw;
                    v0 = (float)fr->y / (float)ih;
                    u1 = (float)(fr->x + fr->w) / (float)iw;
                    v1 = (float)(fr->y + fr->h) / (float)ih;
                }

                /* SpriteAnimator has no tint/sort fields: white tint, sort 0. */
                JceTexture sjt; sjt.idx = st.idx;
                jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                     u0, v0, u1, v1, 0xFFFFFFFFu, 0);
                continue;
            }
        }

        /* ── Sprite path ─────────────────────────────────────────── */
        if (cfg->draw_sprites && sr->sprite_batch &&
            jce_scene_has_sprite_renderer(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_RENDERER)) {
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
                pbr.receive_shadows_off = mr_comp->shadow_receive_off;
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
                /* Editor + missing albedo → pink-black checker hint ONLY in the
                 * TEXTURED debug view, so the normal SHADED view renders an
                 * untextured mesh with its lit base color (the checker is a
                 * texture-coverage debug aid, not a default look). */
                if (editor_mode &&
                    cfg->view_mode == JCE_SCENE_VIEW_TEXTURED &&
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
                /* SSAO override (screen-space AO into the AO stage; see the
                 * other material-build path). */
                if (sr->ssao_active_frame && sr->ssao_ao_idx != UINT16_MAX) {
                    JceTexture st; st.idx = sr->ssao_ao_idx;
                    pbr.ao_map = st;
                }
            }

            /* VideoPlayer-as-texture: a live decoded frame (uploaded by
             * jce_scene_video_update) overrides the entity's albedo so a
             * mesh plays the clip.  Drives both editor preview and runtime. */
            if (jce_scene_has_video_player(scene, e)) {
                JceVideoPlayerComponent *vp =
                    jce_scene_get_video_player(scene, e);
                { static int s_vp_cid = -2;
                  if (s_vp_cid == -2) s_vp_cid = jce_component_find("VideoPlayer");
                  if (vp && s_vp_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_vp_cid)) vp = NULL; }
                if (vp && jce_texture_valid(vp->output_tex)) {
                    pbr.albedo_map = vp->output_tex;
                    use_checker_fallback = false;
                }
            }

            /* Encode the checker-fallback flag in normal_scale sign (the
             * fs_pbr.sc shader checks `u_normalScale.x < 0`). */
            if (use_checker_fallback)
                pbr.normal_scale = -fmaxf(fabsf(pbr.normal_scale), 0.0001f);
            else
                pbr.normal_scale = fabsf(pbr.normal_scale);

            /* Terrain is handled earlier by the dedicated per-chunk path
             * (sr_draw_terrain_chunks); a terrain entity never reaches here. */

            /* Material render state: blend (alpha BLEND), depth-write, and
             * back-face cull (double_sided drops CULL_CW).  Shared by the
             * queue and inline paths so both render transparency / two-
             * sided geometry identically. */
            const uint64_t mat_state = jce_pbr_material_render_state(&pbr);
            const bool is_transparent = jce_pbr_material_is_transparent(&pbr);

            /* Shader Graph custom shader: a .mat.json may persist a graph-
             * generated program (customProgramVs/Fs).  When present, route
             * the mesh through it instead of the default PBR program.  Custom
             * programs are not instanced, so we force single-submit. */
            JceShaderHandle custom_prog = mr_comp
                ? sr_resolve_custom_program(sr, mr_comp->material_path)
                : (JceShaderHandle){ UINT16_MAX };
            const bool has_custom = (custom_prog.idx != UINT16_MAX);

            /* ── Queue path: mesh entities go through the render queue →
             *     auto-batched instanced submit. Alpha-blend materials route
             *     to the back-to-front transparent queue. */
            if (use_rq) {
                uint32_t mat_key = sr_compute_material_key(&pbr, false, -1, NULL);
                uint32_t reg_key = sr_register_material(sr, mat_key, &pbr,
                                                        false, -1, 0.0f, false, NULL);
                JceRenderQueue *target_q = is_transparent
                    ? sr->transparent_queue : sr->render_queue;
                if (reg_key && target_q) {
                    JceDrawCmd cmd;
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.view_id      = view_id;
                    if (has_custom) {
                        /* Single-submit only: no instance variant for graph
                         * shaders.  program_single drives the n=1 path. */
                        cmd.program        = custom_prog.idx;
                        cmd.program_single = custom_prog.idx;
                    } else {
                        cmd.program        = (uint16_t)prog_pbr_inst_h.idx;
                        cmd.program_single = (prog_pbr_h.idx != UINT16_MAX)
                                                ? (uint16_t)prog_pbr_h.idx
                                                : (uint16_t)UINT16_MAX;
                    }
                    cmd.mesh_vbh     = jce_mesh_get_vbh(mesh);
                    cmd.mesh_ibh     = jce_mesh_get_ibh(mesh);
                    cmd.index_count  = jce_mesh_index_count(mesh);
                    cmd.transform    = model;
                    /* Real view-space depth for transparent sorting: distance
                     * in front of the camera (larger = farther = drawn first
                     * in back-to-front order).  Opaque draws ignore this. */
                    if (is_transparent) {
                        jce_vec4 wp = jce_v4(model.col[3].x, model.col[3].y,
                                             model.col[3].z, 1.0f);
                        jce_vec4 vp = jce_m4_mul_v4(&cam_view_mat, wp);
                        cmd.depth    = -vp.z;
                    } else {
                        cmd.depth    = 0.0f;
                    }
                    cmd.material_key = reg_key;
                    cmd.state        = mat_state;
                    jce_rq_push(target_q, &cmd);
                    continue;  /* handled by queue flush below */
                }
                /* Registry overflow → fall through to inline path below. */
            }

            /* ── Inline path (queue overflow or queue disabled) ── */
            sr_inline_bind_pbr_global(sr, &pbr, view_id, scene, list);
            if (has_custom)
                jce_mesh_submit_pbr_with_program(mesh, sr->renderer,
                                                 view_id, custom_prog);
            else {
                /* Forward+ cluster bind (stage 14 = s_cluster + uniforms) only
                 * for the default PBR program path (a custom graph shader has
                 * no s_cluster).  jce_mesh_submit_pbr_state pulls the program
                 * from jce_renderer_get_program_pbr, which returns the
                 * fs_pbr_fwdplus variant while fp_active_frame is set. */
                if (sr->fp_active_frame)
                    jce_forwardplus_bind(sr->forwardplus);
                jce_mesh_submit_pbr_state(mesh, sr->renderer, view_id, mat_state);
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

    /* Flush the per-model GPU-instancing batch (opaque) — one instanced submit
     * per model, before the transparent queue so opaque depth is laid down
     * first.  The Forward+ pre-submit hook is still armed here. */
    sr_inst_flush(sr, view_id);

    /* ── Render-queue flush (Phase 3) ─────────────────────────────────
     * Issues auto-batched instanced submits for all PBR mesh entities
     * pushed during the loop. The binder rebinds material textures /
     * uniforms once per material run. */
    if (use_rq && jce_rq_count(sr->render_queue) > 0) {
        jce_rq_set_material_binder(sr->render_queue, sr_bind_material_cb, sr);
        jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
        sr_rq_flush_and_collect(sr);
    }

    /* ── Transparent queue flush ───────────────────────────────────────
     * Alpha-blended materials, sorted back-to-front by real view-space
     * depth, flushed AFTER the opaque pass so they composite over solid
     * geometry in correct order.  The view is in SEQUENTIAL submit mode,
     * so this submission order is preserved verbatim by bgfx.  Batching is
     * disabled on this queue (see jce_rq_set_no_batch in init) so per-draw
     * blend order stays exact. */
    if (use_rq && sr->transparent_queue &&
        jce_rq_count(sr->transparent_queue) > 0) {
        jce_rq_set_material_binder(sr->transparent_queue,
                                   sr_bind_material_cb, sr);
        jce_rq_sort(sr->transparent_queue, JCE_SORT_BACK_TO_FRONT);
        sr_rq_flush_queue_and_collect(sr, sr->transparent_queue);
    }

    /* Flush sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites &&
        jce_sprite_batch_count(sr->sprite_batch) > 0)
        jce_sprite_batch_flush(sr->sprite_batch, sr->renderer, view_id);

    /* Disarm the jce_model_draw pre-submit hook so the cluster bind never
     * fires for model draws outside this color pass (pick pass, preview,
     * wireframe overlays).  Unconditional clear: cheap and keeps the global
     * hook state deterministic even if it was never armed this frame. */
    jce_model_set_pre_submit_cb(NULL, NULL);

    /* Clear the renderer's Forward+ program-swap flag so it never leaks into
     * later passes / consumers (pick pass, preview, particles) that call the
     * PBR getters but do NOT bind the cluster texture.  Recomputed next frame
     * at the top of the entity walk. */
    if (sr->renderer)
        jce_renderer_set_forwardplus_program_active(sr->renderer, false);
    sr->fp_active_frame = false;
}
