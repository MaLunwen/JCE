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

/* ── Per-entity LOD hysteresis state (large-world-opt P1 #6) ────────────────
 * Open-addressed (linear-probe) table keyed by entity id, replacing the former
 * fixed int8_t[1024] mask that aliased ~50× at 53k entities.  Stored value is
 * (level + 1); 0 == no record.  Grown on demand (load factor < 0.5); pruned to
 * the live touched set so a streaming world can't grow it without bound. */
static uint32_t sr_lod_state_hash(uint32_t e) { return e * 2654435761u; }

/* Return the stored value (level+1; 0 = no record) for `entity`, marking it
 * touched so the prune keeps it.  Never grows. */
static uint8_t sr_lod_state_get(JceSceneRenderer *sr, uint32_t entity)
{
    if (!sr->lod_state || sr->lod_state_cap == 0 || entity == 0) return 0;
    uint32_t mask = sr->lod_state_cap - 1;
    uint32_t h = sr_lod_state_hash(entity) & mask;
    for (uint32_t i = 0; i < sr->lod_state_cap; i++) {
        SrLodStateEntry *en = &sr->lod_state[(h + i) & mask];
        if (en->entity == 0) return 0;          /* empty slot ends the probe */
        if (en->entity == entity) { en->touched = true; return en->value; }
    }
    return 0;
}

static bool sr_lod_state_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t new_cap = sr->lod_state_cap ? sr->lod_state_cap : 1024;
    while (new_cap < need * 2u) new_cap *= 2u;   /* load factor < 0.5 */
    SrLodStateEntry *ns =
        (SrLodStateEntry *)JCE_CALLOC(new_cap, sizeof(SrLodStateEntry));
    if (!ns) return false;
    if (sr->lod_state) {
        uint32_t nmask = new_cap - 1;
        for (uint32_t i = 0; i < sr->lod_state_cap; i++) {
            SrLodStateEntry *o = &sr->lod_state[i];
            if (o->entity == 0) continue;
            uint32_t h = sr_lod_state_hash(o->entity) & nmask;
            while (ns[h].entity != 0) h = (h + 1) & nmask;
            ns[h] = *o;
        }
        JCE_FREE(sr->lod_state);
    }
    sr->lod_state = ns;
    sr->lod_state_cap = new_cap;
    return true;
}

/* Insert/overwrite the stored value for `entity` (value = level+1). */
static void sr_lod_state_set(JceSceneRenderer *sr, uint32_t entity, uint8_t value)
{
    if (entity == 0) return;
    if (sr->lod_state_cap == 0 ||
        (sr->lod_state_count + 1) * 2u >= sr->lod_state_cap) {
        if (!sr_lod_state_grow(sr, sr->lod_state_count + 1)) return;
    }
    uint32_t mask = sr->lod_state_cap - 1;
    uint32_t h = sr_lod_state_hash(entity) & mask;
    while (sr->lod_state[h].entity != 0 && sr->lod_state[h].entity != entity)
        h = (h + 1) & mask;
    SrLodStateEntry *en = &sr->lod_state[h];
    if (en->entity == 0) { en->entity = entity; sr->lod_state_count++; }
    en->value   = value;
    en->touched = true;
}

/* Drop entries not referenced since the last prune so the table tracks the
 * active streamed set.  Called once per render after the LOD compute pass. */
void sr_lod_state_prune(JceSceneRenderer *sr)
{
    if (!sr->lod_state) return;
    for (uint32_t i = 0; i < sr->lod_state_cap; i++) {
        SrLodStateEntry *en = &sr->lod_state[i];
        if (en->entity == 0) continue;
        if (!en->touched) { *en = (SrLodStateEntry){0}; sr->lod_state_count--; }
        else en->touched = false;   /* reset window for next frame */
    }
}

/* ── Per-entity streaming-fade state (Direction B; dithered detail fade-in) ─
 * Parallel open-addressed table mirroring lod_state above (same hash, growth
 * load factor < 0.5, and live-set prune): records the renderer phase-clock time
 * (sr->fade_time) at which an entity was FIRST seen in the color pass so the
 * dithered cross-fade can ramp (now - first_seen) / JCE_SR_FADE_DURATION.
 * Entirely inert until the first draw records a first-seen time. */
static bool sr_fade_state_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t new_cap = sr->fade_state_cap ? sr->fade_state_cap : 1024;
    while (new_cap < need * 2u) new_cap *= 2u;   /* load factor < 0.5 */
    SrFadeStateEntry *ns =
        (SrFadeStateEntry *)JCE_CALLOC(new_cap, sizeof(SrFadeStateEntry));
    if (!ns) return false;
    if (sr->fade_state) {
        uint32_t nmask = new_cap - 1;
        for (uint32_t i = 0; i < sr->fade_state_cap; i++) {
            SrFadeStateEntry *o = &sr->fade_state[i];
            if (o->entity == 0) continue;
            uint32_t h = sr_lod_state_hash(o->entity) & nmask;
            while (ns[h].entity != 0) h = (h + 1) & nmask;
            ns[h] = *o;
        }
        JCE_FREE(sr->fade_state);
    }
    sr->fade_state = ns;
    sr->fade_state_cap = new_cap;
    return true;
}

/* Return this entity's normalized fade-in factor in [0,1] (1 = fully present),
 * recording the first-seen time on first call.  Marks the entry touched so the
 * prune keeps it.  Returns 1.0 (no fade) when the phase clock is not advancing
 * (still editor preview, fade_time == 0): nothing is streaming, so a static
 * scene renders byte-identically. */
float sr_fade_factor_for_entity(JceSceneRenderer *sr, uint32_t entity)
{
    if (entity == 0) return 1.0f;
    /* Clock not advancing (still editor) => never fade.  fade_time only grows
     * while dt_sec>0 (Play / streaming), so this is the static-scene fast exit. */
    if (sr->fade_time <= 0.0f) return 1.0f;

    if (sr->fade_state_cap == 0 ||
        (sr->fade_state_count + 1) * 2u >= sr->fade_state_cap) {
        if (!sr_fade_state_grow(sr, sr->fade_state_count + 1)) return 1.0f;
    }
    uint32_t mask = sr->fade_state_cap - 1;
    uint32_t h = sr_lod_state_hash(entity) & mask;
    while (sr->fade_state[h].entity != 0 && sr->fade_state[h].entity != entity)
        h = (h + 1) & mask;
    SrFadeStateEntry *en = &sr->fade_state[h];
    if (en->entity == 0) {
        /* First sighting this session: stamp the start of the fade. */
        en->entity = entity;
        en->first_seen = sr->fade_time;
        sr->fade_state_count++;
    }
    en->touched = true;
    float age = sr->fade_time - en->first_seen;
    if (age <= 0.0f) return 0.0f;                 /* just appeared this frame */
    if (age >= JCE_SR_FADE_DURATION) return 1.0f; /* done fading */
    return age / JCE_SR_FADE_DURATION;
}

/* Drop entries not referenced since the last prune so a despawned (then later
 * respawned) cell re-fades and the table tracks the active streamed set. */
void sr_fade_state_prune(JceSceneRenderer *sr)
{
    if (!sr->fade_state) return;
    for (uint32_t i = 0; i < sr->fade_state_cap; i++) {
        SrFadeStateEntry *en = &sr->fade_state[i];
        if (en->entity == 0) continue;
        if (!en->touched) { *en = (SrFadeStateEntry){0}; sr->fade_state_count--; }
        else en->touched = false;   /* reset window for next frame */
    }
}

/* Compute and CACHE this entity's in-asset auto-LOD level for the frame
 * (large-world-opt P1 #6).  Runs ONCE in the ecull build loop, before both the
 * shadow and color passes, so they bind the SAME level (shadow silhouette
 * matches the rendered LOD).  Writes ecull[cull_idx].lod_level (0 = base / LOD0;
 * N = the (N-1)'th reduced index set) and .lod_culled (LODGroup far-cull fired).
 *
 * Selection uses the authored JceLodGroupComponent distances + hysteresis via
 * jce_lod_pick, with the per-entity hysteresis state above.  The component's
 * empty-meshPath convention now AUTO-BINDS the in-asset cooked LOD chain: a
 * level with no override mesh path draws the entity's own model at that LOD
 * level (no separate .glb needed).  A non-empty per-level path still swaps the
 * whole model in the color pass (handled there); here we only record the level.
 *
 * No LODGroup => lod_level 0, lod_culled false (byte-identical to pre-LOD). */
void sr_compute_entity_lod(JceSceneRenderer *sr, JceScene *scene,
                           const JceCamera *camera, JceEntity e, int cull_idx)
{
    if (cull_idx < 0 || !sr->ecull) return;
    sr->ecull[cull_idx].lod_level  = 0;
    sr->ecull[cull_idx].lod_culled = false;
    sr->ecull[cull_idx].lod_fade   = 1.0f;   /* steady (no transition) */
    if (!camera || !jce_scene_has_lod_group(scene, e)) return;

    static int s_lodg_cid = -2;
    if (s_lodg_cid == -2) s_lodg_cid = jce_component_find("LODGroup");
    if (s_lodg_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_lodg_cid)) return;

    JceLodGroupComponent *lg = jce_scene_get_lod_group(scene, e);
    if (!lg || lg->level_count <= 0) return;

    /* Streaming bounds residency by camera→chunk distance, so a per-entity LOD
     * far-cull by camera distance is redundant + wrong there (pulling back to
     * survey would vanish everything).  Clamp every level distance up to a huge
     * floor in streamed scenes so nothing LOD-culls while resident. */
    float far_floor = 0.0f;
    {
        const JceSceneStreamingSettings *lst =
            jce_scene_get_streaming_settings(scene);
        if (lst && lst->enabled && lst->chunk_count > 0) far_floor = 1.0e9f;
    }

    const jce_mat4 *w = sr->ecull[cull_idx].world_valid
                          ? &sr->ecull[cull_idx].world : NULL;
    jce_vec3 cp = jce_camera_get_position(camera);
    float ex = w ? w->col[3].x : 0.0f;
    float ey = w ? w->col[3].y : 0.0f;
    float ez = w ? w->col[3].z : 0.0f;
    if (!w) {
        /* No cached world: fall back to the transform position. */
        if (jce_scene_has_transform(scene, e)) {
            JceTransform *xf = jce_scene_get_transform(scene, e);
            if (xf) { ex = xf->position.x; ey = xf->position.y; ez = xf->position.z; }
        }
    }
    float dx = ex - cp.x, dy = ey - cp.y, dz = ez - cp.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    JceLodGroup g;
    jce_lod_init(&g);
    int n = lg->level_count;
    if (n > JCE_LOD_MAX_LEVELS) n = JCE_LOD_MAX_LEVELS;
    g.count = n;
    g.hysteresis = lg->hysteresis > 0.0f ? lg->hysteresis : 0.05f;
    for (int li = 0; li < n; ++li) {
        g.levels[li].mesh = NULL;            /* selection uses distances only */
        g.levels[li].distance = fmaxf(lg->distances[li], far_floor);
    }

    uint8_t rec = sr_lod_state_get(sr, (uint32_t)e);
    int prev = rec > 0 ? (int)rec - 1 : -1;
    int lvl = jce_lod_pick(&g, dist, prev);
    if (lvl < 0) {
        sr_lod_state_set(sr, (uint32_t)e, (uint8_t)(g.count + 1));  /* park last */
        if (lg->cull_when_too_far) sr->ecull[cull_idx].lod_culled = true;
        /* Not culling: pin to the last authored level. */
        sr->ecull[cull_idx].lod_level = (uint8_t)g.count;
        return;
    }
    sr_lod_state_set(sr, (uint32_t)e, (uint8_t)(lvl + 1));
    if (lvl < JCE_LOD_MAX_LEVELS) sr->stat_lod_picks[lvl]++;
    /* level index lvl maps to in-asset LOD: level 0 = base (LOD0), so the
     * draw-LOD passed to the model is `lvl` directly (0 = base). */
    sr->ecull[cull_idx].lod_level = (uint8_t)lvl;

    /* Cross-fade band (large-world #6): as the entity approaches the step-down
     * threshold to lvl+1, fade the outgoing level out over fade_width world-units
     * BEFORE the hard snap.  lod_fade = fraction of lvl to keep (1 far from the
     * switch → 0 at it).  Only when a next level exists + fade_width authored. */
    if (lvl < g.count - 1 && lg->fade_width > 0.0f) {
        float thr = g.levels[lvl].distance;
        if (thr < far_floor * 0.5f) {           /* skip the streamed far-floor */
            float a = (thr - dist) / lg->fade_width;
            sr->ecull[cull_idx].lod_fade = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        }
    }
}

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
                            const jce_mat4 *world, uint16_t lod);
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

    /* In-asset auto-LOD (P1 #6): cast the SAME reduced silhouette the color pass
     * will draw (precomputed in the ecull build loop), so a mid-distance
     * object's shadow uses the same LOD as its render — no peter-panning from a
     * full-detail shadow under a reduced mesh.  An entity whose LODGroup uses a
     * distinct per-level override mesh is drawn by the color pass via that other
     * model; for the shadow we still cast the base model's silhouette at this
     * level (close enough; the override mesh swap is the rarer authoring path). */
    uint16_t lod = (cull_idx >= 0 && sr->ecull)
                     ? (uint16_t)sr->ecull[cull_idx].lod_level : 0;

    /* GPU-instancing fast path (mirrors the color pass): batch instanceable
     * static glb casters per shadow view, flushing when the target view changes
     * (and once before the color pass), so a cascade's thousands of repeated
     * buildings collapse to ~unique-mesh depth submits — the full-load shadow
     * bottleneck.  Skinned models are not instanceable and fall through.  The
     * batch is keyed by (model, lod) so LOD'd casters still instance. */
    if (jce_model_is_instanceable(mc->model)) {
        if (sr->sh_batch_count > 0 && view_id != sr->sh_batch_view)
            sr_sh_flush(sr, sr->sh_batch_view);
        sr->sh_batch_view = view_id;
        if (sr_sh_batch_add(sr, mc->model, &model, lod))
            return true;
    }

    jce_model_set_draw_lod(lod);
    jce_model_draw_shadow(mc->model, sr->renderer, view_id, &model, NULL, 0);
    jce_model_set_draw_lod(0);
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
/* `tint` (RGBA, linear) is the per-instance baseColor tint; pass NULL for the
 * no-tint common case ([1,1,1,1]).  A non-white tint keeps the entity in the
 * instanced batch (carried to vs_pbr_inst_tint's i_data4) instead of forcing a
 * solo draw (large-world-opt P1 #7). */
static bool sr_inst_batch_add(JceSceneRenderer *sr, JceModel *model,
                              const jce_mat4 *world, uint16_t lod,
                              const float tint[4])
{
    if (sr->inst_batch_count >= sr->inst_batch_cap) {
        uint32_t nc = sr->inst_batch_cap ? sr->inst_batch_cap * 2u : 256u;
        SrInstEntry *nb = (SrInstEntry *)JCE_REALLOC(
            sr->inst_batch, (size_t)nc * sizeof(SrInstEntry));
        if (!nb) return false;
        sr->inst_batch = nb;
        sr->inst_batch_cap = nc;
    }
    SrInstEntry *e = &sr->inst_batch[sr->inst_batch_count];
    e->model = model;
    e->world = *world;
    e->lod   = lod;
    if (tint) {
        e->tint[0] = tint[0]; e->tint[1] = tint[1];
        e->tint[2] = tint[2]; e->tint[3] = tint[3];
    } else {
        e->tint[0] = e->tint[1] = e->tint[2] = e->tint[3] = 1.0f;
    }
    sr->inst_batch_count++;
    return true;
}

/* Sort by (model, lod) so a run is a single model AT a single LOD — every copy
 * in the run binds the same index set and collapses into one instanced submit
 * (LOD'd instanced meshes still batch; the draw count doesn't explode). */
static int sr_inst_cmp(const void *a, const void *b)
{
    const SrInstEntry *ea = (const SrInstEntry *)a;
    const SrInstEntry *eb = (const SrInstEntry *)b;
    uintptr_t ma = (uintptr_t)ea->model, mb = (uintptr_t)eb->model;
    if (ma != mb) return (ma > mb) - (ma < mb);
    return (ea->lod > eb->lod) - (ea->lod < eb->lod);
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

    /* The indirect path needs this run's mesh index count so the GPU can write
     * the drawIndexedIndirect numIndices.  (Ignored in the 1:1 fallback.) */
    uint32_t num_indices = jce_model_gpu_index_count(m);

    JceGpuSceneRun gpu_run = { 0, UINT32_MAX };
    if (!jce_gpu_scene_add_run(sr->gpu_scene, sr->gpu_rec, run, num_indices,
                               &gpu_run))
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
    sr->gpu_draw[sr->gpu_draw_count].base  = gpu_run.run_base;  /* partition base */
    sr->gpu_draw[sr->gpu_draw_count].src   = base;      /* inst_batch index   */
    sr->gpu_draw[sr->gpu_draw_count].count = run;
    sr->gpu_draw[sr->gpu_draw_count].indirect_el = gpu_run.indirect_el;
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

    /* Pass 1: per (model, lod) run, either queue it into the GPU cull batch or
     * draw it immediately via the CPU instanced path (ineligible / gpu off /
     * lod>0 / tinted).  A run is one model AT one LOD, so every copy binds the
     * same index set and STILL collapses to a single instanced submit. */
    uint32_t i = 0;
    while (i < sr->inst_batch_count) {
        JceModel *m   = sr->inst_batch[i].model;
        uint16_t  lod = sr->inst_batch[i].lod;
        uint32_t run = 0;
        bool run_tinted = false;   /* any instance in this run carries a tint */
        while (i + run < sr->inst_batch_count &&
               sr->inst_batch[i + run].model == m &&
               sr->inst_batch[i + run].lod   == lod) {
            const float *t = sr->inst_batch[i + run].tint;
            if (t[0] != 1.0f || t[1] != 1.0f || t[2] != 1.0f || t[3] != 1.0f)
                run_tinted = true;
            run++;
        }
        /* The GPU-driven from-buffer draw sources a mat4-only buffer (no tint
         * stream) and does not consult the LOD selector, so route LOD'd runs
         * (lod>0) AND tinted runs (large-world-opt P1 #7) through the CPU
         * instanced path: it binds the reduced index set via
         * jce_model_set_draw_lod and feeds the per-instance i_data4 tint stream.
         * Base, untinted runs (the common case) keep the GPU-driven fast path. */
        if (lod == 0 && !run_tinted && gpu && sr_gpu_add_run(sr, m, i, run)) {
            /* queued for the GPU pass-2 draw */
        } else {
            for (uint32_t k = 0; k < run; k++)
                sr->inst_gather[k] = sr->inst_batch[i + k].world;
            if (lod > 0) jce_model_set_draw_lod(lod);
            if (run_tinted) {
                /* Gather the per-instance tints in lockstep with the matrices,
                 * then submit ONE instanced draw carrying the i_data4 tint
                 * stream — tinted copies batch instead of going solo. */
                if (sr->inst_tint_gather_cap < run) {
                    jce_vec4 *nt = (jce_vec4 *)JCE_REALLOC(
                        sr->inst_tint_gather, (size_t)run * sizeof(jce_vec4));
                    if (nt) { sr->inst_tint_gather = nt; sr->inst_tint_gather_cap = run; }
                }
                if (sr->inst_tint_gather_cap >= run) {
                    for (uint32_t k = 0; k < run; k++) {
                        const float *t = sr->inst_batch[i + k].tint;
                        sr->inst_tint_gather[k].x = t[0];
                        sr->inst_tint_gather[k].y = t[1];
                        sr->inst_tint_gather[k].z = t[2];
                        sr->inst_tint_gather[k].w = t[3];
                    }
                    jce_model_draw_instanced_tinted(m, sr->renderer, view_id,
                                                    sr->inst_gather,
                                                    sr->inst_tint_gather, run);
                } else {
                    /* Tint scratch alloc failed: fall back to untinted batch
                     * (drops colour this frame but never crashes / drops draws). */
                    jce_model_draw_instanced(m, sr->renderer, view_id,
                                             sr->inst_gather, run);
                }
            } else {
                jce_model_draw_instanced(m, sr->renderer, view_id, sr->inst_gather, run);
            }
            if (lod > 0) jce_model_set_draw_lod(0);
        }
        i += run;
    }

    /* Pass 2: one cull dispatch, then draw each GPU run from its compacted
     * partition.  If the dispatch fails (e.g. buffer alloc), fall back to a CPU
     * instanced draw of each queued run so nothing is dropped. */
    if (gpu && sr->gpu_draw_count > 0) {
        bool ok = jce_gpu_scene_dispatch(sr->gpu_scene, sr->gpu_reset_view,
                                         sr->gpu_cull_view, sr->gpu_frame_planes);
        uint16_t vis = jce_gpu_scene_visible_vb(sr->gpu_scene);
        /* The indirect buffer is only valid when the dispatch resolved the
         * indirect path (it falls back to the 1:1 cull on a per-frame resource
         * failure, in which case ind == UINT16_MAX and we use the fixed count). */
        uint16_t ind = jce_gpu_scene_indirect_buffer(sr->gpu_scene);
        for (uint32_t d = 0; d < sr->gpu_draw_count; d++) {
            JceModel *m  = sr->gpu_draw[d].model;
            uint32_t bse = sr->gpu_draw[d].base;
            uint32_t src = sr->gpu_draw[d].src;
            uint32_t cnt = sr->gpu_draw[d].count;
            uint32_t iel = sr->gpu_draw[d].indirect_el;
            if (ok && vis != UINT16_MAX && ind != UINT16_MAX &&
                iel != UINT32_MAX) {
                /* True indirect: one draw, survivor count + start from the GPU
                 * args; only survivors rasterise (no degenerate instances). */
                jce_model_draw_indirect_from_buffer(m, sr->renderer, view_id,
                                                    vis, ind, iel);
            } else if (ok && vis != UINT16_MAX) {
                /* 1:1 fixed-count fallback (indirect unavailable this frame). */
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

/* ── Octahedral impostor terminal LOD (roadmap P2 #10) ──────────────────── */

/* Texture loader trampoline for jce_impostor_atlas_load: resolves the atlas
 * PNG through the renderer's normal texture path (editor callback or runtime
 * PAK), so the cooked atlas rides the existing texture cache. */
static JceTexture sr_impostor_tex_load(void *user, const char *rel_path)
{
    return sr_resolve_texture((JceSceneRenderer *)user, rel_path);
}

/* Find-or-load the atlas cache slot for a .impostor.json meta path.  Returns the
 * slot index, or -1 on failure (missing/invalid atlas — cached as failed so it
 * is not retried every frame). */
static int sr_impostor_get_cache(JceSceneRenderer *sr, const char *meta_path)
{
    if (!meta_path || !meta_path[0]) return -1;
    for (int i = 0; i < sr->impostor_cache_count; ++i) {
        if (sr->impostor_cache[i].used &&
            strncmp(sr->impostor_cache[i].meta_path, meta_path,
                    sizeof(sr->impostor_cache[i].meta_path)) == 0) {
            if (sr->impostor_cache[i].failed) return -1;
            /* Re-resolve the atlas texture handle: in editor mode the asset
             * cache recreates handles on reload (caching one would pin a stale,
             * possibly-destroyed handle → black/garbage), and in runtime mode an
             * async PAK decode may only now be ready.  The metadata is parsed
             * once; only the texture handle is re-fetched here. */
            JceTexture t = sr_resolve_texture(sr,
                              sr->impostor_cache[i].atlas.meta.atlas_path);
            if (jce_texture_valid(t)) {
                sr->impostor_cache[i].atlas.atlas = t;
                sr->impostor_cache[i].atlas.valid = true;
            } else {
                sr->impostor_cache[i].atlas.valid = false;
            }
            return sr->impostor_cache[i].atlas.valid ? i : -1;
        }
    }
    if (sr->impostor_cache_count >= SR_IMPOSTOR_CACHE_MAX) return -1;

    int slot = sr->impostor_cache_count++;
    SrImpostorCache *c = &sr->impostor_cache[slot];
    memset(c, 0, sizeof(*c));
    c->used = true;
    snprintf(c->meta_path, sizeof(c->meta_path), "%s", meta_path);

    /* Resolve the project-relative .impostor.json to a host path the JSON
     * reader can open: the editor's resolve_path callback maps it under the
     * source-asset root; runtime falls through to the raw path (PAK/CWD).
     * Mirrors the sidecar-resolve pattern used for .anim.json / .mask. */
    char meta_host[1024];
    const char *meta_to_load = meta_path;
    if (sr->has_cbs && sr->cbs.resolve_path &&
        sr->cbs.resolve_path(meta_path, meta_host, (int)sizeof(meta_host),
                             sr->cbs.userdata)) {
        meta_to_load = meta_host;
    }
    if (!jce_impostor_atlas_load(meta_to_load, sr_impostor_tex_load, sr, &c->atlas)) {
        /* Metadata may have parsed but the atlas texture is still pending: keep
         * the parsed meta and retry the texture next frame instead of failing. */
        if (c->atlas.meta.atlas_path[0]) {
            return -1;   /* parsed but texture not ready yet (retry next frame) */
        }
        c->failed = true;
        LOG_WARN(LOG_TAG, "impostor atlas load failed: %s", meta_path);
        return -1;
    }
    return slot;
}

/* Append a card instance to the per-atlas accumulator for `cache_slot`. */
static void sr_impostor_batch_add(JceSceneRenderer *sr, int cache_slot,
                                  const jce_vec3 *center, float radius,
                                  const float tint[3])
{
    SrImpostorBatch *b = NULL;
    for (int i = 0; i < sr->impostor_batch_count; ++i) {
        if (sr->impostor_batch[i].cache_slot == cache_slot) { b = &sr->impostor_batch[i]; break; }
    }
    if (!b) {
        if (sr->impostor_batch_count >= SR_IMPOSTOR_CACHE_MAX) return;
        b = &sr->impostor_batch[sr->impostor_batch_count++];
        b->cache_slot = cache_slot;
        b->count = 0;
        /* insts / cap persist across frames (reused); keep whatever's allocated. */
    }
    if (b->count >= b->cap) {
        uint32_t nc = b->cap ? b->cap * 2u : 256u;
        JceImpostorInstance *ni = (JceImpostorInstance *)JCE_REALLOC(
            b->insts, (size_t)nc * sizeof(JceImpostorInstance));
        if (!ni) return;
        b->insts = ni;
        b->cap = nc;
    }
    JceImpostorInstance *in = &b->insts[b->count++];
    /* The card is anchored at the entity world position offset by the model's
     * local bounds center (the bake's billboard anchor). */
    const JceImpostorMeta *m = &sr->impostor_cache[cache_slot].atlas.meta;
    in->center[0] = center->x;
    in->center[1] = center->y;
    in->center[2] = center->z;
    (void)m;
    in->radius = radius;
    if (tint) { in->tint[0]=tint[0]; in->tint[1]=tint[1]; in->tint[2]=tint[2]; }
    else      { in->tint[0]=in->tint[1]=in->tint[2]=1.0f; }
}

/* Flush all per-atlas card accumulators: one instanced draw per atlas. */
static void sr_impostor_flush(JceSceneRenderer *sr, uint16_t view_id)
{
    for (int i = 0; i < sr->impostor_batch_count; ++i) {
        SrImpostorBatch *b = &sr->impostor_batch[i];
        if (b->count == 0) continue;
        const JceImpostorAtlas *atlas = &sr->impostor_cache[b->cache_slot].atlas;
        /* The baked atlas already carries lit albedo; pass a neutral sun term
         * (NULL light) for v1 — directional relighting is a v2 defer. */
        jce_impostor_draw_instanced(sr->renderer, view_id, atlas,
                                    b->insts, b->count, NULL, NULL);
        sr->stat_impostor_cards += b->count;
        sr->stat_impostor_draws++;
        b->count = 0;   /* reset accumulator; keep allocation */
    }
    sr->impostor_batch_count = 0;

    /* Diagnostic (JCE_IMPOSTOR_DBG): periodic stat log so a headless capture can
     * confirm far props collapsed to a handful of instanced impostor draws. */
    if (sr->stat_impostor_cards > 0) {
        static int s_dbg = -1;
        if (s_dbg < 0) s_dbg = getenv("JCE_IMPOSTOR_DBG") ? 1 : 0;
        if (s_dbg) {
            static uint32_t s_n = 0;
            if ((s_n++ % 60u) == 0u)
                LOG_INFO(LOG_TAG, "impostor: %u far cards in %u instanced draw(s)",
                         sr->stat_impostor_cards, sr->stat_impostor_draws);
        }
    }
}

/* ── Shadow-pass GPU-instancing batch (depth-only; see sh_batch) ────── */
bool sr_sh_batch_add(JceSceneRenderer *sr, JceModel *model,
                            const jce_mat4 *world, uint16_t lod)
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
    sr->sh_batch[sr->sh_batch_count].lod   = lod;
    sr->sh_batch_count++;
    return true;
}

/* GPU-driven CSM shadow: which cascade (0..N-1) a shadow view belongs to, or -1
 * when the GPU shadow path is not active for this view (GPU off, planes not set,
 * or a non-cascade shadow view such as the simple map / local atlas tiles). */
static int sr_gpu_shadow_cascade(const JceSceneRenderer *sr, uint16_t view_id)
{
    if (!sr->gpu_driven_frame || !sr->gpu_shadow_planes_valid) return -1;
    if (view_id < sr->gpu_shadow_view0) return -1;
    uint32_t c = (uint32_t)(view_id - sr->gpu_shadow_view0);
    if (c >= sr->gpu_shadow_cascades) return -1;
    return (int)c;
}

/* Queue one (model, run-of-worlds) into the cascade's GPUScene cull batch — the
 * depth-only analogue of sr_gpu_add_run.  `worlds[0..run)` are the run's ENTITY
 * world matrices (already gathered contiguously).  Returns false (caller CPU-
 * draws the run) when the model is not GPU-instanceable, has no AABB, scratch
 * grow fails, or the GPUScene batch is exhausted. */
static bool sr_gpu_sh_add_run(JceSceneRenderer *sr, JceGpuScene *gs, JceModel *m,
                              const jce_mat4 *worlds, uint32_t run, uint32_t src)
{
    jce_mat4 node_lt;
    if (!jce_model_gpu_instanceable(m, &node_lt)) return false;

    float lmn[3], lmx[3];
    if (!jce_model_get_aabb(m, lmn, lmx)) return false;

    if (sr->gpu_rec_cap < run) {
        JceGpuSceneRecord *nr = (JceGpuSceneRecord *)JCE_REALLOC(
            sr->gpu_rec, (size_t)run * sizeof(JceGpuSceneRecord));
        if (!nr) return false;
        sr->gpu_rec = nr;
        sr->gpu_rec_cap = run;
    }

    /* Identical record layout to the color path: render matrix folds node_lt
     * (entity_world * node_lt — matches compute_static_node_world for a single
     * non-joint primitive), the cull AABB comes from the model-local AABB
     * corners transformed by the ENTITY world (corners already bake node_lt). */
    const jce_vec3 corners[8] = {
        { lmn[0], lmn[1], lmn[2] }, { lmx[0], lmn[1], lmn[2] },
        { lmn[0], lmx[1], lmn[2] }, { lmx[0], lmx[1], lmn[2] },
        { lmn[0], lmn[1], lmx[2] }, { lmx[0], lmn[1], lmx[2] },
        { lmn[0], lmx[1], lmx[2] }, { lmx[0], lmx[1], lmx[2] },
    };
    for (uint32_t k = 0; k < run; k++) {
        const jce_mat4 *ew = &worlds[k];
        JceGpuSceneRecord *rec = &sr->gpu_rec[k];
        rec->world = jce_m4_multiply(ew, &node_lt);
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
        rec->run_base = 0.0f; rec->run_index = 0.0f;
        rec->_id_z = 0.0f; rec->_id_w = 0.0f;
    }

    uint32_t num_indices = jce_model_gpu_index_count(m);
    JceGpuSceneRun gpu_run = { 0, UINT32_MAX };
    if (!jce_gpu_scene_add_run(gs, sr->gpu_rec, run, num_indices, &gpu_run))
        return false;

    if (sr->gpu_sh_draw_count >= sr->gpu_sh_draw_cap) {
        uint32_t ncap = sr->gpu_sh_draw_cap ? sr->gpu_sh_draw_cap * 2u : 256u;
        void *nb = JCE_REALLOC(sr->gpu_sh_draw,
                               (size_t)ncap * sizeof(*sr->gpu_sh_draw));
        if (!nb) return false;
        sr->gpu_sh_draw = nb;
        sr->gpu_sh_draw_cap = ncap;
    }
    sr->gpu_sh_draw[sr->gpu_sh_draw_count].model = m;
    sr->gpu_sh_draw[sr->gpu_sh_draw_count].base  = gpu_run.run_base;
    sr->gpu_sh_draw[sr->gpu_sh_draw_count].src   = src;
    sr->gpu_sh_draw[sr->gpu_sh_draw_count].count = run;
    sr->gpu_sh_draw[sr->gpu_sh_draw_count].indirect_el = gpu_run.indirect_el;
    sr->gpu_sh_draw_count++;
    return true;
}

/* Sort the shadow batch by (model, lod) and issue one shadow-instanced draw per
 * run into `view_id` (depth-only).  Mirrors sr_inst_flush; reuses the color
 * batch's inst_gather scratch (shadow flushes are sequenced before the color
 * flush, so there is no overlap).  The (model, lod) key keeps LOD'd casters
 * batched while casting the same reduced silhouette the color pass renders.
 *
 * GPU-driven (roadmap #18, r.gpu_driven on): when this view is a CSM cascade,
 * route base (lod==0) instanceable runs through that cascade's GPUScene compute
 * cull + indirect draw (collapsing the per-instance CPU submit that dominates
 * shadow_gather); LOD'd / non-instanceable runs and every non-cascade shadow
 * view stay on the byte-identical CPU instanced path. */
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

    int casc = sr_gpu_shadow_cascade(sr, view_id);
    JceGpuScene *gs = (casc >= 0) ? sr->gpu_shadow_scene[casc] : NULL;
    JceShaderHandle sh_inst = jce_renderer_get_program_shadow_inst(sr->renderer);
    bool gpu = gs && jce_gpu_scene_is_supported(gs)
             && sh_inst.idx != UINT16_MAX;

    if (gpu) {
        const uint16_t prog = (uint16_t)sh_inst.idx;
        jce_gpu_scene_begin(gs);
        sr->gpu_sh_draw_count = 0;

        /* Pass 1: per (model, lod) run, queue into the cascade cull batch or
         * CPU-draw immediately (lod>0 / non-instanceable). */
        uint32_t i = 0;
        while (i < sr->sh_batch_count) {
            JceModel *m   = sr->sh_batch[i].model;
            uint16_t  lod = sr->sh_batch[i].lod;
            uint32_t run = 0;
            while (i + run < sr->sh_batch_count &&
                   sr->sh_batch[i + run].model == m &&
                   sr->sh_batch[i + run].lod   == lod) {
                sr->inst_gather[run] = sr->sh_batch[i + run].world;
                run++;
            }
            if (lod == 0 &&
                sr_gpu_sh_add_run(sr, gs, m, sr->inst_gather, run, i)) {
                /* queued for the pass-2 GPU draw */
            } else {
                if (lod > 0) jce_model_set_draw_lod(lod);
                jce_model_draw_shadow_instanced(m, sr->renderer, view_id,
                                                sr->inst_gather, run);
                if (lod > 0) jce_model_set_draw_lod(0);
            }
            i += run;
        }

        /* Pass 2: one cull dispatch for this cascade, then draw each run from
         * its compacted partition.  On dispatch / buffer failure fall back to a
         * CPU instanced draw (re-gathering the run's worlds) so no caster drops. */
        if (sr->gpu_sh_draw_count > 0) {
            bool ok = jce_gpu_scene_dispatch(gs, sr->gpu_reset_view,
                                             sr->gpu_cull_view,
                                             sr->gpu_shadow_planes[casc]);
            uint16_t vis = jce_gpu_scene_visible_vb(gs);
            uint16_t ind = jce_gpu_scene_indirect_buffer(gs);
            for (uint32_t d = 0; d < sr->gpu_sh_draw_count; d++) {
                JceModel *m  = sr->gpu_sh_draw[d].model;
                uint32_t bse = sr->gpu_sh_draw[d].base;
                uint32_t src = sr->gpu_sh_draw[d].src;
                uint32_t cnt = sr->gpu_sh_draw[d].count;
                uint32_t iel = sr->gpu_sh_draw[d].indirect_el;
                if (ok && vis != UINT16_MAX && ind != UINT16_MAX &&
                    iel != UINT32_MAX) {
                    jce_model_draw_shadow_indirect_from_buffer(
                        m, sr->renderer, view_id, prog, vis, ind, iel);
                } else if (ok && vis != UINT16_MAX) {
                    jce_model_draw_shadow_instanced_from_buffer(
                        m, sr->renderer, view_id, prog, vis, bse, cnt);
                } else {
                    for (uint32_t k = 0; k < cnt; k++)
                        sr->inst_gather[k] = sr->sh_batch[src + k].world;
                    jce_model_draw_shadow_instanced(m, sr->renderer, view_id,
                                                    sr->inst_gather, cnt);
                }
            }
        }
        sr->sh_batch_count = 0;
        return;
    }

    /* ── CPU instanced path (GPU off, or a non-cascade shadow view) ──────── */
    uint32_t i = 0;
    while (i < sr->sh_batch_count) {
        JceModel *m   = sr->sh_batch[i].model;
        uint16_t  lod = sr->sh_batch[i].lod;
        uint32_t run = 0;
        while (i + run < sr->sh_batch_count &&
               sr->sh_batch[i + run].model == m &&
               sr->sh_batch[i + run].lod   == lod) {
            sr->inst_gather[run] = sr->sh_batch[i + run].world;
            run++;
        }
        if (lod > 0) jce_model_set_draw_lod(lod);
        jce_model_draw_shadow_instanced(m, sr->renderer, view_id, sr->inst_gather, run);
        if (lod > 0) jce_model_set_draw_lod(0);
        i += run;
    }
    sr->sh_batch_count = 0;
}

static bool sr_try_draw_mesh_renderer_model(JceSceneRenderer *sr,
                                            JceScene *scene,
                                            JceEntity e,
                                            uint16_t view_id,
                                            const jce_mat4 *model,
                                            const JceSceneRenderConfig *cfg,
                                            uint32_t entity_lod, float lod_fade)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path) return false;

    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    /* ── Streaming LOD cross-fade (Direction B; dithered detail fade-in) ──
     * A freshly streamed-in detail entity dithers in over JCE_SR_FADE_DURATION
     * while the resident HLOD proxy stays visible (the streamer delays hiding
     * the proxy by the same duration), so the cell no longer pops.  fade==1.0
     * is the steady state (the overwhelmingly common case) and an exact no-op:
     * the uniform stays at the pass default {1,0,0,0} (inactive) and the model
     * draws byte-identically.  In a still editor preview sr->fade_time is 0, so
     * sr_fade_factor_for_entity returns 1.0 and this whole feature is dormant. */
    float fade = sr_fade_factor_for_entity(sr, (uint32_t)e);
    bool  fading = (fade < 0.999f) && BGFX_HANDLE_IS_VALID(sr->u_lod_fade);

    /* LOD cross-fade (large-world #6): mid-transition between in-asset levels.
     * Draws the outgoing level dithered + the incoming level full underneath. */
    bool  lod_xfade = (lod_fade < 0.999f) && BGFX_HANDLE_IS_VALID(sr->u_lod_fade);

    /* Standard material-slot override: a MeshRenderer that authored a material/
     * albedo texture wins over the model's embedded materials.  Fixes
     * geometry-only models converted to glTF whose texture lives in the scene.
     * `have_ov` means GENUINE material divergence (different texture/material),
     * which one shared instanced submit cannot express → that entity stays solo. */
    const JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    JcePbrMaterial ov;
    bool have_ov = sr_mr_override_material(sr, mr, &ov);

    /* Per-entity baseColor tint (large-world-opt P1 #7).  A MeshRenderer can
     * author a non-white baseColor WITHOUT diverging the texture/material; that
     * is a pure tint, which the instanced path now carries per-instance (i_data4
     * → fs_pbr_tint) so the copy STILL batches instead of being kicked solo.
     * base_color[3] > 0 marks an authored colour (matches sr_mr_override_material);
     * a divergent override (have_ov) already folds base_color into its bound
     * material, so the tint stream is only used on the non-override batch path. */
    float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool have_tint = false;
    if (mr && mr->base_color[3] > 0.0f &&
        (mr->base_color[0] != 1.0f || mr->base_color[1] != 1.0f ||
         mr->base_color[2] != 1.0f || mr->base_color[3] != 1.0f)) {
        tint[0] = mr->base_color[0];
        tint[1] = mr->base_color[1];
        tint[2] = mr->base_color[2];
        tint[3] = mr->base_color[3];
        have_tint = true;
    }

    /* Editor missing-albedo checker — ONLY in the TEXTURED debug view, so the
     * normal SHADED view (and runtime) renders an untextured model with its
     * lit base color instead of the pink-black hint. */
    bool editor_mode = (sr->has_cbs && sr->cbs.load_texture);
    bool checker = editor_mode && cfg &&
        cfg->view_mode == JCE_SCENE_VIEW_TEXTURED;

    /* GPU-instancing fast path: a model with no GENUINE material override and
     * no skinned primitives, outside the checker debug view, is deferred into
     * the per-model instance batch (flushed once per model at pass end).  A pure
     * baseColor tint NO LONGER kicks the entity to a solo draw (P1 #7): it rides
     * along in the batch via the per-instance tint, so a colour-varied city's
     * thousands of repeated-mesh draws still collapse into a handful of instanced
     * submits.  Falls through to the immediate draw on batch-OOM.
     * A fading entity is kept OUT of the instanced batch (the batch never sets
     * u_lodFade, so it always draws at the inactive default): routing it solo
     * lets the per-draw uniform below dither just this one instance in.  Fades
     * last ~0.4s on a freshly streamed cell, so the brief solo draw is cheap. */
    if (!have_ov && !checker && !fading && !lod_xfade &&
        jce_model_is_instanceable(mc->model)) {
        if (sr_inst_batch_add(sr, mc->model, model, (uint16_t)entity_lod,
                              have_tint ? tint : NULL))
            return true;
    }

    if (have_ov) jce_model_set_material_override(&ov);
    if (checker) jce_model_set_albedo_checker(true);

    /* One solo submit, parameterised: a pure baseColor tint routes through the
     * count==1 tinted path (parity with the batched copy); else plain. */
    #define SR_DRAW_ONE() do {                                                  \
        if (have_tint && !have_ov && !checker) {                                \
            jce_vec4 t1 = { tint[0], tint[1], tint[2], tint[3] };               \
            jce_model_draw_instanced_tinted(mc->model, sr->renderer, view_id,   \
                                            model, &t1, 1);                     \
        } else {                                                                \
            jce_model_draw(mc->model, sr->renderer, view_id, model, NULL, 0);   \
        }                                                                       \
    } while (0)

    if (lod_xfade) {
        /* True LOD cross-fade: draw the OUTGOING (higher-detail) level dithered
         * — the screen-door-discarded pixels write NO depth — then the INCOMING
         * (lower-detail) level full underneath fills exactly those holes.  Each
         * pixel resolves once (no z-fight), no shader change, pop-free.  A
         * concurrent streaming fade folds into the outgoing alpha. */
        float out_fade = lod_fade * (fading ? fade : 1.0f);
        if (entity_lod > 0) jce_model_set_draw_lod(entity_lod);
        { float lf[4] = { out_fade, 0.0f, 0.0f, 1.0f };
          bgfx_set_uniform(sr->u_lod_fade, lf, 1); }
        SR_DRAW_ONE();
        jce_model_set_draw_lod(entity_lod + 1);   /* incoming, clamped by model */
        { float lf[4] = { fading ? fade : 1.0f, 0.0f, 0.0f, fading ? 1.0f : 0.0f };
          bgfx_set_uniform(sr->u_lod_fade, lf, 1); }
        SR_DRAW_ONE();
        { float lf[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
          bgfx_set_uniform(sr->u_lod_fade, lf, 1); }
        jce_model_set_draw_lod(0);
    } else {
        /* In-asset auto-LOD: a distant entity (entity_lod>0) binds its model's
         * reduced index set; 0 => base (byte-identical to before). */
        if (entity_lod > 0) jce_model_set_draw_lod(entity_lod);
        /* Streaming cross-fade: screen-door dither for THIS draw only. */
        if (fading) {
            float lf_on[4] = { fade, 0.0f, 0.0f, 1.0f };
            bgfx_set_uniform(sr->u_lod_fade, lf_on, 1);
        }
        SR_DRAW_ONE();
        if (fading) {
            float lf_off[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(sr->u_lod_fade, lf_off, 1);
        }
        if (entity_lod > 0) jce_model_set_draw_lod(0);
    }
    #undef SR_DRAW_ONE

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
static void sr_model_presubmit_cb(void *user, uint16_t view_id)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    (void)view_id;
    if (!sr) return;
    if (sr->fp_active_frame)
        jce_forwardplus_bind(sr->forwardplus);
    /* Re-bind the directional shadow state (CSM cascade textures stages 9-12,
     * or the legacy single map / local atlas) before EVERY primitive submit.
     * jce_model_draw emits one bgfx_submit per primitive and the D3D11/D3D12/
     * Vulkan backends clear texture-stage bindings between submits (the GL
     * backend leaves them bound, which is why binding once per frame silently
     * "worked" on GL only).  Without this re-bind, every primitive after the
     * first sampled UNBOUND cascade textures (-> 0) and was self-shadowed to
     * black on D3D/VK while GL rendered correctly.  Mirrors the Forward+
     * cluster re-bind above; gated on frame_shadow_active so non-shadow frames
     * are byte-identical. */
    if (sr->frame_shadow_active)
        sr_bind_frame_shadow_state(sr);
}

/* Set toon uniforms before each toon-character primitive submit (mirrors the
 * fwdplus cluster-bind pre-submit hook: bgfx clears uniform state per submit). */
static void sr_model_toon_presubmit_cb(void *user, uint16_t view_id)
{
    (void)view_id;
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    if (!sr) return;
    bgfx_set_uniform(sr->u_toon_params,    sr->toon_params_frame,    1);
    bgfx_set_uniform(sr->u_toon_rim_color, sr->toon_rim_color_frame, 1);
    /* Same per-submit shadow re-bind as sr_model_presubmit_cb (toon characters
     * still receive directional shadows). */
    if (sr->frame_shadow_active)
        sr_bind_frame_shadow_state(sr);
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

    /* Advance the grass wind phase clock (drives vs_grass's u_grass_time).
     * Mirrors the water clock: 0 in still editor previews, animates in Play. */
    if (dt_sec > 0.0f) {
        sr->grass_time += dt_sec;
        if (sr->grass_time > 100000.0f) sr->grass_time -= 100000.0f;
    }

    /* Advance the streaming cross-fade phase clock (Direction B).  GATED on a
     * streaming scene (enabled + chunks): the dithered detail fade-in is only for
     * cells streaming in, so in every non-streaming scene fade_time stays 0 and
     * those scenes render byte-identically (no gratuitous load dither).  Also 0
     * in still previews (dt_sec==0).  NOT wrapped: first_seen stamps store an
     * absolute time and the diff drives the [0,1] fade, so a wrap would briefly
     * snap an in-progress fade.  100000s (~27h) of monotonic float is plenty. */
    {
        const JceSceneStreamingSettings *fst = jce_scene_get_streaming_settings(scene);
        if (dt_sec > 0.0f && fst && fst->enabled && fst->chunk_count > 0)
            sr->fade_time += dt_sec;
    }

    if (list->count == 0) return;

    /* Gather lights. */
    if (sr->light_env) {
        jce_light_env_clear(sr->light_env);
        /* Forward+ (ROUND A): accumulate the same point+spot lights into the
         * cluster scratch arrays as we add them to light_env.  Counts up to
         * fp_max_lights (256), independent of the brute-force env caps. */
        uint32_t fp_n = 0;
        /* Round B: collect EVERY visible point/spot light into the cluster
         * scratch (up to fp_max_lights), decoupled from the brute-force top-N
         * selection below — that decoupling is what actually lifts the 16-light
         * cap.  Only fill when Forward+ is on this frame (when off the cluster
         * is never uploaded, so skip the work to stay byte-identical/cheap). */
        bool fp_collect = sr->cv_forwardplus
                       && jce_cvar_get_bool(sr->cv_forwardplus);
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
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_POINT_LIGHT)) {
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
                    /* Brute-force env path: only the culled top-N (≤16). */
                    if (sr_light_selected(sr->frame_sel_point,
                                          sr->frame_sel_point_n, e))
                        jce_light_env_add_point_light(sr->light_env, &pl);

                    /* Forward+ cluster: every visible point light up to capacity,
                     * decoupled from the 16-cap selection (Round B). */
                    if (fp_collect && sr->fp_proxies && fp_n < sr->fp_max_lights) {
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
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPOT_LIGHT)) {
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
                    /* Brute-force env path: only the culled top-N (≤4). */
                    if (sr_light_selected(sr->frame_sel_spot,
                                          sr->frame_sel_spot_n, e))
                        jce_light_env_add_spot_light(sr->light_env, &sl);

                    /* Forward+ cluster: every visible spot light up to capacity,
                     * decoupled from the brute-force selection (Round B).  The
                     * cluster broad-phase treats the spot as a bounding sphere
                     * (radius); the per-light cone params let the shader evaluate
                     * the cone afterwards. */
                    if (fp_collect && sr->fp_proxies && fp_n < sr->fp_max_lights) {
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

    /* Begin sprite batch.  Unconditional: billboards (draw_opaque-gated) add to
     * this batch even when draw_sprites is off (e.g. the editor scene view), so
     * the batch must be reset every frame regardless. Sprites are still only
     * ADDED when draw_sprites, so a draw_sprites=off frame begins/flushes an
     * empty batch unless billboards filled it. */
    if (sr->sprite_batch)
        jce_sprite_batch_begin(sr->sprite_batch);

    /* Optional broadphase frustum culling.  When disabled, every entity
     * is treated as visible (matches legacy behaviour).
     *
     * Large-world capacity: the visibility scratch is a PER-RENDERER HEAP buffer
     * grown to list->count, NOT a stack array — a fixed `bool[SR_MAX_ENTITIES]`
     * is 32KB today and would be 100KB+ once the entity cap is removed, risking a
     * stack overflow.  Grown lazily here (2× hysteresis), freed on destroy.  If
     * the grow fails we treat every entity as visible (safe: nothing is dropped,
     * just unculled) so a single OOM never crashes the frame. */
    if ((uint32_t)list->count > sr->visible_buf_cap) {
        uint32_t new_cap = sr->visible_buf_cap ? sr->visible_buf_cap : SR_MAX_ENTITIES;
        while (new_cap < (uint32_t)list->count) {
            if (new_cap > 0x7FFFFFFFu / 2u) { new_cap = (uint32_t)list->count; break; }
            new_cap *= 2u;
        }
        bool *grown = (bool *)JCE_REALLOC(sr->visible_buf, (size_t)new_cap * sizeof(bool));
        if (grown) { sr->visible_buf = grown; sr->visible_buf_cap = new_cap; }
    }
    bool *visible = sr->visible_buf;
    /* If the grow failed (buffer absent / still too small) take the "all visible"
     * path: nothing is dropped (just unculled), and the per-entity reads below
     * are gated by `all_visible` so the NULL/short buffer is never dereferenced. */
    const bool all_visible = (!visible || sr->visible_buf_cap < (uint32_t)list->count);
    sr->stat_total_entities  = (uint32_t)list->count;
    sr->stat_culling_enabled = cfg->frustum_culling && !all_visible;
    if (cfg->frustum_culling && !all_visible && camera && list->count > 0) {
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
        /* Cull disabled OR the visibility buffer is unavailable: treat every
         * entity as visible.  Only fill the buffer when it exists + is big
         * enough; the !all_visible read below skips the buffer entirely when it
         * isn't, so no out-of-range / NULL access occurs. */
        if (!all_visible)
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
    /* Reset per-frame impostor counters (P2 #10). */
    sr->stat_impostor_cards = 0;
    sr->stat_impostor_draws = 0;

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
        /* Set the view transform for the proxy pre-pass view so the depth-only
         * proxy draws are in the correct camera space.  Use the culler's OWN
         * view id (default 254) — NOT a hard-coded 254 — so a second culler
         * sharing this engine renderer in the same bgfx frame (editor game-view
         * alongside the scene-view) can drive a distinct view and never clobber
         * the other path's proxy transform. */
        bgfx_set_view_transform(
            jce_occlusion_culler_get_view_id(cfg->occlusion_culler),
            JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
        /* Bind the proxy view to the SAME framebuffer the color/depth pass
         * renders into (cfg->scene_frame_buffer: editor bridge FBO, runtime
         * postfx offscreen FBO, or UINT16_MAX backbuffer).  Without this, every
         * OFFSCREEN path's proxy boxes test a stale/empty backbuffer depth →
         * occlusion either inert (passes everything) or false-culls visible
         * geometry.  The proxy view id is > the color view id, so bgfx orders it
         * AFTER the color pass; the depth it tests is the current frame's.  Use
         * the color pass's exact viewport rect so the rasterised proxies cover
         * the pixels the depth was written at. */
        {
            uint16_t oc_w = (cfg->viewport_width  > 0 && cfg->viewport_width  <= 0xFFFFu)
                            ? (uint16_t)cfg->viewport_width  : 0;
            uint16_t oc_h = (cfg->viewport_height > 0 && cfg->viewport_height <= 0xFFFFu)
                            ? (uint16_t)cfg->viewport_height : 0;
            jce_occlusion_culler_bind_target(cfg->occlusion_culler,
                                             cfg->scene_frame_buffer,
                                             0, 0, oc_w, oc_h);
        }
    } else if (cfg->occlusion_culler) {
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler, NULL, NULL);
    }
    jce_vec3 lod_cam_pos = (jce_vec3){0, 0, 0};
    if (sr->stat_lod_enabled && camera) lod_cam_pos = jce_camera_get_position(camera);

    /* NOTE: per-entity LODGroup selection + the streaming far-cull FLOOR moved
     * to sr_compute_entity_lod (run once in the ecull build loop, before BOTH
     * the shadow and color passes, so they bind the SAME LOD level).  This pass
     * now only CONSUMES the cached ecull[i].lod_level / .lod_culled. */

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
    if (sr->fp_active_frame || sr->frame_shadow_active)
        jce_model_set_pre_submit_cb(sr_model_presubmit_cb, sr);

    /* Flush any shadow-pass instancing batch left pending by the last shadow
     * view (cascades/lights auto-flush on view change; this drains the final
     * one before the color pass reuses inst_gather). */
    if (sr->sh_batch_count > 0)
        sr_sh_flush(sr, sr->sh_batch_view);

    /* Reset the per-model GPU-instancing batch; batchable static glTF entities
     * accumulate during the loop and flush once after it. */
    sr->inst_batch_count = 0;

    /* ── Streaming LOD cross-fade (Direction B): leak-safe default ────────
     * Set u_lodFade INACTIVE ({fade=1, active=0}) once at the start of the
     * color pass.  bgfx uniform values persist across submits within a frame,
     * so a single default here means EVERY draw path that touches fs_pbr_body
     * (the 6 PBR variants) inherits the inactive value unless a fading static
     * draw explicitly overrides it for its own submit and resets it right after.
     * The shader's discard block is gated by (active>0.5 && fade<0.999), so with
     * this default the dither path is never taken => byte-identical to today.
     * Created lazily here (and destroyed in jce_scene_renderer_destroy) so the
     * handle exists before the first set; never set when the handle is invalid. */
    if (!BGFX_HANDLE_IS_VALID(sr->u_lod_fade))
        sr->u_lod_fade = bgfx_create_uniform("u_lodFade",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    if (BGFX_HANDLE_IS_VALID(sr->u_lod_fade)) {
        float lf_off[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; /* fade=1, active=0 */
        bgfx_set_uniform(sr->u_lod_fade, lf_off, 1);
    }

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

        /* ── Line Renderer (polyline; reuses the color program + PT_LINES) ──
         * Bypasses the per-entity AABB (segments may be large/scattered).
         * Drawn additively — does NOT `continue`, so an entity may carry both a
         * mesh and a line renderer. */
        if (cfg->draw_opaque && jce_scene_has_line_renderer(scene, e)) {
            static int s_line_cid = -2;
            if (s_line_cid == -2) s_line_cid = jce_component_find("LineRenderer");
            if (s_line_cid < 0 || jce_scene_comp_enabled(scene, e, s_line_cid))
                sr_draw_line_renderer(sr, scene, list, e, camera, view_id);
        }

        /* ── Trail Renderer (open ribbon over the captured point buffer) ──
         * Same camera-facing ribbon as Line; world-space captured points.
         * Bypasses the AABB + does NOT continue (mesh + trail can coexist). */
        if (cfg->draw_opaque && jce_scene_has_trail_renderer(scene, e)) {
            static int s_trail_cid = -2;
            if (s_trail_cid == -2) s_trail_cid = jce_component_find("TrailRenderer");
            if (s_trail_cid < 0 || jce_scene_comp_enabled(scene, e, s_trail_cid))
                sr_draw_trail_renderer(sr, scene, list, e, camera, view_id);
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

        /* ── Grass Field (GPU-instanced blades + wind, Stage 1b.6) ────
         * Dedicated module; bypasses the per-entity AABB like foliage/water.
         * Three-layer gate: hardware instancing + GPU tier >= HIGH +
         * project-level grass_enabled (JceRenderSettings).  Opaque, so
         * dispatched in the cfg->draw_opaque branch. */
        if (cfg->draw_opaque && jce_scene_has_grass_field(scene, e)) {
            static int s_grass_cid = -2;
            if (s_grass_cid == -2) s_grass_cid = jce_component_find("GrassField");
            bool enabled  = (s_grass_cid < 0) ||
                             jce_scene_comp_enabled(scene, e, s_grass_cid);
            bool caps_ok  = (jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0;
            bool tier_ok  = jce_renderer_get_tier() >= JCE_GPU_TIER_HIGH;
            bool proj_ok  = sr->grass_enabled;
            if (enabled && caps_ok && tier_ok && proj_ok)
                sr_draw_grass(sr, scene, list, e, view_id);
            continue;
        }

        if (!all_visible && !visible[i]) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;

        /* ── Occlusion culling (GPU-query, two-pass coherence) ───────
         * The proxy box must match the REAL geometry's world AABB, not a flat
         * cube sized off one scale value: the per-frame cull cache (sr->ecull[i])
         * already holds the entity's world AABB (model LOCAL aabb transformed by
         * the world matrix — includes the geometry's pivot offset and per-axis
         * extents), so reuse it for an exact-fit proxy.  has_aabb==false (no
         * resolvable model: primitives, .obj) → fall back to the world position +
         * a scale-derived cube.  Skip the draw if the entity was fully occluded
         * last frame and submit a depth-only proxy instead so the culler can
         * recover visibility. */
        jce_vec3 occ_center = { model.col[3].x, model.col[3].y, model.col[3].z };
        jce_vec3 occ_half   = { 0.5f, 0.5f, 0.5f };
        if (cfg->occlusion_culler) {
            if (sr->ecull && sr->ecull[i].has_aabb) {
                const jce_vec3 mn = sr->ecull[i].wmin;
                const jce_vec3 mx = sr->ecull[i].wmax;
                occ_center.x = (mn.x + mx.x) * 0.5f;
                occ_center.y = (mn.y + mx.y) * 0.5f;
                occ_center.z = (mn.z + mx.z) * 0.5f;
                occ_half.x   = (mx.x - mn.x) * 0.5f;
                occ_half.y   = (mx.y - mn.y) * 0.5f;
                occ_half.z   = (mx.z - mn.z) * 0.5f;
            } else {
                /* No resolvable AABB — derive a cube from the entity scale. */
                float sx = model.col[0].x, sy = model.col[1].y, sz = model.col[2].z;
                float sc = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
                if (sc < 0.001f) sc = 0.001f;
                occ_half.x = occ_half.y = occ_half.z = sc * 0.5f;
            }

            /* Pass the LARGEST half-extent as the proximity radius so the
             * culler's camera-near skip (which forces a too-close object visible
             * to avoid near-plane/self-occlusion flicker) matches submit_query. */
            float occ_r = occ_half.x;
            if (occ_half.y > occ_r) occ_r = occ_half.y;
            if (occ_half.z > occ_r) occ_r = occ_half.z;
            bool vis = jce_occlusion_culler_entity_visible(
                           cfg->occlusion_culler, (uint64_t)e, occ_center,
                           occ_r);
            /* Submit the proxy query HERE — BEFORE any downstream `continue`.
             * The dense city's static buildings take the instanced glTF path
             * (sr_try_draw_mesh_renderer_model) which `continue`s well before the
             * end of the loop body, so a submit_query at the loop's tail (the
             * old location) was NEVER reached for them → no slot ever created →
             * every entity stuck in warm-up and the culler permanently inert.
             * Issuing the query right after the visibility test covers EVERY draw
             * path (instanced, skinned, sprite, inline) uniformly.  Submit for
             * occluded entities too so visibility can recover next frame. */
            jce_occlusion_culler_submit_query(cfg->occlusion_culler,
                                              (uint64_t)e, occ_center, occ_half);
            if (!vis) continue;  /* occluded last frame → skip the draw */
        }

        /* ── Octahedral impostor terminal LOD (roadmap P2 #10) ────────
         * Checked BEFORE the per-level mesh-override LOD block below: the
         * impostor is the TERMINAL LOD and must win over any mid-level mesh
         * override (e.g. street_demo trees swap to tree-small at LOD1, which
         * would otherwise `continue` and the card would never fire).  Past the
         * authored impostor distance a far prop renders as a single camera-facing
         * card sampling its pre-baked octahedral atlas — a forest of distant
         * trees collapses to a handful of instanced quads.  Skinned entities
         * never impostor.  No impostor authored => byte-identical to before. */
        if (cfg->draw_opaque && camera &&
            jce_scene_has_lod_group(scene, e) &&
            !jce_scene_has_skeletal_animator(scene, e)) {
            JceLodGroupComponent *lg = jce_scene_get_lod_group(scene, e);
            if (lg && lg->impostor_meta_path[0] && lg->impostor_distance > 0.0f) {
                jce_vec3 cp = jce_camera_get_position(camera);
                float dx = model.col[3].x - cp.x;
                float dy = model.col[3].y - cp.y;
                float dz = model.col[3].z - cp.z;
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                if (dist >= lg->impostor_distance) {
                    int cs = sr_impostor_get_cache(sr, lg->impostor_meta_path);
                    if (cs >= 0) {
                        /* Card anchor: entity world position + the model's local
                         * bounds center (the bake's billboard anchor), scaled by
                         * the entity's uniform scale.  Radius = bake radius *
                         * entity scale so the card matches the mesh footprint. */
                        const JceImpostorMeta *m =
                            &sr->impostor_cache[cs].atlas.meta;
                        float sx = sqrtf(model.col[0].x*model.col[0].x +
                                         model.col[0].y*model.col[0].y +
                                         model.col[0].z*model.col[0].z);
                        jce_vec3 anchor = jce_v3(
                            model.col[3].x + m->center[0] * sx,
                            model.col[3].y + m->center[1] * sx,
                            model.col[3].z + m->center[2] * sx);
                        float card_r = m->radius * sx;
                        /* Per-instance baseColor tint from the MeshRenderer. */
                        float tint[3] = { 1.0f, 1.0f, 1.0f };
                        if (jce_scene_has_mesh_renderer(scene, e)) {
                            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
                            if (mr) { tint[0]=mr->base_color[0]; tint[1]=mr->base_color[1]; tint[2]=mr->base_color[2]; }
                        }
                        sr_impostor_batch_add(sr, cs, &anchor, card_r, tint);
                        continue;   /* drew impostor card; skip the mesh + LOD override */
                    }
                    /* Atlas not ready yet (async) → fall through to the mesh this
                     * frame; the card takes over once the atlas finishes loading. */
                }
            }
        }

        /* ── Per-entity LOD group (P1 #6) ─────────────────────────────
         * The LOD level + far-cull were computed ONCE in the ecull build loop
         * (sr_compute_entity_lod) so the color and shadow passes agree.  Consume
         * the cached result here:
         *   - lod_culled  → skip (LODGroup cull-when-too-far fired).
         *   - a non-empty per-level override mesh path → draw THAT model
         *     (legacy "distinct mesh per level" authoring).
         *   - otherwise AUTO-BIND the in-asset cooked LOD chain: arm the model's
         *     draw-LOD so the entity's own model draws its reduced index set
         *     (empty meshPath convention => uses cooked LODs, no extra file).
         * entity_lod (1-based for jce_model_set_draw_lod; 0 = base) flows into
         * BOTH the skinned/mesh-renderer draws below and the instancing batch. */
        uint32_t entity_lod = 0;   /* 0 = base (LOD0); N = (N-1)'th reduced set */
        /* ecull is grown to >= list->count each frame (large-world capacity), and
         * this loop runs i < list->count, so ecull[i] is always in range — the
         * old `i < SR_MAX_ENTITIES` bound is no longer needed. */
        if (sr->ecull) {
            if (sr->ecull[i].lod_culled) continue;   /* beyond range → cull */
            uint8_t sel = sr->ecull[i].lod_level;    /* 0 = base */
            if (sel > 0 && jce_scene_has_lod_group(scene, e)) {
                JceLodGroupComponent *lg = jce_scene_get_lod_group(scene, e);
                int n = lg ? lg->level_count : 0;
                if (n > JCE_LOD_MAX_LEVELS) n = JCE_LOD_MAX_LEVELS;
                int li = (sel < (uint8_t)n) ? (int)sel : (n - 1);
                if (lg && li >= 0) {
                    const char *lpath = lg->level_mesh_paths[li];
                    if (lpath[0] && !jce_scene_has_skeletal_animator(scene, e)) {
                        /* Distinct per-level mesh override: draw that model. */
                        SrModelCache *lmc = sr_get_model(sr, lpath, (uint32_t)e);
                        if (lmc && lmc->model) {
                            jce_model_draw(lmc->model, sr->renderer,
                                           view_id, &model, NULL, 0);
                            continue;  /* drew LOD level model; skip base */
                        }
                    }
                    /* Empty path → auto-bind the in-asset cooked LOD: the model
                     * binds reduced index set (sel) on its own geometry. */
                    entity_lod = (uint32_t)sel;
                }
            }
        }

        /* ── Global LOD substitution (MVP, debug; off by default) ──── */
        if (sr->stat_lod_enabled) {
            const float dx = model.col[3].x - lod_cam_pos.x;
            const float dy = model.col[3].y - lod_cam_pos.y;
            const float dz = model.col[3].z - lod_cam_pos.z;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            uint8_t rec = sr_lod_state_get(sr, (uint32_t)e);
            const int prev = rec > 0 ? (int)rec - 1 : -1;
            const int lvl  = jce_lod_pick(sr->global_lod, dist, prev);
            if (lvl < 0) {
                sr->stat_lod_culled++;
                /* Park at last level so a return swing snaps cleanly. */
                sr_lod_state_set(sr, (uint32_t)e,
                                 (uint8_t)(sr->global_lod->count + 1));
                continue;
            }
            sr_lod_state_set(sr, (uint32_t)e, (uint8_t)(lvl + 1));
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

                    if (ai && ai->morph_vb_count > 0) {
                        /* Morph path unchanged (v1: toon not applied to morphed). */
                        jce_model_draw_morphed(mc->model, sr->renderer, view_id,
                                               &model, pal, pal_n,
                                               sr_morph_vb_cb, ai);
                    } else {
                        /* Toon decision: sibling MeshRenderer.toon + frame gate. */
                        JceMeshRenderer *smr = jce_scene_has_mesh_renderer(scene, e)
                            ? jce_scene_get_mesh_renderer(scene, e) : NULL;
                        bool want_toon = sr->toon_allowed_frame && smr && smr->toon;

                        /* V1 LIMITATION — Forward+ cluster lighting and toon are
                         * mutually exclusive on the same character.  When
                         * fp_active_frame is true the scene uses clustered
                         * point/spot lights (jce_forwardplus_bind, stage 14).
                         * The toon path switches to fs_pbr_toon which has no
                         * pbr_fwdplus variant and never calls jce_forwardplus_bind;
                         * the sr_model_toon_presubmit_cb only sets u_toonParams /
                         * u_toonRimColor.  As a result, a toon character rendered
                         * while Forward+ is active receives only directional lights
                         * (quantized via the cel ramp) + IBL/ambient — point and
                         * spot lights from the cluster grid are NOT applied to the
                         * toon character.  Non-toon skinned entities in the same
                         * frame ARE correctly lit via fwdplus (the cb is restored
                         * after the toon section).  A future pbr_toon_fwdplus
                         * variant can resolve this; out of scope for v1. */

                        JceShaderHandle toon_override = JCE_INVALID_SHADER;
                        if (want_toon) {
                            /* Stash this character's knobs for the pre-submit cb. */
                            float bands = (smr->toon_bands > 0) ? (float)smr->toon_bands : 3.0f;
                            sr->toon_params_frame[0] = bands;
                            sr->toon_params_frame[1] = 0.15f; /* ramp edge softness */
                            sr->toon_params_frame[2] = (smr->rim_power > 0.0f) ? smr->rim_power : 4.0f;
                            sr->toon_params_frame[3] = smr->rim_intensity;
                            sr->toon_rim_color_frame[0] = smr->rim_color[0];
                            sr->toon_rim_color_frame[1] = smr->rim_color[1];
                            sr->toon_rim_color_frame[2] = smr->rim_color[2];
                            sr->toon_rim_color_frame[3] = 0.0f;

                            /* OUTLINE: inverted hull in the SAME color view, BEFORE
                             * the lit character.  FRONT-cull (CULL_CCW) so only the
                             * back-faces of the expanded hull show as a silhouette
                             * ring; DEPTH_TEST_LESS + depth write.
                             *
                             * Clear the Forward+ pre-submit hook before the outline
                             * draw so the cluster-texture binding does not fire on
                             * outline primitives (fs_outline is flat; it ignores the
                             * cluster binding entirely, but firing it is architecturally
                             * inconsistent and wastes the stage-14 bind).  The toon
                             * pre-submit cb is installed immediately after. */
                            jce_model_set_pre_submit_cb(NULL, NULL);

                            JceShaderHandle outline_prog =
                                jce_renderer_get_program_outline_skinned(sr->renderer);
                            if (outline_prog.idx != UINT16_MAX && smr->outline_width > 0.0f) {
                                float op[4] = { smr->outline_width, 0.0f, 0.0f, 0.0f };
                                float oc[4] = { smr->outline_color[0], smr->outline_color[1],
                                                smr->outline_color[2], 1.0f };
                                bgfx_set_uniform(sr->u_outline_params, op, 1);
                                bgfx_set_uniform(sr->u_outline_color,  oc, 1);
                                /* Draw the hull with the outline program as the
                                 * skinned override; FRONT-cull set via the model
                                 * draw's per-submit state is not exposed, so submit
                                 * the hull as a dedicated skinned draw with the
                                 * outline override + reverse-cull double-sided off.
                                 * (See NOTE in plan Task 5 Step 6 for the cull-state seam.) */
                                jce_model_draw_program(mc->model, sr->renderer, view_id,
                                                       &model, pal, pal_n, outline_prog);
                            }

                            toon_override = jce_renderer_get_program_pbr_skinned_toon(sr->renderer);
                            jce_model_set_pre_submit_cb(sr_model_toon_presubmit_cb, sr);
                        }

                        jce_model_draw_program(mc->model, sr->renderer, view_id,
                                               &model, pal, pal_n, toon_override);

                        if (want_toon) {
                            /* Restore the pre-submit hook to its pre-toon state:
                             * fwdplus cluster-bind when Forward+ is active this frame,
                             * or NULL otherwise.  Restoring to NULL unconditionally
                             * would silently drop cluster lighting for all subsequent
                             * skinned entities in the same draw loop when fp_active_frame
                             * is true (the original bug: fwdplus cb set at :1373 was
                             * never re-armed after the toon character). */
                            jce_model_set_pre_submit_cb(
                                (sr->fp_active_frame || sr->frame_shadow_active) ? sr_model_presubmit_cb : NULL,
                                (sr->fp_active_frame || sr->frame_shadow_active) ? sr : NULL);
                        }
                    }
                    continue;
                }
            }
        }

        if (cfg->draw_opaque &&
            !jce_scene_has_skeletal_animator(scene, e) &&
            sr_try_draw_mesh_renderer_model(sr, scene, e, view_id, &model, cfg,
                                            entity_lod,
                                            sr->ecull ? sr->ecull[i].lod_fade : 1.0f)) {
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

        /* ── Billboard path (camera-facing textured quad) ─────────────
         * Authored-but-never-drawn before this. Reuses the sprite batch (same
         * textured-quad shader — no new shader) with a camera-facing model
         * matrix built from the camera basis (FULL) or world-Y-locked (Y_AXIS).
         * The sprite batch's local quad is the centered unit square in XY, so
         * col0 = right*size.x, col1 = up*size.y places a size-scaled quad. */
        if (cfg->draw_opaque && sr->sprite_batch && camera &&
            jce_scene_has_billboard_renderer(scene, e)) {
            static int s_bb_cid = -2;
            if (s_bb_cid == -2) s_bb_cid = jce_component_find("BillboardRenderer");
            bool bb_on = (s_bb_cid < 0) || jce_scene_comp_enabled(scene, e, s_bb_cid);
            JceBillboardRendererComponent *bb =
                bb_on ? jce_scene_get_billboard_renderer(scene, e) : NULL;
            if (bb && bb->visible) {
                bgfx_texture_handle_t bt = { UINT16_MAX };
                if (bb->texture_path[0]) {
                    JceTexture t = sr_resolve_texture(sr, bb->texture_path);
                    if (jce_texture_valid(t)) bt.idx = t.idx;
                }
                if (!BGFX_HANDLE_IS_VALID(bt)) bt = sr->white_tex;

                jce_vec3 pos = { model.col[3].x, model.col[3].y, model.col[3].z };
                jce_vec3 right, up;
                if (bb->mode == JCE_BILLBOARD_Y_AXIS) {
                    jce_vec3 to_cam = jce_v3_sub(jce_camera_get_position(camera), pos);
                    to_cam.y = 0.0f;
                    float l = jce_v3_len(to_cam);
                    jce_vec3 fwd = (l > 1e-5f) ? (jce_vec3){ to_cam.x/l, 0.0f, to_cam.z/l }
                                               : (jce_vec3){ 0.0f, 0.0f, 1.0f };
                    up    = (jce_vec3){ 0.0f, 1.0f, 0.0f };
                    right = jce_v3_cross(up, fwd);
                } else {
                    right = jce_camera_get_right(camera);
                    up    = jce_camera_get_up(camera);
                }
                float sx = bb->size[0] != 0.0f ? bb->size[0] : 1.0f;
                float sy = bb->size[1] != 0.0f ? bb->size[1] : 1.0f;
                const float *bc = bb->color;
                uint32_t bbgr = ((uint32_t)(bc[3] * 255.0f) << 24)
                              | ((uint32_t)(bc[2] * 255.0f) << 16)
                              | ((uint32_t)(bc[1] * 255.0f) << 8)
                              |  (uint32_t)(bc[0] * 255.0f);
                if (bb->texture_path[0]) {
                    /* Textured: sprite batch (mesh program — shows the texture,
                     * lit; the vertex-color tint is not applied by that shader). */
                    float world[16] = {
                        right.x * sx, right.y * sx, right.z * sx, 0.0f,
                        up.x    * sy, up.y    * sy, up.z    * sy, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        pos.x, pos.y, pos.z, 1.0f
                    };
                    JceTexture bjt; bjt.idx = bt.idx;
                    jce_sprite_batch_add(sr->sprite_batch, bjt, world,
                                         0.0f, 0.0f, 1.0f, 1.0f, bbgr, 0);
                } else {
                    /* Solid colour: camera-facing quad via the color program
                     * (unlit, tinted — renders in the pre-postfx offscreen where
                     * the sprite batch's lit/untinted look would otherwise show). */
                    bgfx_vertex_layout_t bl;
                    bgfx_vertex_layout_begin(&bl, bgfx_get_renderer_type());
                    bgfx_vertex_layout_add(&bl, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                    bgfx_vertex_layout_add(&bl, BGFX_ATTRIB_COLOR0,   4, BGFX_ATTRIB_TYPE_UINT8, true, false);
                    bgfx_vertex_layout_end(&bl);
                    bgfx_transient_vertex_buffer_t bvb;
                    bgfx_transient_index_buffer_t  bib;
                    if (bgfx_alloc_transient_buffers(&bvb, &bl, 4, &bib, 6, false)) {
                        struct BbVtx { float x, y, z; uint32_t abgr; };
                        struct BbVtx *bv = (struct BbVtx *)bvb.data;
                        uint16_t *bidx = (uint16_t *)bib.data;
                        jce_vec3 rx = jce_v3_scale(right, sx * 0.5f);
                        jce_vec3 uy = jce_v3_scale(up,    sy * 0.5f);
                        jce_vec3 c0 = jce_v3_sub(jce_v3_sub(pos, rx), uy);
                        jce_vec3 c1 = jce_v3_sub(jce_v3_add(pos, rx), uy);
                        jce_vec3 c2 = jce_v3_add(jce_v3_add(pos, rx), uy);
                        jce_vec3 c3 = jce_v3_add(jce_v3_sub(pos, rx), uy);
                        bv[0].x=c0.x; bv[0].y=c0.y; bv[0].z=c0.z; bv[0].abgr=bbgr;
                        bv[1].x=c1.x; bv[1].y=c1.y; bv[1].z=c1.z; bv[1].abgr=bbgr;
                        bv[2].x=c2.x; bv[2].y=c2.y; bv[2].z=c2.z; bv[2].abgr=bbgr;
                        bv[3].x=c3.x; bv[3].y=c3.y; bv[3].z=c3.z; bv[3].abgr=bbgr;
                        bidx[0]=0; bidx[1]=1; bidx[2]=2; bidx[3]=0; bidx[4]=2; bidx[5]=3;
                        bgfx_set_transient_vertex_buffer(0, &bvb, 0, 4);
                        bgfx_set_transient_index_buffer(&bib, 0, 6);
                        jce_mat4 bid = jce_m4_identity();
                        bgfx_set_transform(bid.raw[0], 1);
                        bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                       BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                                       BGFX_STATE_BLEND_ALPHA, 0);
                        JceShaderHandle csh = jce_renderer_get_program_color(sr->renderer);
                        bgfx_program_handle_t cprog; cprog.idx = csh.idx;
                        if (BGFX_HANDLE_IS_VALID(cprog))
                            bgfx_submit(view_id, cprog, 0, BGFX_DISCARD_ALL);
                    }
                }
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

        /* (Occlusion proxy query already submitted right after the visibility
         * test above — BEFORE the per-draw-path `continue`s — so it covers every
         * entity regardless of which draw path it took.) */
    }

    /* Collect occlusion stats from the culler after entity loop. */
    if (cfg->occlusion_culler)
        sr->stat_occlusion = jce_occlusion_culler_get_stats(cfg->occlusion_culler);

    /* Flush the per-model GPU-instancing batch (opaque) — one instanced submit
     * per model, before the transparent queue so opaque depth is laid down
     * first.  The Forward+ pre-submit hook is still armed here. */
    sr_inst_flush(sr, view_id);

    /* Flush the octahedral impostor cards (terminal LOD, P2 #10): one instanced
     * draw per atlas.  Opaque alpha-tested, so before the transparent queue. */
    sr_impostor_flush(sr, view_id);

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

    /* Flush sprite batch whenever it has content — billboards add even when
     * draw_sprites is off, so gate on count, not draw_sprites. */
    if (sr->sprite_batch && jce_sprite_batch_count(sr->sprite_batch) > 0)
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

    /* Streaming cross-fade (Direction B): prune fade state to the entities seen
     * THIS color pass so the table tracks the active streamed set and a despawn
     * + later respawn re-fades.  Done here (end of the pass) so every fade touch
     * above is already recorded.  No-op until something has streamed in. */
    sr_fade_state_prune(sr);
}
