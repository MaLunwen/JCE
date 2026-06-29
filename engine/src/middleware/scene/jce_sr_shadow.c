/*
 * jce_sr_shadow.c  Scene-renderer shadow module (split from
 * jce_scene_renderer.c).
 *
 * Shadow-caster per-cascade culling + caster AABB, the shadow VP / CSM
 * helpers, the directional shadow target lifecycle, the per-frame
 * shadow-caster spatial grid, and the directional + local (spot/point)
 * shadow passes.  Pure move from the monolithic renderer: cross-module entry
 * points are declared in jce_sr_internal.h, everything else stays file-static
 * here.  No behaviour change.
 */

#include "jce_sr_internal.h"

/* ── Shadow-caster per-cascade culling ───────────────────────────────
 * The directional shadow pass submits every caster to every cascade, which
 * blows up per-frame uniform writes (bgfx's fixed Vulkan uniform scratch) on
 * large scenes.  Standard-engine fix: cull a caster from a cascade when its
 * light-space XY footprint lies outside that cascade's coverage.  Under the
 * cascade's orthographic light projection a caster and the ground its shadow
 * lands on share the same light-space XY (moving along the light axis doesn't
 * change the right/up projection), so XY-footprint culling drops nothing
 * visible — no shadow truncation. */

/* World AABB of an entity for shadow-caster culling.  Handles skinned + static
 * glTF models (the bulk).  Returns false for primitives/terrain/unresolvable
 * (the caller then does NOT cull them — never drop an unknown-bounds caster). */
bool sr_shadow_caster_aabb(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, const jce_mat4 *world,
                                  jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    if (!jce_scene_has_transform(scene, e)) return false;
    const char *path = NULL;
    if (jce_scene_has_skeletal_animator(scene, e)) {
        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (sa && sa->skeleton_path[0]) path = sa->skeleton_path;
    }
    if (!path) path = sr_mesh_renderer_model_path(scene, e);
    if (!path) return false;
    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;
    float lmn[3], lmx[3];
    if (!jce_model_get_aabb(mc->model, lmn, lmx)) return false;
    /* #8 — reuse the world matrix the caller already composed (ecull build) to
     * skip a redundant parent-chain walk; fall back if none supplied. */
    jce_mat4 model = world ? *world : jce_scene_get_world_matrix(scene, e);
    sr_transform_aabb(&model, jce_v3(lmn[0], lmn[1], lmn[2]),
                      jce_v3(lmx[0], lmx[1], lmx[2]), out_mn, out_mx);
    return true;
}

/* True when the caster's world AABB is fully outside cascade `cc`±`radius`'s
 * light-space XY coverage (+ pad).  No light-axis test => casters between the
 * cascade and the sun are kept (their shadow still falls inside the cascade). */
static bool sr_caster_culled_for_cascade(const jce_vec3 *wmn, const jce_vec3 *wmx,
                                         jce_vec3 right_ws, jce_vec3 up_ws,
                                         jce_vec3 cc, float radius)
{
    const float pad = 1.0f;
    float lim = radius + pad;
    float ccx = jce_v3_dot(cc, right_ws);
    float ccy = jce_v3_dot(cc, up_ws);
    float xmn = 3.4e38f, xmx = -3.4e38f, ymn = 3.4e38f, ymx = -3.4e38f;
    for (int k = 0; k < 8; k++) {
        jce_vec3 cr = jce_v3((k & 1) ? wmx->x : wmn->x,
                             (k & 2) ? wmx->y : wmn->y,
                             (k & 4) ? wmx->z : wmn->z);
        float px = jce_v3_dot(cr, right_ws);
        float py = jce_v3_dot(cr, up_ws);
        if (px < xmn) xmn = px; if (px > xmx) xmx = px;
        if (py < ymn) ymn = py; if (py > ymx) ymx = py;
    }
    if (xmx < ccx - lim || xmn > ccx + lim) return true;
    if (ymx < ccy - lim || ymn > ccy + lim) return true;
    return false;
}

/* Per-renderer "Cast Shadows" (Unity-style): an entity whose MeshRenderer has
 * shadow_cast_off set is skipped by EVERY shadow producer pass (CSM/dir +
 * local atlas). Entities without a MeshRenderer keep the default (cast on). */
bool sr_entity_casts_shadow(JceScene *scene, JceEntity e)
{
    if (!jce_scene_has_mesh_renderer(scene, e)) return true;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    return !(mr && mr->shadow_cast_off);
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

/* Cascade-blend ratio honoring the shadow filter tier: tier 0 forces the
 * blend off, and the shader's existing `blend > 0.0001` guard then skips
 * the entire second-cascade PCF block with zero shader changes. */
static float sr_effective_csm_blend(const JceSceneRenderer *sr)
{
    return (sr->shadow_filter_tier < 0.5f) ? 0.0f : sr->csm_blend_ratio;
}

static void sr_bind_shadow_params(JceSceneRenderer *sr, float inv_map_size)
{
    if (!sr) return;
    float disabled_splits[4] = { 0, 0, 0, 0 };
    float params[4] = { inv_map_size, sr_effective_csm_blend(sr),
                        sr->csm_normal_bias, sr->csm_filter_radius };
    float bias_scales[4] = { 1, 1, 1, 1 };
    bgfx_set_uniform(sr->u_csm_splits, disabled_splits, 1);
    bgfx_set_uniform(sr->u_csm_params, params, 1);
    bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);
}

static void sr_bind_shadow_uniforms_disabled(JceSceneRenderer *sr)
{
    sr_bind_shadow_params(sr, 0.0f);
}

/* P1 — bind the local (spot) shadow atlas + per-spot slot table. Bound for
 * every material run alongside the directional shadow state; slots default to
 * -1 (shader skips) when no spot casts a shadow. Stage 15 is shared with
 * terrain's layer2 — terrain rebinds 15 after this, so terrain receives only
 * directional shadows (fs_pbr.sc samples local shadows; fs_terrain.sc does not). */
static void sr_bind_local_shadow_state(JceSceneRenderer *sr)
{
    if (!BGFX_HANDLE_IS_VALID(sr->u_local_shadow_map)) return;

    bgfx_texture_handle_t tex = (sr->local_atlas_valid
                                 && BGFX_HANDLE_IS_VALID(sr->local_atlas_tex))
        ? sr->local_atlas_tex : sr->shadow_tex;
    if (BGFX_HANDLE_IS_VALID(tex))
        bgfx_set_texture(15, sr->u_local_shadow_map, tex, UINT32_MAX);

    bgfx_set_uniform(sr->u_local_shadow_vp, sr->frame_local_vp[0].raw[0],
                     JCE_MAX_LOCAL_SHADOWS);

    float slots[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    if (sr->frame_local_active) {
        for (int i = 0; i < JCE_MAX_SPOT_LIGHTS && i < 4; i++)
            slots[i] = sr->frame_spot_slot[i];
    }
    bgfx_set_uniform(sr->u_spot_shadow_slot, slots, 1);

    float pslots[16];   /* 16 point lanes -> 4 vec4 */
    for (int i = 0; i < JCE_MAX_POINT_LIGHTS && i < 16; i++)
        pslots[i] = sr->frame_local_active ? sr->frame_point_slot[i] : -1.0f;
    bgfx_set_uniform(sr->u_point_shadow_slot, pslots, 4);

    float inv_atlas = sr->shadow_map_size > 0
        ? 1.0f / (float)sr->shadow_map_size : 0.0f;
    /* tiles/side follows the active grid (2 default, 6 when point cube shadows
       are on) so the shader's tile-rect math matches the producer. */
    float tiles_side = (float)(sr->local_tiles ? sr->local_tiles
                                               : JCE_LOCAL_SHADOW_TILES);
    float params[4] = { tiles_side, inv_atlas,
                        sr->frame_local_bias, inv_atlas };
    bgfx_set_uniform(sr->u_local_shadow_params, params, 1);

    /* Per-slot depth bias (lane i = atlas slot i). 4 floats = one vec4. Lets each
       shadow-casting light use its authored shadowBias instead of one global. */
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_bias))
        bgfx_set_uniform(sr->u_local_shadow_bias, sr->frame_local_bias_slot, 1);

    /* #7 omnidirectional point cube shadows: per-point face-0 slot table + the
       6-face VPs. Off / no-cube => lanes all -1, so the shader takes the legacy
       single-tile u_pointShadowSlot path (byte-identical default). */
    if (BGFX_HANDLE_IS_VALID(sr->u_point_cube_base_slot)) {
        float cbslots[16];
        for (int i = 0; i < JCE_MAX_POINT_LIGHTS && i < 16; i++)
            cbslots[i] = (sr->frame_local_active && sr->point_cube_shadows)
                       ? sr->frame_point_cube_base_slot[i] : -1.0f;
        bgfx_set_uniform(sr->u_point_cube_base_slot, cbslots, 4);
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_point_cube_vp))
        bgfx_set_uniform(sr->u_point_cube_vp, sr->frame_point_cube_vp[0].raw[0],
                         JCE_POINT_SHADOW_MAX * JCE_POINT_CUBE_FACES);
}

void sr_bind_frame_shadow_state(JceSceneRenderer *sr)
{
    if (!sr) return;
    /* Filter tier first — both the CSM and local-shadow shader paths
       branch on it (frame-constant, fully coherent per draw). */
    if (BGFX_HANDLE_IS_VALID(sr->u_shadow_quality)) {
        float q[4] = { sr->shadow_filter_tier, 0.0f, 0.0f, 0.0f };
        bgfx_set_uniform(sr->u_shadow_quality, q, 1);
    }
    sr_bind_local_shadow_state(sr);
    if (!sr->frame_shadow_active) {
        sr_bind_shadow_uniforms_disabled(sr);
        return;
    }

    if (sr->shadow_use_csm && sr->last_csm_valid) {
        const JceCsmData *csm = &sr->last_csm;
        bgfx_set_uniform(sr->u_csm_vp, csm->vp[0].raw[0], (uint16_t)csm->cascade_count);
        float splits_v4[4] = { 0, 0, 0, 0 };
        for (uint32_t ci = 0; ci < csm->cascade_count && ci < 4; ci++)
            splits_v4[ci] = csm->splits[ci + 1];
        bgfx_set_uniform(sr->u_csm_splits, splits_v4, 1);

        float csm_params[4] = { sr->shadow_map_size > 0 ? 1.0f / (float)sr->shadow_map_size : 0.0f,
            sr_effective_csm_blend(sr), sr->csm_normal_bias, sr->csm_filter_radius };
        bgfx_set_uniform(sr->u_csm_params, csm_params, 1);

        float bias_scales[4];
        sr_fill_csm_bias_scales(csm, bias_scales);
        bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);

        for (uint32_t ci = 0;
             ci < sr->csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
            bgfx_set_texture((uint8_t)(9 + ci), sr->u_csm_samplers[ci], sr->csm_tex[ci], UINT32_MAX);
        return;
    }

    if (!sr->shadow_use_csm && sr->frame_shadow_vp_valid && BGFX_HANDLE_IS_VALID(sr->shadow_tex)) {
        bgfx_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
        bgfx_set_uniform(sr->u_shadowVP, sr->frame_shadow_vp, 1);
        sr_bind_shadow_params(sr, sr->shadow_map_size > 0
                              ? 1.0f / (float)sr->shadow_map_size : 0.0f);
        return;
    }

    sr_bind_shadow_uniforms_disabled(sr);
}

/* #7: omnidirectional point-shadow view-band gate. Mirrors the producer gate in
   sr_draw_local_shadow_pass — r.point_shadows cvar (idempotent re-register
   returns the existing handle, so no `sr` needed here) OR the
   JCE_POINT_CUBE_SHADOWS env, AND shadows enabled. */
static bool point_cube_views_on(const JceSceneRenderConfig *cfg)
{
    if (!cfg || !cfg->draw_shadows) return false;
    JceCvar *cv = jce_cvar_register_bool("r.point_shadows", false,
                                         JCE_CVAR_FLAG_NONE, "");
    return (cv && jce_cvar_get_bool(cv))
        || (getenv("JCE_POINT_CUBE_SHADOWS") != NULL);
}

void sr_apply_view_order(uint16_t view_id_base,
                                const JceSceneRenderConfig *cfg,
                                uint32_t csm_cascade_count,
                                bool gpu_cull_view)
{
    JceSceneRendererViewOrder order;
    bool include_fog_views = cfg && cfg->fog_enabled;
    uint8_t cascades = csm_cascade_count > JCE_CSM_MAX_CASCADES
        ? JCE_CSM_MAX_CASCADES : (uint8_t)csm_cascade_count;

    if (!jce_scene_renderer_view_order_build(
            view_id_base,
            cfg && cfg->draw_shadows,
            cascades,
            include_fog_views,
            jce_render_pipeline_is_feature_enabled("gpu_particles"),
            gpu_cull_view,          /* GPU-cull compute view (shares base+9) */
            /* #7: cube point-shadow tile band (base+100..) — SAME gate as the
               producer (r.point_shadows cvar [idempotent re-register returns the
               existing handle] OR the JCE_POINT_CUBE_SHADOWS env). */
            point_cube_views_on(cfg),
            &order))
        return;

    bgfx_set_view_order(order.first, order.count, order.order);
}

/* ── Shadow pass ──────────────────────────────────────────────────── */

void sr_destroy_shadow_targets(JceSceneRenderer *sr)
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
    if (BGFX_HANDLE_IS_VALID(sr->local_atlas_fbo)) {
        bgfx_destroy_frame_buffer(sr->local_atlas_fbo);
        sr->local_atlas_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->local_atlas_tex)) {
        bgfx_destroy_texture(sr->local_atlas_tex);
        sr->local_atlas_tex.idx = UINT16_MAX;
    }
    sr->local_atlas_valid = false;
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

void sr_create_shadow_targets(JceSceneRenderer *sr)
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

    /* Local (spot/point) shadow atlas: one square depth texture, NxN tiles. */
    sr->local_atlas_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL);
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, sr->local_atlas_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    sr->local_atlas_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    sr->local_atlas_valid = BGFX_HANDLE_IS_VALID(sr->local_atlas_fbo);
}

void sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size)
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
    sr->shadow_far_valid = false;
}

/* ── #6 cull-once / consume-many: shadow-caster spatial grid ───────────────
 *
 * Build a uniform grid ONCE per frame over the shadow-casting entities (using
 * the world AABBs the ecull cache already computed) so each CSM cascade can
 * gather only the casters that overlap its bounds, instead of re-scanning the
 * entire entity list per cascade.  Casters whose AABB is unknown (has_aabb ==
 * false: primitives / terrain / unresolvable) are NOT inserted — they are
 * collected into shadow_noaabb[] and visited by EVERY cascade unconditionally,
 * exactly as the full-list scan did (never drop an unknown-bounds caster).
 *
 * Returns false if the grid could not be built (caller falls back to the
 * legacy full-list scan — never a correctness change, only a perf miss). */
static bool sr_build_shadow_space(JceSceneRenderer *sr, EntityList *list)
{
    sr->shadow_space_valid   = false;
    sr->shadow_noaabb_count  = 0;
    if (!sr->ecull || list->count <= 0) return false;

    /* Grow the no-aabb + query scratch arrays (worst case = every entity). */
    if (sr->shadow_noaabb_cap < (uint32_t)list->count) {
        uint32_t nc = sr->shadow_noaabb_cap ? sr->shadow_noaabb_cap : 64u;
        while (nc < (uint32_t)list->count) nc *= 2u;
        uint32_t *g = (uint32_t *)JCE_REALLOC(sr->shadow_noaabb,
                                              nc * sizeof(uint32_t));
        if (!g) return false;
        sr->shadow_noaabb     = g;
        sr->shadow_noaabb_cap = nc;
    }
    if (sr->shadow_query_cap < (uint32_t)list->count) {
        uint32_t nc = sr->shadow_query_cap ? sr->shadow_query_cap : 64u;
        while (nc < (uint32_t)list->count) nc *= 2u;
        uint32_t *g = (uint32_t *)JCE_REALLOC(sr->shadow_query,
                                              nc * sizeof(uint32_t));
        if (!g) return false;
        sr->shadow_query     = g;
        sr->shadow_query_cap = nc;
    }

    /* World bounds over the caster AABBs (the grid needs finite bounds). */
    jce_vec3 wmin = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    uint32_t aabb_casters = 0;
    for (int i = 0; i < list->count; i++) {
        if (!sr->ecull[i].casts_shadow) continue;
        if (!sr->ecull[i].has_aabb) {
            sr->shadow_noaabb[sr->shadow_noaabb_count++] = (uint32_t)i;
            continue;
        }
        const jce_vec3 bmn = sr->ecull[i].wmin, bmx = sr->ecull[i].wmax;
        if (bmn.x < wmin.x) wmin.x = bmn.x;
        if (bmn.y < wmin.y) wmin.y = bmn.y;
        if (bmn.z < wmin.z) wmin.z = bmn.z;
        if (bmx.x > wmax.x) wmax.x = bmx.x;
        if (bmx.y > wmax.y) wmax.y = bmx.y;
        if (bmx.z > wmax.z) wmax.z = bmx.z;
        aabb_casters++;
    }
    if (aabb_casters == 0) {
        /* No AABB casters to grid; the no-aabb list (if any) is still consumed
         * by the cascade loops, so report "no grid" and let them iterate it. */
        return false;
    }

    const float pad = 1.0f;
    wmin.x -= pad; wmin.y -= pad; wmin.z -= pad;
    wmax.x += pad; wmax.y += pad; wmax.z += pad;
    JceAABB world = { wmin, wmax };

    if (!sr->shadow_space) {
        JceSpaceConfig cfg = { 0 };
        cfg.type        = JCE_SPACE_GRID;
        cfg.world_bounds = world;
        cfg.max_objects = (uint32_t)list->count;
        sr->shadow_space = jce_space_create(&cfg);
    } else {
        jce_space_reset(sr->shadow_space, &world);
    }
    if (!sr->shadow_space) return false;

    for (int i = 0; i < list->count; i++) {
        if (!sr->ecull[i].casts_shadow || !sr->ecull[i].has_aabb) continue;
        JceAABB b = { sr->ecull[i].wmin, sr->ecull[i].wmax };
        jce_space_insert(sr->shadow_space, b, (uint32_t)i);
    }
    sr->shadow_space_valid = true;
    return true;
}

/* Conservative WORLD AABB enclosing every caster a cascade could need.  The
 * fine cull (sr_caster_culled_for_cascade) keeps a caster iff its LIGHT-SPACE
 * XY footprint overlaps the cascade's XY coverage, with NO light-axis test —
 * casters offset along the light direction are kept (their shadow still lands
 * in the cascade).  So the broadphase box must cover the cascade's XY coverage
 * (radius + pad + a safety margin) AND span the full scene extent along the
 * light axis.  Built in light space then transformed back to a world AABB, this
 * is a guaranteed superset of the fine-cull keep-set: the grid never drops a
 * caster the fine cull would have kept. */
static void sr_cascade_world_aabb(jce_vec3 cc, float radius,
                                  jce_vec3 right_ws, jce_vec3 up_ws,
                                  jce_vec3 fwd_ws,
                                  jce_vec3 scene_min, jce_vec3 scene_max,
                                  bool scene_valid,
                                  jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    /* Light-space XY half-extent: cascade radius + the fine cull's pad (1.0)
     * + a generous safety margin so a caster straddling the coverage edge is
     * never broadphase-dropped before the fine cull can judge it. */
    const float xy_half = radius + 1.0f + 4.0f;
    const float ccx = jce_v3_dot(cc, right_ws);
    const float ccy = jce_v3_dot(cc, up_ws);

    /* Light-axis (forward) span: cover the whole scene caster extent so a
     * caster between the cascade and the sun (or behind it) is never dropped.
     * Fall back to a large symmetric span around the cascade if bounds unknown.
     * Always UNION the cascade's own light-axis neighbourhood so a stale/empty
     * scene-bounds value can never under-cover this cascade. */
    const float ccf = jce_v3_dot(cc, fwd_ws);
    float fmn, fmx;
    if (scene_valid) {
        fmn = +FLT_MAX; fmx = -FLT_MAX;
        for (int k = 0; k < 8; k++) {
            jce_vec3 cr = jce_v3((k & 1) ? scene_max.x : scene_min.x,
                                 (k & 2) ? scene_max.y : scene_min.y,
                                 (k & 4) ? scene_max.z : scene_min.z);
            float pf = jce_v3_dot(cr, fwd_ws);
            if (pf < fmn) fmn = pf;
            if (pf > fmx) fmx = pf;
        }
        fmn -= 1.0f; fmx += 1.0f;
    } else {
        const float big = 1.0e6f;
        fmn = ccf - big; fmx = ccf + big;
    }
    /* Guard against stale/under-covering scene bounds. */
    const float cc_reach = radius + xy_half;
    if (ccf - cc_reach < fmn) fmn = ccf - cc_reach;
    if (ccf + cc_reach > fmx) fmx = ccf + cc_reach;

    /* 8 corners of the light-space box → world AABB. */
    jce_vec3 mn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 mx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int k = 0; k < 8; k++) {
        float lx = (k & 1) ? (ccx + xy_half) : (ccx - xy_half);
        float ly = (k & 2) ? (ccy + xy_half) : (ccy - xy_half);
        float lf = (k & 4) ? fmx : fmn;
        jce_vec3 w = jce_v3_add(jce_v3_add(jce_v3_scale(right_ws, lx),
                                           jce_v3_scale(up_ws, ly)),
                                jce_v3_scale(fwd_ws, lf));
        if (w.x < mn.x) mn.x = w.x; if (w.x > mx.x) mx.x = w.x;
        if (w.y < mn.y) mn.y = w.y; if (w.y > mx.y) mx.y = w.y;
        if (w.z < mn.z) mn.z = w.z; if (w.z > mx.z) mx.z = w.z;
    }
    /* Final world-space pad: floating-point edge safety for the overlap test. */
    const float wpad = 1.0f;
    mn.x -= wpad; mn.y -= wpad; mn.z -= wpad;
    mx.x += wpad; mx.y += wpad; mx.z += wpad;
    *out_mn = mn;
    *out_mx = mx;
}

void sr_draw_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                const JceCamera *camera, EntityList *list,
                                uint16_t view_id_base, uint32_t vp_w,
                                uint32_t vp_h, float shadow_distance,
                                float split_lambda)
{
    sr->shadow_use_csm = false; sr->last_csm_valid = false;
    sr->frame_shadow_active = false; sr->frame_shadow_vp_valid = false;
    /* GPU-driven shadow context invalid until the CSM cascade planes are set
     * below; sr_sh_flush keys off this to know which (if any) cascade a shadow
     * view belongs to.  Cleared here so the non-CSM / early-return paths never
     * leave a stale per-cascade plane set behind. */
    sr->gpu_shadow_planes_valid = false;
    sr_bind_shadow_uniforms_disabled(sr);

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
    jce_vec3 shadow_dir;
    if (!sr_resolve_primary_dir_light(sr, scene, list, true, &shadow_dir, NULL, NULL))
        return;

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
        bgfx_touch(shadow_view_0);

        memcpy(sr->frame_shadow_vp, shadow_vp, sizeof(shadow_vp));
        sr->frame_shadow_vp_valid = true; sr->frame_shadow_active = true;

        if (use_rq_shadow) {
            jce_rq_clear(sr->render_queue);
            jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
            for (int i = 0; i < list->count; i++) {
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (!sr->ecull[i].casts_shadow) continue;
                if (sr->ecull[i].lod_culled) continue;  /* LODGroup far-cull (P1 #6) */
                if (sr_try_submit_skinned_shadow(sr, scene, e, i, shadow_view_0))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, shadow_view_0))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
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
                /* Depth-only shadow state (matches jce_mesh_submit_shadow). */
                cmd.state          = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                   | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
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
            if (!sr->ecull[i].casts_shadow) continue;
            if (sr->ecull[i].lod_culled) continue;  /* LODGroup far-cull (P1 #6) */
            if (sr_try_submit_skinned_shadow(sr, scene, e, i, shadow_view_0))
                continue;
            if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, shadow_view_0))
                continue;
            if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0))
                continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
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

    /* CSM cascade NEAR is a STABLE, camera-independent constant — deliberately
     * NOT the camera's z-precision near plane.  The editor orbit camera sets its
     * near to orbit_distance*0.001 (orbit_apply), so it scales with zoom; feeding
     * that into the cascade splits (split[i] = near*(far/near)^(i/N)) made every
     * cascade's world size — hence its shadow-map texel size — change on every
     * zoom frame, defeating the texel-snap stabilisation and producing the
     * shimmer/flicker the user saw while zooming (measured: 126 cascade
     * re-partitions in a single zoom sweep).  With shadow_far (below) already
     * coarse-bucketed stable, a fixed near keeps the whole cascade range constant
     * under zoom → stable, texel-snapped shadows.  Standard practice: shadow
     * cascades use a stable quality-driven range, not the camera dynamic near. */
    (void)cam_near;
    float shadow_near = JCE_CSM_SHADOW_NEAR;

    /* Shadow distance must never be CAPPED by the camera's far plane.  The
     * editor orbit camera's far plane scales with zoom (orbit_distance*100 in
     * orbit_apply), so an old min(cam_far, shadow_distance) made the shadow
     * cutoff SHRINK as the user zoomed in: normal models only a bit far stopped
     * casting/receiving shadows ("远处没有/远一点就不投影").  The cascades cover
     * [near, shadow_far] and the visible sub-range simply samples whichever
     * cascades fall in it (a far plane > the view far is harmless: out-of-view
     * cascade range is unused, and the caster cull keeps only casters whose
     * light-space XY lands inside a cascade).  So shadow_far may only be GROWN
     * by the view (for aerial reach, below), never shrunk by it. */
    float shadow_far_target;
    if (shadow_distance > 0.0f) {
        shadow_far_target = fmaxf(shadow_distance, shadow_near + 1.0f);
    } else {
        shadow_far_target = fmaxf(shadow_near * CSM_DIST_SCALE, CSM_DIST_MAX);
        shadow_far_target = fmaxf(shadow_far_target,
                                  shadow_near + CSM_DIST_MIN);
    }

    /* ZOOM-OUT SHADOW REACH: the authored shadow distance is tuned for
     * gameplay / street-level framing.  At an extreme zoomed-out (aerial)
     * view the visible ground extends FAR past that distance, so the bulk of
     * the frame falls beyond [shadow_near, shadow_far] and renders fully lit:
     * the terrain washes out and the boundary between the shadowed near-region
     * and the unshadowed far-region (and the unshadowed dark building walls)
     * reads as a curved dark/bright band sweeping the ground.  This only shows
     * at extreme aerial zoom — at gameplay zoom the whole view is already
     * inside the authored distance.
     *
     * Fix: let the shadow distance GROW with how far the camera can see, but
     * never SHRINK below the authored distance (so street-level keeps its crisp
     * tuned range) and only in COARSE power-of-two buckets (so it changes in a
     * few discrete jumps across a full zoom sweep instead of wobbling every
     * frame — preserving the texel-snap stability the fixed distance was
     * protecting).  cam_far is the camera's own view reach (the editor orbit
     * camera sets it to orbit_distance*100); a small fraction of it is a clean,
     * camera-pose-independent proxy for the visible ground extent.  The coarse
     * bucketing below + the existing deadband tracking keep it shimmer-free. */
    {
        float view_reach = cam_far * 0.25f;   /* visible-ground extent proxy */
        /* Never reach past the scene itself — shadowing empty space beyond the
         * casters is wasted precision and needlessly inflates cascade 3. */
        if (sr->scene_world_valid) {
            jce_vec3 d = jce_v3_sub(sr->scene_world_max, sr->scene_world_min);
            float scene_diag = jce_v3_len(d);
            if (scene_diag > 1.0f && view_reach > scene_diag)
                view_reach = scene_diag;
        }
        if (view_reach > shadow_far_target) {
            /* Snap up to the next power-of-two multiple of the authored distance
             * so the value only takes a handful of discrete steps across a full
             * zoom sweep (no per-frame wobble => texel-snap stability holds). */
            float bucket = shadow_far_target > 1.0f ? shadow_far_target : 1.0f;
            while (bucket < view_reach) bucket *= 2.0f;
            shadow_far_target = bucket;
        }
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

    /* Skip the per-cascade frustum/sphere/matrix recompute when none of the
       inputs changed since last frame (static camera + light). The key compare
       (~25 float eqs + one mat4 memcmp) is far cheaper than jce_csm_compute().
       Exact compare is safe: identical inputs are bit-identical (jce_camera_view
       is deterministic, shadow_far is coarse-bucketed) and any change recomputes. */
    JceCsmData csm;
    bool csm_changed =
        !sr->csm_key.valid ||
        sr->csm_key.cascades != sr->csm_cascade_count ||
        sr->csm_key.map_size != sr->shadow_map_size ||
        sr->csm_key.homog    != sr->homogeneous_depth ||
        sr->csm_key.znear    != shadow_near ||
        sr->csm_key.zfar     != shadow_far ||
        sr->csm_key.fov      != cam_fov ||
        sr->csm_key.aspect   != aspect ||
        sr->csm_key.lambda   != split_lambda ||
        sr->csm_key.light_dir.x != light_dir.x ||
        sr->csm_key.light_dir.y != light_dir.y ||
        sr->csm_key.light_dir.z != light_dir.z ||
        sr->csm_key.caster_valid != sr->scene_world_valid ||
        (sr->scene_world_valid &&
            (sr->csm_key.caster_min.x != sr->scene_world_min.x ||
             sr->csm_key.caster_min.y != sr->scene_world_min.y ||
             sr->csm_key.caster_min.z != sr->scene_world_min.z ||
             sr->csm_key.caster_max.x != sr->scene_world_max.x ||
             sr->csm_key.caster_max.y != sr->scene_world_max.y ||
             sr->csm_key.caster_max.z != sr->scene_world_max.z)) ||
        memcmp(&sr->csm_key.view, &cam_view, sizeof(jce_mat4)) != 0;

    if (csm_changed) {
        jce_csm_compute(&csm, sr->csm_cascade_count,
                        shadow_near, shadow_far, cam_fov, aspect,
                        &cam_view, &light_dir,
                        sr->homogeneous_depth, sr->shadow_map_size,
                        split_lambda,
                        sr->scene_world_valid ? &sr->scene_world_min : NULL,
                        sr->scene_world_valid ? &sr->scene_world_max : NULL);
        sr->csm_key.valid     = true;
        sr->csm_key.cascades  = sr->csm_cascade_count;
        sr->csm_key.map_size  = sr->shadow_map_size;
        sr->csm_key.homog     = sr->homogeneous_depth;
        sr->csm_key.znear     = shadow_near;
        sr->csm_key.zfar      = shadow_far;
        sr->csm_key.fov       = cam_fov;
        sr->csm_key.aspect    = aspect;
        sr->csm_key.lambda    = split_lambda;
        sr->csm_key.light_dir = light_dir;
        sr->csm_key.view      = cam_view;
        sr->csm_key.caster_valid = sr->scene_world_valid;
        sr->csm_key.caster_min   = sr->scene_world_min;
        sr->csm_key.caster_max   = sr->scene_world_max;
    } else {
        csm = sr->last_csm;
    }

    sr->last_csm = csm;
    sr->last_csm_valid = true;
    sr->frame_shadow_active = true;

    /* GPU-driven shadow cull (roadmap #18, shadow extension): when the GPU path
     * is live this frame, extract each cascade's 6 frustum planes from its light
     * view-proj (Gribb-Hartmann, same extractor the color cull uses) and publish
     * the cascade-view→slot mapping so sr_sh_flush routes each cascade's static
     * instanced casters through that cascade's GPUScene compute cull + indirect
     * draw instead of the CPU per-instance instanced submit.  Skinned / terrain /
     * no-AABB casters stay on their existing CPU paths. */
    if (sr->gpu_driven_frame) {
        uint32_t nc = csm.cascade_count < JCE_CSM_MAX_CASCADES
                    ? csm.cascade_count : JCE_CSM_MAX_CASCADES;
        for (uint32_t c = 0; c < nc; c++)
            sr_extract_frustum_planes(&csm.vp[c], sr->gpu_shadow_planes[c]);
        sr->gpu_shadow_view0    = (uint16_t)(view_id_base + 11);
        sr->gpu_shadow_cascades = nc;
        sr->gpu_shadow_planes_valid = (nc > 0);
    }

    /* World-anchored light-space basis (mirrors jce_csm.c) used by the
     * per-cascade shadow-caster culling in the loops below. */
    jce_vec3 scull_ld = jce_v3_normalize(light_dir);
    jce_vec3 scull_upref = (fabsf(scull_ld.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);
    jce_vec3 scull_right = jce_v3_normalize(jce_v3_cross(scull_ld, scull_upref));
    jce_vec3 scull_up    = jce_v3_normalize(jce_v3_cross(scull_right, scull_ld));

    /* #6 — build the shadow-caster grid ONCE so each cascade gathers only the
     * casters overlapping its (conservative) bounds.  On failure (or when there
     * are no AABB casters) the cascade falls back to the full-list scan — never a
     * correctness change.  has_grid gates the gather path per cascade. */
    const bool has_grid = sr_build_shadow_space(sr, list);

    /* Per-cascade caster gather: when the grid is available, query the
     * conservative cascade world AABB into sr->shadow_query (a guaranteed
     * superset of the fine-cull keep-set), then UNION the always-visit no-aabb
     * caster list.  The result is written to `idx_buf`/`idx_n` for the cascade
     * body to iterate.  Without the grid, idx_n == -1 signals "scan all". */
    #define SR_CASCADE_GATHER(c, idx_buf, idx_n)                                \
        const uint32_t *idx_buf = NULL;                                         \
        int idx_n = -1; /* -1 => full-list scan */                              \
        if (has_grid && sr->shadow_space_valid) {                              \
            jce_vec3 q_mn, q_mx;                                                \
            sr_cascade_world_aabb(csm.center[c], csm.radius[c],                 \
                                  scull_right, scull_up, scull_ld,             \
                                  sr->scene_world_min, sr->scene_world_max,    \
                                  sr->scene_world_valid, &q_mn, &q_mx);        \
            JceAABB q = { q_mn, q_mx };                                         \
            uint32_t hits = jce_space_query_aabb(sr->shadow_space, q,           \
                                                 sr->shadow_query,             \
                                                 sr->shadow_query_cap);        \
            /* Append the no-aabb casters (disjoint set — never gridded). */    \
            uint32_t total = hits;                                              \
            for (uint32_t n = 0; n < sr->shadow_noaabb_count &&               \
                                 total < sr->shadow_query_cap; n++)            \
                sr->shadow_query[total++] = sr->shadow_noaabb[n];               \
            idx_buf = sr->shadow_query;                                         \
            idx_n   = (int)total;                                               \
        }

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
            bgfx_touch(cv);

            SR_CASCADE_GATHER(c, cand, cand_n);
            const int iter_n = (cand_n >= 0) ? cand_n : list->count;
            for (int k = 0; k < iter_n; k++) {
                const int i = (cand_n >= 0) ? (int)cand[k] : k;
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (!sr->ecull[i].casts_shadow) continue;
                if (sr->ecull[i].lod_culled) continue;  /* LODGroup far-cull (P1 #6) */
                if (sr->ecull[i].has_aabb &&
                    sr_caster_culled_for_cascade(&sr->ecull[i].wmin, &sr->ecull[i].wmax,
                                                 scull_right, scull_up,
                                                 csm.center[c], csm.radius[c]))
                    continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, cv))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
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
                /* Depth-only shadow state (matches jce_mesh_submit_shadow). */
                cmd.state          = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                   | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
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
            bgfx_touch(cv);

            SR_CASCADE_GATHER(c, cand, cand_n);
            const int iter_n = (cand_n >= 0) ? cand_n : list->count;
            for (int k = 0; k < iter_n; k++) {
                const int i = (cand_n >= 0) ? (int)cand[k] : k;
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (!sr->ecull[i].casts_shadow) continue;
                if (sr->ecull[i].lod_culled) continue;  /* LODGroup far-cull (P1 #6) */
                if (sr->ecull[i].has_aabb &&
                    sr_caster_culled_for_cascade(&sr->ecull[i].wmin, &sr->ecull[i].wmax,
                                                 scull_right, scull_up,
                                                 csm.center[c], csm.radius[c]))
                    continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, cv))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
                if (!mesh) continue;
                bgfx_set_transform(model.raw[0], 1);
                jce_mesh_submit_shadow(mesh, sr->renderer, cv);
            }
        }
    }

    #undef SR_CASCADE_GATHER
    sr_bind_frame_shadow_state(sr);
}

/* Render one local-shadow tile: lazily clear the whole atlas once (D3D clears
 * the full target, so per-tile clears would wipe earlier tiles), set up the
 * tile's view (rect + light VP), and submit all casters (reusing the skinned +
 * static depth helpers so animated casters deform their local shadows too).
 * Stores the VP into frame_local_vp[slot]. */
static void sr_local_shadow_render_tile(JceSceneRenderer *sr, JceScene *scene,
                                        EntityList *list, uint16_t view_id_base,
                                        const jce_mat4 *vp, uint32_t slot,
                                        const float *ident, bool *cleared,
                                        uint32_t tiles, uint16_t view_off)
{
    uint16_t tx, ty, tsz;
    if (!jce_local_shadow_atlas_tile(slot, sr->shadow_map_size,
                                     tiles, &tx, &ty, &tsz))
        return;

    if (!*cleared) {
        const uint16_t clear_view = (uint16_t)(view_id_base + view_off);
        bgfx_set_view_rect(clear_view, 0, 0,
                           sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(clear_view, sr->local_atlas_fbo);
        bgfx_set_view_clear(clear_view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
        bgfx_set_view_transform(clear_view, ident, ident);
        bgfx_touch(clear_view);
        *cleared = true;
    }

    const uint16_t lv = (uint16_t)(view_id_base + view_off + 1 + slot);
    bgfx_set_view_rect(lv, tx, ty, tsz, tsz);
    bgfx_set_view_frame_buffer(lv, sr->local_atlas_fbo);
    bgfx_set_view_clear(lv, 0, 0, 1.0f, 0);   /* cleared once above */
    bgfx_set_view_transform(lv, ident, vp->raw[0]);
    bgfx_touch(lv);

    for (int j = 0; j < list->count; j++) {
        JceEntity ee = list->entities[j];
        if (!entity_enabled(scene, ee)) continue;
        if (!sr->ecull[j].casts_shadow) continue;
        if (sr_try_submit_skinned_shadow(sr, scene, ee, j, lv)) continue;
        if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, ee, j, lv)) continue;
        if (sr_try_submit_terrain_shadow(sr, scene, ee, lv)) continue;
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, ee, j, &model, &mesh)) continue;
        if (!mesh) continue;
        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(mesh, sr->renderer, lv);
    }

    /* Spot/legacy slots (0..3) feed u_localShadowVP; cube slots (>=4) store
       their VP in frame_point_cube_vp via the caller, so guard the OOB write. */
    if (slot < JCE_MAX_LOCAL_SHADOWS)
        sr->frame_local_vp[slot] = *vp;
}

/* P1 — local (spot + point) shadow producer pass. Renders up to
 * JCE_MAX_LOCAL_SHADOWS shadow-casting SPOT and POINT lights as perspective
 * depth tiles sharing ONE shadow atlas (1 full-atlas clear at view base+4 +
 * one view per tile at base+5+slot). Spots use their cone FOV aimed along the
 * spot direction; points use a single wide-FOV frustum aimed straight DOWN
 * (v1 hemisphere approximation — good for elevated point lights). Spot/point
 * index alignment with the shader's u_spotLights[]/u_pointLights[] is
 * guaranteed by iterating `list` in the SAME order + caps as the light gather.
 * Reuses the depth submit helpers, so animated casters deform their local
 * shadows too (skinned path). */
void sr_draw_local_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                      EntityList *list, uint16_t view_id_base)
{
    sr->frame_local_active = false;
    sr->frame_local_count  = 0;
    for (uint32_t i = 0; i < JCE_MAX_SPOT_LIGHTS; i++)
        sr->frame_spot_slot[i] = -1.0f;
    for (uint32_t i = 0; i < JCE_MAX_POINT_LIGHTS; i++)
        sr->frame_point_slot[i] = -1.0f;
    for (uint32_t i = 0; i < JCE_MAX_LOCAL_SHADOWS; i++)
        sr->frame_local_bias_slot[i] = 0.0015f;   /* per-slot default */

    /* #7 omnidirectional point shadows (opt-in). Env gate for now; an editor
       feature flag / cvar sets it later. When on, the atlas subdivides 6x6 so a
       point light can claim 6 contiguous cube-face tiles. Default => 2x2, the
       legacy single-downward point tile, byte-identical. */
    sr->point_cube_shadows =
        (sr->cv_point_shadows && jce_cvar_get_bool(sr->cv_point_shadows))
        || (getenv("JCE_POINT_CUBE_SHADOWS") != NULL);
    sr->local_tiles = sr->point_cube_shadows
                    ? JCE_LOCAL_SHADOW_TILES_CUBE : JCE_LOCAL_SHADOW_TILES;
    for (uint32_t i = 0; i < JCE_MAX_POINT_LIGHTS; i++)
        sr->frame_point_cube_base_slot[i] = -1.0f;

    if (!sr->local_atlas_valid || !list) return;
    if (jce_renderer_get_program_shadow(sr->renderer).idx == UINT16_MAX) return;

    const bool homog = sr->homogeneous_depth;
    float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    uint32_t slot    = 0;   /* shared spot+point atlas slot pool */
    bool     cleared = false;
    /* When cube shadows are on, all local tiles relocate to a free view band
       (base+100..) because 16 tiles would overrun base+4..+19 into post/fog. */
    const uint16_t loff = sr->point_cube_shadows
                        ? (uint16_t)JCE_VIEW_LOCAL_SHADOW_CUBE_OFFSET
                        : (uint16_t)JCE_VIEW_LOCAL_SHADOW_OFFSET;
    /* 6 cardinal cube-face directions. ORDER MUST MATCH the shader major-axis
       face pick in fs_pbr_body.sh samplePointCubeShadow: {+X,-X,+Y,-Y,+Z,-Z}. */
    static const float CUBE_DIRS[6][3] = {
        { 1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f},
        { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f,-1.0f} };

    /* Spots — index aligns with u_spotLights[]. */
    uint32_t spot_idx = 0;
    for (int li = 0; li < list->count && slot < JCE_MAX_LOCAL_SHADOWS; li++) {
        JceEntity e = list->entities[li];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_spot_light(scene, e)) continue;
        /* Match the light-gather filter EXACTLY (it skips component-disabled
           lights, jce_scene_renderer.c gather loop) so my_spot stays aligned
           with this light's index in u_spotLights[]; otherwise a disabled spot
           component shifts every later spot's slot and shadows attach to the
           WRONG light. */
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPOT_LIGHT))
            continue;
        if (!sr_light_selected(sr->frame_sel_spot, sr->frame_sel_spot_n, e))
            continue;   /* distance-culled: not in the top-N for this view */
        JceSpotLight *sl = jce_scene_get_spot_light(scene, e);
        if (!sl) continue;
        const uint32_t my_spot = spot_idx++;
        if (my_spot >= JCE_MAX_SPOT_LIGHTS) break;
        if (!sl->casts_shadow) continue;

        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 pos    = xf ? xf->position : sl->position;
        jce_vec3 dir    = sr_light_world_shine_direction(&sl->direction, xf);
        float    radius = sl->radius > 0.0f ? sl->radius : 10.0f;
        float    fov    = 2.0f * acosf(sl->outer_cone_cos);
        jce_mat4 vp = jce_local_shadow_vp(pos, dir, fov,
                                          0.05f * radius, radius, homog);
        sr_local_shadow_render_tile(sr, scene, list, view_id_base,
                                    &vp, slot, ident, &cleared,
                                    sr->local_tiles, loff);
        sr->frame_local_bias_slot[slot] =
            (sl->shadow_bias > 0.0f) ? sl->shadow_bias : 0.0015f;
        sr->frame_spot_slot[my_spot] = (float)slot;
        slot++;
    }

    /* Points — index aligns with u_pointLights[]; share the slot pool. When
       point_cube_shadows is ON a budgeted point claims 6 contiguous cube-face
       tiles (true omni); otherwise (default) a single wide-FOV downward tile
       (v1 hemisphere approximation) — byte-identical to the legacy path. */
    uint32_t point_idx   = 0;
    uint32_t cube_budget = 0;
    const uint32_t max_slot = sr->point_cube_shadows
        ? (uint32_t)(JCE_LOCAL_SHADOW_TILES_CUBE * JCE_LOCAL_SHADOW_TILES_CUBE)
        : (uint32_t)JCE_MAX_LOCAL_SHADOWS;
    for (int li = 0; li < list->count && slot < max_slot; li++) {
        JceEntity e = list->entities[li];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_point_light(scene, e)) continue;
        /* Match the light-gather filter EXACTLY (skip component-disabled) so
           my_point stays aligned with this light's index in u_pointLights[]. */
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_POINT_LIGHT))
            continue;
        if (!sr_light_selected(sr->frame_sel_point, sr->frame_sel_point_n, e))
            continue;   /* distance-culled: not in the top-N for this view */
        JcePointLight *pl = jce_scene_get_point_light(scene, e);
        if (!pl) continue;
        const uint32_t my_point = point_idx++;
        if (my_point >= JCE_MAX_POINT_LIGHTS) break;
        if (!pl->casts_shadow) continue;

        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 pos    = xf ? xf->position : pl->position;
        float    radius = pl->radius > 0.0f ? pl->radius : 10.0f;

        if (sr->point_cube_shadows) {
            /* Omni: 6 perspective cube faces (FOV=PI/2). FIXED reserved layout so
               the shader can map base_slot -> the 6-VP block in u_pointCubeVP:
               slots 0-3 = spots/legacy, 4-9 = cube point 0, 10-15 = cube point 1.
               (Reserving 0-3 even with fewer spots keeps base = 4 + budget*6, so
               vp_index = (base-4) + face is stable.) */
            if (cube_budget >= (uint32_t)JCE_POINT_SHADOW_MAX)
                continue;   /* shadow-casting point budget exhausted -> unshadowed */
            const uint32_t base = (uint32_t)JCE_MAX_LOCAL_SHADOWS
                                + cube_budget * (uint32_t)JCE_POINT_CUBE_FACES;
            if (base + (uint32_t)JCE_POINT_CUBE_FACES > max_slot)
                continue;   /* atlas full */
            for (uint32_t f = 0; f < (uint32_t)JCE_POINT_CUBE_FACES; f++) {
                jce_vec3 d = jce_v3(CUBE_DIRS[f][0], CUBE_DIRS[f][1], CUBE_DIRS[f][2]);
                jce_mat4 vp = jce_local_shadow_vp(pos, d, JCE_POINT_CUBE_FOV,
                                                  0.05f * radius, radius, homog);
                sr_local_shadow_render_tile(sr, scene, list, view_id_base,
                                            &vp, base + f, ident, &cleared,
                                            sr->local_tiles, loff);
                sr->frame_point_cube_vp[cube_budget * (uint32_t)JCE_POINT_CUBE_FACES + f] = vp;
            }
            sr->frame_point_cube_base_slot[my_point] = (float)base;
            cube_budget++;
        } else {
            /* Legacy single wide-FOV downward tile (default path; slot < 4). */
            jce_mat4 vp = jce_local_shadow_vp(pos, jce_v3(0.0f, -1.0f, 0.0f),
                                              JCE_POINT_SHADOW_FOV,
                                              0.05f * radius, radius, homog);
            sr_local_shadow_render_tile(sr, scene, list, view_id_base,
                                        &vp, slot, ident, &cleared,
                                        sr->local_tiles, loff);
            sr->frame_local_bias_slot[slot] =
                (pl->shadow_bias > 0.0f) ? pl->shadow_bias : 0.0015f;
            sr->frame_point_slot[my_point] = (float)slot;
            slot++;
        }
    }

    /* Active if any spot/legacy slot OR any cube point rendered (cube uses a
       fixed reserved region independent of `slot`, so a cube-only frame still
       has slot==0). frame_local_count = highest used slot+1 for consumers. */
    uint32_t used = slot;
    if (cube_budget > 0) {
        uint32_t cube_end = (uint32_t)JCE_MAX_LOCAL_SHADOWS
                          + cube_budget * (uint32_t)JCE_POINT_CUBE_FACES;
        if (cube_end > used) used = cube_end;
    }
    sr->frame_local_count  = used;
    sr->frame_local_active = used > 0;
    sr->frame_local_bias   = 0.0015f;   /* default depth bias; tune by eye */
}
