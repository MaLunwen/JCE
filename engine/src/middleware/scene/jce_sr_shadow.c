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

#include <stdio.h>
#include <string.h>   /* snprintf for the JCE_DBG_CSM_LOG probe */
#include <stdlib.h>  /* getenv */
#include <SDL3/SDL.h> /* SDL_IOStream: engine code does not use stdio file IO */
#include <jce/os/core/jce_frustum.h>
#include "jce_sr_internal.h"
#include <jce/renderer/jce_render_pipeline.h>  /* live cascade count */
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include "renderer/jce_render_encoder.h"
/* The view-band gate below returns bool.  Without this include both entry
 * points were implicitly declared `extern int`, so the caller read all of EAX
 * while the callee only defined AL -- and this is the guard that keeps the
 * scene renderer's view ids from silently landing on postfx's. */
#include "renderer/jce_view_bands.h"

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
/* Pack the cascade far-planes into the vec4 the shader reads.
 *
 * ALL FOUR lanes must carry a split even when fewer cascades exist, because
 * csm_shadow.sh reads them as an ordered ladder AND reads .w as the shadow
 * distance:
 *
 *     if      (d < u_csmSplits.x) cascade = 0;
 *     else if (d < u_csmSplits.y) cascade = 1;   // 0 => never taken
 *     else if (d < u_csmSplits.z) cascade = 2;   // 0 => never taken
 *     ...                                        // => cascade 3
 *     float shadow_far = u_csmSplits.w;          // 0 => fade disabled
 *
 * Left at zero -- which is what a plain `for (ci < cascade_count)` over a
 * zero-initialised array leaves -- a one- or two-cascade configuration sent
 * EVERY fragment past its last real split into cascade 3: an index whose
 * matrix was never uploaded (the u_csmVP upload is cascade_count long) and
 * whose map was never rendered. It disabled the distance fade in the same
 * stroke, so the boundary was a hard step rather than a ramp.
 *
 * That is not an edge configuration. csm_cascade_count is 1 on LOW, 2 on
 * MEDIUM, and jce_render_pipeline.c clamps it to 1 outright on integrated
 * GPUs. The step sits at splits[1], tens of metres out, and because the
 * shader compares VIEW-SPACE Z the locus is a plane whose normal is the
 * camera forward axis -- so it SWEEPS across the world when the camera merely
 * turns on the spot. That is the knife-cut band this was reported as.
 *
 * Padding with the last real split fixes both halves at once: .w becomes the
 * true shadow distance so the fade is alive again, and everything at or
 * beyond it is faded fully to lit, which mixes the never-rendered cascade-3
 * lookup entirely out of the result.
 *
 * Extracted from its caller so the rule can be executed rather than merely
 * written down; see tests/middleware/scene/test_jce_csm_splits.c. */
/* THE matrix cascade `c` is addressed with this frame.
 *
 * There is exactly one right answer and two textures that must both use it:
 * the static cascade map and, in dual mode, the dynamic atlas tile. The shader
 * derives BOTH taps from the single u_csmVP[cascade] uniform
 * (fs_pbr_body.sh: csm_clip -> csm_uv, then the same csm_uv indexes the atlas),
 * so a texture rendered with any other matrix is addressed in a coordinate
 * frame it was not drawn in.
 *
 * This existed inline in sr_bind_shadow_uniforms and NOT in the atlas pass,
 * which rendered every tile with csm->vp[c] every frame. That was correct
 * until the far-cascade round-robin was defaulted on: sr_csm_far_defer skips
 * the static render AND its cache store together, so shadow_cache_vp[c] keeps
 * describing the static texture -- but the atlas tile had just been redrawn
 * with the NEW matrix and was still being sampled with the old one. Movers'
 * shadows in cascades 2 and 3 therefore shifted by the accumulated snap delta
 * and snapped back on the frame that cascade came round again: a period-3
 * judder at whatever the frame rate is, on exactly the mid-and-far shadows
 * that were reported as shimmering.
 *
 * The defer gate's own comment states the invariant this restores -- "the
 * stale cascade is addressed with the transform it was rendered with" -- and
 * it was true of only one of the two textures. Sharing the selection is what
 * makes it structural rather than remembered.
 *
 * Must be called AFTER the static passes have run their cache stores, which is
 * where both callers already sit. */
/* How often the two matrices ACTUALLY differ, per cascade, and how often this
 * was asked. Dumped by JCE_DBG_SHPROF.
 *
 * Added because the fix above could not be measured: paired and unpaired
 * builds produced bit-identical captures in two scenes, including one with 28
 * dynamic casters. That is consistent with the fix being correct and the
 * PRECONDITION never occurring, and with the fix doing nothing at all, and a
 * picture cannot tell those apart. A counter can. */
static uint64_t g_sr_dbg_vp_mismatch[JCE_CSM_MAX_CASCADES];
static uint64_t g_sr_dbg_vp_asked[JCE_CSM_MAX_CASCADES];

jce_mat4 sr_cascade_sample_vp(const JceSceneRenderer *sr,
                              const JceCsmData *csm, uint32_t c)
{
    if (c >= JCE_CSM_MAX_CASCADES) return csm->vp[0];
    const bool cached = sr->shadow_cache_valid[c];
    g_sr_dbg_vp_asked[c]++;
    if (cached && memcmp(&sr->shadow_cache_vp[c], &csm->vp[c],
                         sizeof(jce_mat4)) != 0)
        g_sr_dbg_vp_mismatch[c]++;
    return cached ? sr->shadow_cache_vp[c] : csm->vp[c];
}

void sr_pack_csm_splits(const float *splits, uint32_t cascade_count,
                        float out[4])
{
    uint32_t n = 0;
    if (splits) {
        for (; n < cascade_count && n < 4u; n++) out[n] = splits[n + 1];
    }
    /* No cascades at all is a legitimate state -- the shader's own first line
     * is `if (u_csmSplits.x <= 0.0) return 1.0;`, i.e. zero means "no shadow
     * map", which is a different statement from "shadows end here". Padding
     * that case with anything positive would invent a shadow distance. */
    for (uint32_t i = n; i < 4u; i++) out[i] = (n > 0u) ? out[n - 1u] : 0.0f;
}

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
    if (!path) {
        /* Primitive / shared-JceMesh caster (cube/sphere/.. or a non-glTF .mesh):
         * use the shared mesh's local AABB so the per-cascade shadow cull, the
         * color/prepass frustum cull, AND the static wcache fast-path all apply to
         * it too — otherwise has_aabb stays false and every cascade keeps every
         * primitive (uncullable).  The result is cached in the wcache (computed
         * once per static entity), so the per-entity resolve cost is one-time. */
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            JceMesh *pm = mr ? sr_resolve_mesh(sr, mr) : NULL;
            if (pm) {
                float plmn[3], plmx[3];
                jce_mesh_get_aabb(pm, plmn, plmx);
                jce_mat4 pmodel = world ? *world : jce_scene_get_world_matrix(scene, e);
                sr_transform_aabb(&pmodel, jce_v3(plmn[0], plmn[1], plmn[2]),
                                  jce_v3(plmx[0], plmx[1], plmx[2]), out_mn, out_mx);
                return true;
            }
        }
        return false;
    }
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
/* Reject a caster whose light-space footprint misses this cascade's square.
 * Depth along the light is deliberately unbounded: something far behind the
 * cascade still casts into it.
 *
 * If this ever looks like it "barely culls anything", check the content before
 * changing it. At the 200k bench it drops 1.5-4%, which is correct: that bench
 * packs 200356 unit cubes into an 87x87x87 block (side^3 >= count, spacing 1.5,
 * see jce_state_benchmark_spawn), while cascade 0's radius is 60.5 -- the whole
 * scene fits inside the NEAR cascade, so every cascade legitimately contains
 * every caster and there is nothing to reject. Real content spread over a world
 * culls normally. */
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
float sr_effective_csm_blend(const JceSceneRenderer *sr)
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
    jce_enc_set_uniform(sr->u_csm_splits, disabled_splits, 1);
    jce_enc_set_uniform(sr->u_csm_params, params, 1);
    jce_enc_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);
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

    /* Fallback exists only to keep sampler 15 bound when no local shadow is
     * active (the shader's slot indices are all -1, so it never reads it).
     * It used to point at shadow_tex, which is now allocated lazily and is
     * absent in exactly the common case this fallback serves; csm_tex[0] is
     * always created with sr_create_shadow_targets and costs nothing extra. */
    bgfx_texture_handle_t tex = (sr->local_atlas_valid
                                 && BGFX_HANDLE_IS_VALID(sr->local_atlas_tex))
        ? sr->local_atlas_tex
        : (BGFX_HANDLE_IS_VALID(sr->csm_tex[0]) ? sr->csm_tex[0]
                                                : sr->shadow_tex);
    if (BGFX_HANDLE_IS_VALID(tex))
        jce_enc_set_texture(15, sr->u_local_shadow_map, tex, UINT32_MAX);

    /* Only upload the local-shadow VP matrices when a local shadow is actually
     * active: when frame_local_active is false every slot below is -1 and the
     * shader never reads u_local_shadow_vp — so skipping the JCE_MAX_LOCAL_SHADOWS-
     * mat4 upload is byte-identical AND removes that per-submit cost (a bench with
     * no point/spot shadows re-uploaded these dead matrices on every draw). */
    if (sr->frame_local_active)
        jce_enc_set_uniform(sr->u_local_shadow_vp, sr->frame_local_vp[0].raw[0],
                         JCE_MAX_LOCAL_SHADOWS);

    float slots[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    if (sr->frame_local_active) {
        for (int i = 0; i < JCE_MAX_SPOT_LIGHTS && i < 4; i++)
            slots[i] = sr->frame_spot_slot[i];
    }
    jce_enc_set_uniform(sr->u_spot_shadow_slot, slots, 1);

    float pslots[16];   /* 16 point lanes -> 4 vec4 */
    for (int i = 0; i < JCE_MAX_POINT_LIGHTS && i < 16; i++)
        pslots[i] = sr->frame_local_active ? sr->frame_point_slot[i] : -1.0f;
    jce_enc_set_uniform(sr->u_point_shadow_slot, pslots, 4);

    float inv_atlas = sr->shadow_map_size > 0
        ? 1.0f / (float)sr->shadow_map_size : 0.0f;
    /* tiles/side follows the active grid (2 default, 6 when point cube shadows
       are on) so the shader's tile-rect math matches the producer. */
    float tiles_side = (float)(sr->local_tiles ? sr->local_tiles
                                               : JCE_LOCAL_SHADOW_TILES);
    float params[4] = { tiles_side, inv_atlas,
                        sr->frame_local_bias, inv_atlas };
    jce_enc_set_uniform(sr->u_local_shadow_params, params, 1);

    /* Per-slot depth bias (lane i = atlas slot i). 4 floats = one vec4. Lets each
       shadow-casting light use its authored shadowBias instead of one global. */
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_bias))
        jce_enc_set_uniform(sr->u_local_shadow_bias, sr->frame_local_bias_slot, 1);

    /* #7 omnidirectional point cube shadows: per-point face-0 slot table + the
       6-face VPs. Off / no-cube => lanes all -1, so the shader takes the legacy
       single-tile u_pointShadowSlot path (byte-identical default). */
    if (BGFX_HANDLE_IS_VALID(sr->u_point_cube_base_slot)) {
        float cbslots[16];
        for (int i = 0; i < JCE_MAX_POINT_LIGHTS && i < 16; i++)
            cbslots[i] = (sr->frame_local_active && sr->point_cube_shadows)
                       ? sr->frame_point_cube_base_slot[i] : -1.0f;
        jce_enc_set_uniform(sr->u_point_cube_base_slot, cbslots, 4);
    }
    /* Same skip for the 6-face point-cube VPs (JCE_POINT_SHADOW_MAX*6 = the single
     * biggest matrix array in the whole bind): only uploaded when point cube
     * shadows are actually active — else every base-slot lane is -1 and the shader
     * never reads them, so skipping is byte-identical + drops the dead upload. */
    if (BGFX_HANDLE_IS_VALID(sr->u_point_cube_vp) &&
        sr->frame_local_active && sr->point_cube_shadows)
        jce_enc_set_uniform(sr->u_point_cube_vp, sr->frame_point_cube_vp[0].raw[0],
                         JCE_POINT_SHADOW_MAX * JCE_POINT_CUBE_FACES);
}

void sr_bind_frame_shadow_state(JceSceneRenderer *sr)
{
    if (!sr) return;
    /* Filter tier first — both the CSM and local-shadow shader paths
       branch on it (frame-constant, fully coherent per draw). */
    if (BGFX_HANDLE_IS_VALID(sr->u_shadow_quality)) {
        float q[4] = { sr->shadow_filter_tier, 0.0f, 0.0f, 0.0f };
        jce_enc_set_uniform(sr->u_shadow_quality, q, 1);
    }
    sr_bind_local_shadow_state(sr);

    /* r.shadows=0 -- force every lit surface fully unshadowed.
     *
     * Added because this ablation could not be obtained through settings.
     * `shadows.distance=0.01`, `shadows.cascades=0` and the render-pipeline
     * preset's `enable_csm=false` were each tried as "shadows off" and each
     * left CSM running: the probe kept logging cascade uploads and the
     * viewport moved by 0.27 grey levels -- in the DARKER direction, which
     * removing shadows cannot do.  Three plausible-looking controls, three
     * measurements of nothing.
     *
     * Routing through the existing disabled binder rather than a new path
     * makes the ablation exact: it zeroes u_csmSplits, and csm_shadow.sh
     * opens with `if (u_csmSplits.x <= 0.0) return 1.0;`, so the shader is
     * provably taking the fully-lit branch and not a nearly-off one. */
    {
        static JceCvar *s_cv_shadows;
        if (!s_cv_shadows)
            s_cv_shadows = jce_cvar_register_bool(
                "r.shadows", true, JCE_CVAR_FLAG_NONE,
                "Directional + cascade shadows. 0 forces every surface fully "
                "lit (exact ablation: zeroes u_csmSplits).");
        if (s_cv_shadows && !jce_cvar_get_bool(s_cv_shadows)) {
            sr_bind_shadow_uniforms_disabled(sr);
            return;
        }
    }

    if (!sr->frame_shadow_active) {
        sr_bind_shadow_uniforms_disabled(sr);
        return;
    }

    if (sr->shadow_use_csm && sr->last_csm_valid) {
        const JceCsmData *csm = &sr->last_csm;
        /* Sample each cascade with the matrix its TEXTURE was rendered with,
         * not with this frame's.
         *
         * Today these are the same thing: a cascade is only left un-rendered
         * when SR_CASCADE_CACHE_HIT fires, and that test IS a memcmp of the two
         * matrices -- so this is a no-op as it stands, verified by an unchanged
         * capture. It exists because it is the precondition for ever skipping a
         * cascade whose matrix HAS moved. Cascade round-robin (the far-defer
         * path) is unsound without it: the texture would hold last frame's
         * render while the shader addressed it with this frame's transform,
         * which samples the wrong texels rather than showing a stale shadow.
         * Shipping engines that stagger cascades keep this pairing for exactly
         * that reason. */
        jce_mat4 sample_vp[JCE_CSM_MAX_CASCADES];
        for (uint32_t ci = 0; ci < csm->cascade_count &&
                              ci < JCE_CSM_MAX_CASCADES; ci++)
            sample_vp[ci] = sr_cascade_sample_vp(sr, csm, ci);
        jce_enc_set_uniform(sr->u_csm_vp, sample_vp[0].raw[0],
                            (uint16_t)csm->cascade_count);
        float splits_v4[4];
        sr_pack_csm_splits(csm->splits, csm->cascade_count, splits_v4);
        jce_enc_set_uniform(sr->u_csm_splits, splits_v4, 1);

        /* JCE_DBG_CSM_LOG -- print the cascade partition the SHADER sees.
         *
         * Instrumented at the uniform upload, not at the variable that feeds
         * it, because the two are not the same thing: shadow_far is cached,
         * deadbanded and bucketed on the way here, and the whole question is
         * what survives that.  csm_shadow.sh fades shadows out over
         * [w*0.85, w], so a moving w moves a soft brightness band across the
         * ground -- which is the artifact this is meant to confirm or clear.
         *
         * Centre and radius are logged as well as the splits, because the
         * splits alone cannot answer the question. jce_csm.c snaps each
         * cascade centre onto a texel grid whose spacing is
         * (radius*2)/map_size, and quantises the radius into 0.5 m buckets
         * to keep that spacing bit-exact under rotation. If the radius ever
         * crosses a bucket the spacing changes and the WHOLE grid moves --
         * every shadow edge in that cascade jumps at once. The splits can be
         * perfectly steady while that happens, so logging them alone would
         * clear a suspect that was never being watched.
         *
         * One line per frame, env-gated, no cost when unset. */
        {
            /* Writes to the PATH in JCE_DBG_CSM_LOG, not to stdout: the
             * capture harness runs the editor with stdout=DEVNULL, so a
             * printf probe here produces a measurement that silently does not
             * exist.  A file also means the user can run the editor by hand
             * and hand back the log. */
            static SDL_IOStream *s_csm_log;
            static int           s_csm_log_tried;
            if (!s_csm_log_tried) {
                const char *e = getenv("JCE_DBG_CSM_LOG");
                s_csm_log_tried = 1;
                if (e && e[0] && e[0] != '0')
                    s_csm_log = SDL_IOFromFile(e, "wb");
            }
            if (s_csm_log) {
                static uint32_t s_frame;
                char line[384];
                int n = snprintf(line, sizeof line,
                        "CSMLOG frame=%u splits=%.4f,%.4f,%.4f,%.4f count=%u"
                        " r=%.4f,%.4f,%.4f,%.4f"
                        " c0=%.4f,%.4f,%.4f c1=%.4f,%.4f,%.4f\n",
                        s_frame++, (double)splits_v4[0], (double)splits_v4[1],
                        (double)splits_v4[2], (double)splits_v4[3],
                        csm->cascade_count,
                        (double)csm->radius[0], (double)csm->radius[1],
                        (double)csm->radius[2], (double)csm->radius[3],
                        (double)csm->center[0].x, (double)csm->center[0].y,
                        (double)csm->center[0].z,
                        (double)csm->center[1].x, (double)csm->center[1].y,
                        (double)csm->center[1].z);
                if (n > 0) SDL_WriteIO(s_csm_log, line, (size_t)n);
                SDL_FlushIO(s_csm_log);
            }
        }

        float csm_params[4] = { sr->shadow_map_size > 0 ? 1.0f / (float)sr->shadow_map_size : 0.0f,
            sr_effective_csm_blend(sr), sr->csm_normal_bias, sr->csm_filter_radius };
        jce_enc_set_uniform(sr->u_csm_params, csm_params, 1);

        float bias_scales[4];
        sr_fill_csm_bias_scales(csm, bias_scales);
        jce_enc_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);

        for (uint32_t ci = 0;
             ci < sr->csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
            jce_enc_set_texture((uint8_t)(9 + ci), sr->u_csm_samplers[ci], sr->csm_tex[ci], UINT32_MAX);

        /* Dual shadow maps: bind the dynamic-caster atlas to stage 5 (s_shadowMap
         * — dead under CSM, the single sampler-slot unlock) + upload
         * u_csmDynParams {tiles=2, 1/atlas_size, enabled, 0}.  The PBR shader
         * min()s the atlas tile into sample_csm_shadow only when enabled>0.5, so
         * OFF (or a frame with no movers) stays byte-identical to the single-map
         * path (no atlas bound, enabled=0). */
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_dyn_params)) {
            const bool dual = sr->frame_dyn_csm_active &&
                              BGFX_HANDLE_IS_VALID(sr->dyn_csm_atlas_tex) &&
                              BGFX_HANDLE_IS_VALID(sr->u_shadowMap);
            float dp[4] = {
                2.0f,
                sr->dyn_csm_atlas_size > 0 ? 1.0f / (float)sr->dyn_csm_atlas_size : 0.0f,
                dual ? 1.0f : 0.0f,
                0.0f
            };
            jce_enc_set_uniform(sr->u_csm_dyn_params, dp, 1);
            if (dual)
                jce_enc_set_texture(5, sr->u_shadowMap, sr->dyn_csm_atlas_tex, UINT32_MAX);
        }
        return;
    }

    if (!sr->shadow_use_csm && sr->frame_shadow_vp_valid && BGFX_HANDLE_IS_VALID(sr->shadow_tex)) {
        jce_enc_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
        jce_enc_set_uniform(sr->u_shadowVP, sr->frame_shadow_vp, 1);
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

static bool sr_shadow_dual_on(void);   /* fwd: dual shadow-map env toggle */

void sr_apply_view_order(uint16_t view_id_base,
                                const JceSceneRenderConfig *cfg,
                                uint32_t csm_cascade_count,
                                bool gpu_cull_view,
                                bool include_underwater_view)
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
            /* Dual shadow-map dynamic-atlas band (base+21..25): reserve it before
               the color view whenever dual mode is enabled so the atlas renders
               before it is sampled.  Harmless when unused (empty views). */
            sr_shadow_dual_on(),
            /* Underwater absorption (base+17): reserved once this renderer has
             * seen water with absorption authored.  Sticky rather than
             * per-frame, because the alternative -- gate on being submerged --
             * is decided during the water draw, which is after this. */
            include_underwater_view,
            &order)) {
        /* A failed build means the remap window did not fit the order
         * buffer.  Returning leaves bgfx on default id order, which sorts
         * the shadow atlas AFTER the colour pass that samples it -- a
         * silently wrong frame.  Say so once. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            LOG_WARN(LOG_TAG,
                     "view order: window did not fit %u slots at base %u; "
                     "falling back to bgfx default id order",
                     (unsigned)JCE_SCENE_RENDERER_VIEW_ORDER_MAX,
                     (unsigned)view_id_base);
        }
        return;
    }

    bgfx_set_view_order(order.first, order.count, order.order);

    /* Declare what the scene renderer now owns, so a future band that lands on
     * postfx (or on the editor's absolute ids) is named at the moment of the
     * clash instead of showing up as a black viewport. The order array holds
     * every id this render will bind; collapse it into contiguous runs so the
     * report reads as bands rather than 30 single views. */
    if (jce_view_bands_enabled() && order.named_count > 0u) {
        uint16_t sorted[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
        /* Only the NAMED views -- the filler that closes the remap window
         * belongs to postfx and the editor overlays, and claiming it made this
         * guard fire on every single frame. */
        uint16_t n = order.named_count, i, j;
        memcpy(sorted, order.order, (size_t)n * sizeof(sorted[0]));
        for (i = 1u; i < n; ++i) {          /* insertion sort: n <= 64 */
            const uint16_t k = sorted[i];
            for (j = i; j > 0u && sorted[j - 1u] > k; --j)
                sorted[j] = sorted[j - 1u];
            sorted[j] = k;
        }
        i = 0u;
        while (i < n) {
            uint16_t run_first = sorted[i];
            uint16_t run_end   = run_first;
            while (i + 1u < n && sorted[i + 1u] == (uint16_t)(run_end + 1u)) {
                ++i; run_end = sorted[i];
            }
            jce_view_bands_claim("scene-renderer", run_first,
                                 (uint16_t)(run_end - run_first + 1u));
            ++i;
        }
    }
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
    /* Dual shadow-map dynamic atlas (idx==0 is a legal bgfx handle → guard +
     * reset to UINT16_MAX so a stale handle can't silently destroy another
     * resource; see the D3D12 leak family). */
    if (BGFX_HANDLE_IS_VALID(sr->dyn_csm_atlas_fbo)) {
        bgfx_destroy_frame_buffer(sr->dyn_csm_atlas_fbo);
        sr->dyn_csm_atlas_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->dyn_csm_atlas_tex)) {
        bgfx_destroy_texture(sr->dyn_csm_atlas_tex);
        sr->dyn_csm_atlas_tex.idx = UINT16_MAX;
    }
    sr->dyn_csm_valid = false;
    sr->shadow_valid = false;
    sr->csm_valid = false;
    sr->last_csm_valid = false;
}

static bool sr_make_depth_target(JceSceneRenderer *sr,
                                 bgfx_texture_handle_t *tex,
                                 bgfx_frame_buffer_handle_t *fbo);

/* How many cascade targets this configuration can actually render into.
 * Reads the live pipeline rather than a cached value so a tier change (which
 * re-clamps csm_cascade_count) is reflected the next time targets are built.
 * Never returns 0: the local-shadow sampler fallback binds csm_tex[0]. */
static uint32_t sr_csm_wanted_count(const JceSceneRenderer *sr)
{
    (void)sr;
    JceRenderPipelineDesc d;
    jce_render_pipeline_get(&d);
    uint32_t n = (uint32_t)d.csm_cascade_count;
    if (n < 1u) n = 1u;
    if (n > JCE_CSM_MAX_CASCADES) n = JCE_CSM_MAX_CASCADES;
    return n;
}

/* Allocate any cascade target that is now needed but was skipped when the
 * targets were built with a smaller count (a tier or pipeline change can raise
 * it).  Cheap no-op once the set is complete. */
bool sr_ensure_csm_count(JceSceneRenderer *sr)
{
    if (!sr || sr->shadow_map_size == 0) return false;
    const uint32_t want = sr_csm_wanted_count(sr);
    bool ok = true;
    for (uint32_t i = 0; i < want; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->csm_fbo[i])) continue;
        if (!sr_make_depth_target(sr, &sr->csm_tex[i], &sr->csm_fbo[i]))
            ok = false;
        else
            sr->shadow_cache_valid[i] = false;  /* fresh depth: re-render */
    }
    return ok;
}

void sr_create_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr || sr->shadow_map_size == 0) return;

    const uint16_t sz = sr->shadow_map_size;
    const bgfx_texture_format_t depth_fmt = sr->shadow_depth_fmt;
    bgfx_attachment_t at;

    /* shadow_tex (the single non-CSM map) and local_atlas_tex are created on
     * first use, not here -- see sr_ensure_shadow_map / sr_ensure_local_atlas.
     * A scene with one directional light and no local shadow casters -- the
     * common case -- was paying for both regardless: seven sz^2 depth targets
     * were allocated where five are reachable, 33 MB of the 117 MB shadow
     * budget at sz=2048 for surfaces nothing ever rendered into. */

    /* Re-arm the pass gate: sr_destroy_shadow_targets clears it, and it used
     * to be restored as a side effect of creating the non-CSM map here.  With
     * that allocation moved out, a shadow-map RESIZE would have left the flag
     * false and silently killed every shadow from then on. */
    sr->shadow_valid = true;
    sr->csm_valid = true;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        /* Only the cascades the pipeline actually renders are allocated.  All
         * four were created unconditionally, so a LOW-tier run -- where the
         * floor clamps the count to 1 -- still paid for three shadow_map_size^2
         * depth surfaces that nothing ever wrote to, and a 3-cascade HIGH-tier
         * scene paid for a fourth.  Strictly waste: this changes no cascade
         * that is used, so shadow quality is untouched.  sr_ensure_csm_count
         * grows the set if the count later rises. */
        if (i >= sr_csm_wanted_count(sr)) {
            sr->csm_tex[i].idx = UINT16_MAX;
            sr->csm_fbo[i].idx = UINT16_MAX;
            continue;
        }
        sr->csm_tex[i] = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL, 0);
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, sr->csm_tex[i], BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        sr->csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        if (!BGFX_HANDLE_IS_VALID(sr->csm_fbo[i]))
            sr->csm_valid = false;
        /* Fresh FBO holds no valid depth → invalidate the shadow-map cache so
         * the next frame re-renders this cascade instead of trusting stale
         * contents of a just-recreated texture. */
        sr->shadow_cache_valid[i] = false;
    }

    /* Dual shadow-map DYNAMIC atlas (JCE_SHADOW_DUAL): one depth texture the
     * same size as a full cascade map, holding the 4 cascades as a 2x2 grid of
     * (sz/2)^2 tiles.  Same depth fmt / point-clamp as csm_tex.  Always allocated
     * (cheap: one map) so toggling the env needs no target rebuild; only written
     * + sampled when dual mode is active. */
    sr->dyn_csm_atlas_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL, 0);
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, sr->dyn_csm_atlas_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    sr->dyn_csm_atlas_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    sr->dyn_csm_atlas_size = sz;
    sr->dyn_csm_valid = BGFX_HANDLE_IS_VALID(sr->dyn_csm_atlas_fbo);

}

/* Create one sz^2 depth target + its framebuffer.  Returns false when the
 * allocation failed, in which case the caller must skip the pass rather than
 * bind an invalid handle. */
static bool sr_make_depth_target(JceSceneRenderer *sr,
                                 bgfx_texture_handle_t *tex,
                                 bgfx_frame_buffer_handle_t *fbo)
{
    if (BGFX_HANDLE_IS_VALID(*fbo)) return true;
    if (!sr || sr->shadow_map_size == 0) return false;
    const uint16_t sz = sr->shadow_map_size;
    *tex = bgfx_create_texture_2d(sz, sz, false, 1, sr->shadow_depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(*tex)) return false;
    bgfx_attachment_t at;
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, *tex, BGFX_ACCESS_WRITE, 0, 1, 0,
                         BGFX_RESOLVE_NONE);
    *fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    return BGFX_HANDLE_IS_VALID(*fbo);
}

/* The single non-CSM shadow map.  Only reachable when a frame decides against
 * cascades, which most directional-light scenes never do. */
bool sr_ensure_shadow_map(JceSceneRenderer *sr)
{
    if (!sr) return false;
    /* Deliberately does NOT touch sr->shadow_valid: that flag gates the whole
     * shadow pass (sr_render_shadows returns early on it), not the presence of
     * this one target.  Conflating the two switched every shadow in the engine
     * off the moment this map became lazily allocated. */
    return sr_make_depth_target(sr, &sr->shadow_tex, &sr->shadow_fbo);
}

/* The spot/point shadow atlas.  Only reachable once a frame actually has a
 * local shadow caster to render. */
bool sr_ensure_local_atlas(JceSceneRenderer *sr)
{
    if (!sr) return false;
    sr->local_atlas_valid = sr_make_depth_target(sr, &sr->local_atlas_tex,
                                                 &sr->local_atlas_fbo);
    return sr->local_atlas_valid;
}

void sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size)
{
    if (!sr || size == 0) return;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    /* Deliberately does NOT test shadow_fbo: that target is allocated on first
     * use and a CSM scene never allocates it at all, so requiring it here made
     * this guard permanently false.  Every frame then destroyed and rebuilt
     * every shadow target, which cleared shadow_cache_valid[] and drove the
     * CSM cascade skip-cache to a 0% hit rate -- measured at 4796 evaluations
     * and 0 hits over 1200 frames on a static camera with no dynamic casters,
     * costing ~56 ms of a 66 ms frame at 150k entities.  csm_fbo[0] is the
     * target this function actually sizes; it is the correct witness. */
    if (sr->shadow_map_size == size &&
        BGFX_HANDLE_IS_VALID(sr->csm_fbo[0]))
        return;

    sr_destroy_shadow_targets(sr);
    sr->shadow_map_size = size;
    sr_create_shadow_targets(sr);
    sr->shadow_far_valid = false;
    LOG_INFO(LOG_TAG, "shadow map resized to %u", (unsigned)size);
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
/* JCE_DISABLE_SHADOW_PERCASCADE=1 restores the all-or-nothing dynamic gate
 * (grid + whole cache voided while any dynamic caster exists). */
static int sr_shadow_percascade_on(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *v = getenv("JCE_DISABLE_SHADOW_PERCASCADE");
        s_on = (v && v[0] && v[0] != '0') ? 0 : 1;
    }
    return s_on;
}

/* Active this frame: env-enabled AND the dynamic set is small enough that
 * per-cascade overlap tests + un-gridded gather appends stay cheap.  A
 * mostly-dynamic world (e.g. a full physics stress) falls back to the
 * all-or-nothing gate — with everything moving there is nothing to save.
 * Flips are safe: crossing the threshold means the caster SET changed, so
 * shadow_caster_key changed and the grid/cascade caches re-key anyway. */
/* Cascade round-robin interval: cascades 2+ re-render every Nth shadow pass
 * instead of every one. 0/1 disable it.
 *
 * Default 3, changed from 0 after the cost was measured on both axes rather
 * than assumed. It had been off since it was written, partly because it could
 * not work (the sampling matrix was not paired with the texture) and partly
 * because there was no instrument able to say what it cost.
 *
 *   200k spread over 4000 units, camera panning 0.3 deg/frame:
 *     off      frame 10.66 ms, sh_gather 6.37, 4/4 cascades rendered
 *     N = 3    frame  6.90 ms, sh_gather 3.21, 3/4                 -35%
 *
 *   Image, against a same-build noise floor of 0.00185% of pixels over
 *   threshold: 0.04283% at N=3, 23x the floor -- real, and confined to the
 *   horizon strip the far cascades cover. Reviewed side by side at 1:1 with a
 *   blink comparator; the artifact was not visible to the reviewer.
 *
 * The pixel count alone would not have justified this (0.17% of subpixels once
 * turned out to be a grid pattern anyone could see at a glance). The default
 * moved because someone looked.
 *
 * A stale block above this one still said "default 0=off ... never enabled by
 * default" and has been deleted: it was written when that was true and left
 * behind when the default moved, so the file stated both defaults at once and
 * a reader could take either home.
 *
 * The reasoning above -- "it could not work (the sampling matrix was not
 * paired with the texture)" -- was right about the STATIC cascade map and was
 * not applied to the dynamic atlas, which kept rendering every tile with this
 * frame's VP while the shader addressed it with the cached one. That half is
 * closed now; see sr_cascade_sample_vp. */
static int sr_csm_far_interval(void)
{
    static int s_iv = -1;
    if (s_iv < 0) {
        /* DEFAULT 1 = off.  It was 3, and 3 is visible.
         *
         * A deferred cascade keeps the VP it was rendered with (see
         * sr_cascade_sample_vp), so its texels are addressed correctly -- but
         * they are the shadows of a camera 1 or 2 frames old.  The near
         * cascades are never deferred, so while the view moves the mid-ground
         * shadows LAG the rest of the frame and then catch up in a single
         * step, once every `iv` frames.  That is not a soft cost; it is a
         * periodic jump in a whole band of the image.
         *
         * Measured at a user-supplied camera bookmark (forest canopy from
         * 100 m, 0.5 deg/frame), per-pair frame-to-frame residual in the band
         * that cascade 2 covers, grouped by (pair index mod 3):
         *
         *   interval 3 : 12.4679  12.6062  11.3132   -> spread 10.7%
         *   interval 1 : 11.0830  11.1678  11.0438   -> spread  1.1%
         *
         * and the mean residual itself fell from 12.4 to 11.1.  A tenfold
         * difference in the periodic component, at exactly the period the
         * round-robin runs at, is the mechanism's own fingerprint.
         *
         * What it bought: 3.17 of 4 cascades rendered per frame instead of
         * 4.00, i.e. about 21% of the cascade renders.  That saving is
         * purchased with the lag, not alongside it -- the deferral only ever
         * fires when the cascade genuinely needed re-fitting, because a still
         * camera hits SR_CASCADE_CACHE_HIT and returns before reaching here.
         *
         * Kept as an env var rather than deleted: the trade is real, and a
         * project that is GPU-bound with a slow camera may want it.  It is no
         * longer the default because the artifact was reported by a user
         * before it was measured by anyone. */
        const char *v = getenv("JCE_CSM_FAR_INTERVAL");
        s_iv = (v && v[0]) ? atoi(v) : 1;
        if (s_iv < 0) s_iv = 0;
    }
    return s_iv;
}

static bool sr_cascade_has_dyn(JceSceneRenderer *sr,
                               jce_vec3 right_ws, jce_vec3 up_ws,
                               jce_vec3 cc, float radius);

/* Why sr_csm_far_defer declined, by guard: 0=interval/near 1=nothing cached
 * 2=mover erase frame 3=cascade has a dynamic caster.  JCE_DBG_SHPROF dumps it;
 * the switch measured as a no-op and this says which test is always true. */
static uint64_t g_sr_dbg_defer_why[4] = { 0, 0, 0, 0 };

/* `dual_on` matters here for the same reason SR_CASCADE_CACHE_HIT already
 * exempts it: in dual mode a mover is rendered into the SEPARATE dynamic atlas
 * and is never appended to the static cascade, so it cannot dirty this map.
 * The cache-hit path drew that conclusion; this path did not, and so declined
 * on every scene with a moving caster reaching a far cascade -- 28 of them in
 * street_demo, which is why the switch measured as a no-op.
 *
 * Deferring is only sound because the sampling matrix is now paired with the
 * texture (see sr_bind_shadow_uniforms): the stale cascade is addressed with
 * the transform it was rendered with, so it shows a slightly older placement
 * rather than the wrong texels. */
static bool sr_csm_far_defer(JceSceneRenderer *sr, uint32_t c, bool dual_on,
                             jce_vec3 right_ws, jce_vec3 up_ws,
                             jce_vec3 cc, float radius)
{
    const int iv = sr_csm_far_interval();
    if (iv <= 1 || c < 2u) { g_sr_dbg_defer_why[0]++; return false; }
    if (!sr->shadow_cache_valid[c]) { g_sr_dbg_defer_why[1]++; return false; }
    if (!dual_on) {
        if (sr->shadow_cache_dyn_c[c]) { g_sr_dbg_defer_why[2]++; return false; }
        if (sr_cascade_has_dyn(sr, right_ws, up_ws, cc, radius))
            { g_sr_dbg_defer_why[3]++; return false; }
    }
    static uint32_t s_pass;                          /* counts shadow passes */
    if (c == 2u) s_pass++;                           /* bump once per pass */
    return (s_pass % (uint32_t)iv) != (c % (uint32_t)iv);
}

/* Cascade cache-hit tallies for JCE_DBG_CSM_STATS (dumped from the CSM block,
 * incremented at the SR_CASCADE_CACHE_HIT sites).  File scope so both reach
 * them; only touched under the debug env, otherwise dead. */
static uint64_t g_sr_dbg_cascade_eval = 0;
static uint64_t g_sr_dbg_cascade_hit  = 0;

#define SR_SHADOW_DYN_MAX 512u
static bool sr_shadow_percascade_active(const JceSceneRenderer *sr)
{
    return sr_shadow_percascade_on()
        && sr->shadow_dyn_count <= SR_SHADOW_DYN_MAX;
}

/* Dual shadow maps (JCE_SHADOW_DUAL, default ON -- see the block below, which
 * says so and is the authority): render dynamic casters into a
 * SEPARATE atlas and min() it in the shader, so the static csm_tex[c] cache is
 * no longer voided by a mover overlapping the cascade.  Requires percascade
 * active (dynamics tracked in shadow_dyn) and a valid atlas. */
static bool sr_shadow_dual_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("JCE_SHADOW_DUAL");
        if (e && e[0]) {
            v = (e[0] != '0') ? 1 : 0;          /* explicit override wins */
        } else {
            /* Default ON everywhere.
             *
             * It was ON for integrated only (~9.5% there on 15k static + 400
             * movers), and OFF on discrete "until its higher-tier 3x3
             * min-combine cost is measured net-positive there". That could not
             * be measured: the dynamic atlas owned view ids that belonged to
             * the postfx chain, so turning it on rendered a black viewport —
             * see JCE_VIEW_DYN_CSM_OFFSET. With the band moved, measured on
             * discrete (win-x64, D3D12, 900 frames, JCE_FRAME_DT_FIXED):
             *
             *   caged_kingdom/graveyard, 54 movers:
             *     5.90 -> 4.76 ms avg (-19%), 1360 -> 760 draws (-44%),
             *     p50 5.70 -> 4.73, p95 6.63 -> 5.79, p99 11.94 -> 10.58,
             *     CSM cascade cache hit 0/3356 -> 3345/3356.
             *   street_demo/street, 28 movers:
             *     3.60 -> 3.15 ms avg (-13%), 396 -> 219 draws (-45%),
             *     cache hit 0/3356 -> 3329/3356.
             *
             * Both frames are pixel-equivalent to the single-map path. The
             * 3x3 min-combine the comment worried about costs +0.07 ms in
             * Scene/Color, against 2.5 ms of cascade re-render removed.
             * Mover-free frames still take the enabled=0 lane and stay
             * byte-identical. */
            (void)jce_renderer_get_recommendation;

            /* Default ON.  The black/flickering viewport this used to cause
             * was never the band's address -- it was the remap WINDOW.
             * bgfx_set_view_order rewrites [first, first+count), and at offset
             * 120 the scene viewport's window swallowed the game viewport
             * whole.  The band now sits at base+52 and every window is a true
             * permutation of itself (see JCE_VIEW_DYN_CSM_OFFSET and the
             * builder's tail), so the viewports no longer overlap.
             *
             * Worth having on: it is what makes the CSM cascade cache hit.
             * street_demo + 200k entities, shadow_gather per frame:
             *   off 130.79 ms      on 0.03 ms
             * On authored content the gap is small (0.05-0.55 ms), but the
             * cost of being wrong at scale is the whole frame.
             * JCE_SHADOW_DUAL=0 still opts out. */
            v = 1;
        }
    }
    return v != 0;
}

/* True when any listed dynamic caster overlaps cascade c's coverage disc —
 * the same conservative test the cascade body uses to keep casters, so the
 * gate is a superset of what the render would draw.  A dynamic caster with
 * no resolvable AABB can't be tested: conservatively overlaps everything. */
static bool sr_cascade_has_dyn(JceSceneRenderer *sr,
                               jce_vec3 right_ws, jce_vec3 up_ws,
                               jce_vec3 cc, float radius)
{
    for (uint32_t di = 0; di < sr->shadow_dyn_count; di++) {
        const SrEntityCull *ec = &sr->ecull[sr->shadow_dyn[di]];
        if (!ec->casts_shadow || !ec->is_dyn_caster) continue;
        if (!ec->has_aabb) return true;
        if (!sr_caster_culled_for_cascade(&ec->wmin, &ec->wmax,
                                          right_ws, up_ws, cc, radius))
            return true;
    }
    return false;
}

static bool sr_build_shadow_space(JceSceneRenderer *sr, EntityList *list,
                                  JceScene *scene)
{
    /* Incremental gate: on a static frame last frame's grid + noaabb[] are
     * still exact — skip the O(casters x cells_per_obj) reset + re-insert
     * (~2.3ms/frame at 150k static casters, measured via the sh_space
     * phase).  Same triple invalidation as the CSM cascade cache:
     * caster_key covers the caster SET + each caster's xform_gen, the
     * dynamic-caster gate covers in-place movers the key can't see, and
     * structural_epoch + list-count cover entity add/remove/reparent (grid
     * values are list indices; flecs table order only changes on structural
     * edits, which bump the epoch). */
    static int s_spacecache_disabled = -1;   /* JCE_DISABLE_SHADOWSPACECACHE A/B */
    if (s_spacecache_disabled < 0) {
        const char *v = getenv("JCE_DISABLE_SHADOWSPACECACHE");
        s_spacecache_disabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    const uint64_t sp_epoch = jce_scene_get_structural_epoch(scene);
    if (!s_spacecache_disabled
        && sr->shadow_space_valid
        /* Per-cascade mode: dynamics are EXCLUDED from the grid (delivered
         * via sr->shadow_dyn instead), so a mover no longer voids it — the
         * ~2.3ms/frame full grid rebuild while anything moves is gone. */
        && (sr_shadow_percascade_active(sr) || !sr->shadow_has_dynamic_caster)
        && sr->shadow_space_caster_key   == sr->shadow_caster_key
        && sr->shadow_space_struct_epoch == sp_epoch
        && sr->shadow_space_list_count   == list->count)
        return true;

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
    const bool pc_on = sr_shadow_percascade_active(sr);
    for (int i = 0; i < list->count; i++) {
        if (!sr->ecull[i].casts_shadow) continue;
        /* Per-cascade mode: dynamics are not gridded (and not in noaabb[]) —
         * the cascade gather appends sr->shadow_dyn instead, so the static
         * grid + noaabb list stay exact across mover frames. */
        if (pc_on && sr->ecull[i].is_dyn_caster) continue;
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
        if (pc_on && sr->ecull[i].is_dyn_caster) continue;
        JceAABB b = { sr->ecull[i].wmin, sr->ecull[i].wmax };
        jce_space_insert(sr->shadow_space, b, (uint32_t)i);
    }
    sr->shadow_space_valid       = true;
    sr->shadow_space_caster_key  = sr->shadow_caster_key;
    sr->shadow_space_struct_epoch = sp_epoch;
    sr->shadow_space_list_count  = list->count;
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

/* Dual shadow maps: render ONLY the dynamic (mover) casters into the dynamic CSM
 * atlas — a 2x2 tile grid of the 4 cascades in one depth texture — using the
 * SAME cascade VPs as the static maps.  One full-atlas depth clear, then one
 * BGFX_CLEAR_NONE tile view per cascade (per-tile clears wipe neighbours on
 * D3D).  Iterates only sr->shadow_dyn[], fine-culled per cascade.  The instanced
 * / crowd shadow batchers are flushed per tile view so a mover lands in its own
 * cascade tile.  Runs every frame (movers change) while the static cascades stay
 * cached.  The PBR shader min()s this atlas into sample_csm_shadow. */
static void sr_draw_dyn_csm_pass(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list, const JceCsmData *csm,
                                 uint16_t view_id_base,
                                 jce_vec3 scull_right, jce_vec3 scull_up)
{
    sr->frame_dyn_csm_active = false;
    if (!sr->dyn_csm_valid || sr->shadow_dyn_count == 0) return;

    const uint16_t N    = sr->dyn_csm_atlas_size;
    const uint16_t half = (uint16_t)(N / 2u);
    /* Dynamic-atlas view band: one clear at base+JCE_VIEW_DYN_CSM_OFFSET plus
     * four tiles above it, reserved before the color view in
     * jce_scene_renderer_view_order_build.  Must stay out of the postfx range,
     * which runs base+20..base+40.
     *
     * The offset is 52. Two comments here and in jce_scene_renderer_view_order.c
     * said "base+120..124" -- the value this band was moved to when it was
     * overlapping postfx and the whole screen went black, and not the value it
     * has now. Stale coordinates in the one place that has already produced a
     * black screen are worth correcting.
     *
     * See the note at the local-shadow band for why this does not claim a
     * view range of its own. */
    const uint16_t clear_v = (uint16_t)(view_id_base + JCE_VIEW_DYN_CSM_OFFSET);
    float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    /* One full-target depth clear (depth=1 => "no dynamic occluder"). */
    bgfx_set_view_rect(clear_v, 0, 0, N, N);
    bgfx_set_view_frame_buffer(clear_v, sr->dyn_csm_atlas_fbo);
    bgfx_set_view_clear(clear_v, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx_touch(clear_v);

    sr_crowd_shadow_reset(sr);

    for (uint32_t c = 0; c < csm->cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
        const uint16_t cv = (uint16_t)(view_id_base + JCE_VIEW_DYN_CSM_OFFSET + 1u + c);
        const uint16_t tx = (uint16_t)((c % 2u) * half);
        const uint16_t ty = (uint16_t)((c / 2u) * half);
        bgfx_set_view_rect(cv, tx, ty, half, half);
        bgfx_set_view_frame_buffer(cv, sr->dyn_csm_atlas_fbo);
        bgfx_set_view_clear(cv, BGFX_CLEAR_NONE, 0, 1.0f, 0);   /* never per-tile clear */
        /* The matrix the SHADER will address this tile with, not this frame's.
         * They differ exactly when the far-cascade round-robin deferred this
         * cascade's static render -- see sr_cascade_sample_vp. */
        const jce_mat4 tile_vp = sr_cascade_sample_vp(sr, csm, c);
        bgfx_set_view_transform(cv, identity, tile_vp.raw[0]);
        bgfx_touch(cv);

        for (uint32_t di = 0; di < sr->shadow_dyn_count; di++) {
            const uint32_t i = sr->shadow_dyn[di];
            const SrEntityCull *ec = &sr->ecull[i];
            if (!ec->casts_shadow || !ec->is_dyn_caster) continue;
            if (ec->has_aabb &&
                sr_caster_culled_for_cascade(&ec->wmin, &ec->wmax,
                                             scull_right, scull_up,
                                             csm->center[c], csm->radius[c]))
                continue;
            JceEntity e = list->entities[i];
            if (sr_try_submit_bindpose_shadow(sr, scene, e, (int)i, cv)) continue;
            if (sr_try_submit_skinned_shadow(sr, scene, e, (int)i, cv)) continue;
            if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, (int)i, cv)) continue;
            jce_mat4 model; JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, (int)i, &model, &mesh, NULL)) continue;
            if (!mesh) continue;
            jce_enc_set_transform(model.raw[0], 1);
            jce_mesh_submit_shadow(mesh, sr->renderer, cv);
        }
        /* Drain the deferred instanced/crowd shadow batches into THIS tile view
         * (skinned/instanced movers accumulate, must flush per view). */
        sr_sh_flush(sr, cv);
        sr_crowd_shadow_flush(sr, cv);
    }
    sr->frame_dyn_csm_active = true;
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

    /* Start this frame's bind-pose shadow batch empty; it accumulates per
     * cascade / tile and is drained by sr_bindpose_shadow_flush at color-pass
     * start (the last view's batch), the previous views flushing on change. */
    sr_bindpose_shadow_reset(sr);

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
    /* Cascade targets are allocated only up to the count in use; a tier or
     * pipeline change can raise it after they were built, so grow the set
     * before rendering into it.  No-op once complete. */
    if (use_csm) sr_ensure_csm_count(sr);
    jce_vec3 shadow_dir;
    if (!sr_resolve_primary_dir_light(sr, scene, list, true, &shadow_dir, NULL, NULL))
        return;

    /* Capability-matrix U3: at night the moon is the key light.
     *
     * Below the horizon the resolver still returns the sun, so the cascades
     * were fitted around a light underneath the world. This points them at the
     * moon instead.
     *
     * ITS VISIBLE EFFECT IS CURRENTLY NIL, AND THAT IS NOT A REASON TO REMOVE
     * IT -- it is the finding. Measured on a scene frozen at midnight, pixels
     * moved by switching shadows off: 0.004% before this change, 0.000% after,
     * against 4.7% by day. The cause is not the cascades: there is 506x less
     * directional energy at night to occlude (mean |delta| 0.0033 vs 1.6698),
     * because jce_time_of_day.c gives night a "moonlight TINT" -- a colour
     * floor on the sun term -- and never a moon with radiance of its own.
     *
     * So the matrix's estimate for U3, "one call-site change in the CSM
     * setup", is wrong: a shadow needs a light, and the moon is not one yet.
     * This half is correct and guarded and costs nothing; the other half is
     * plan section 8, and until it lands the right expectation for this line
     * is that it changes no pixel.
     *
     * jce_environment_key_direction is the one predicate everything that picks
     * a key light must share -- that is its stated reason for existing -- and
     * shadow_dir is already in its convention, a unit vector TOWARD the light.
     *
     * GATED ON tod_active, and that guard is the whole safety of this change.
     * Without a day/night cycle, sun_direction_ws is whatever the scene's
     * directional light happens to be, and a light authored pointing upward
     * (to_light.y < 0, which is perfectly legal and appears in stylised
     * scenes) would read as night and flip every shadow to the antipode. A
     * scene that opted into a cycle has asked for a sky where the sun sets;
     * one that did not has not. */
    if (sr->tod_active && !jce_environment_is_daytime(sr->env))
        shadow_dir = jce_environment_key_direction(sr->env);

    /* Compute shadow view IDs from base. */
    const uint16_t shadow_view_0 = (uint16_t)(view_id_base + 10);

    if (!use_csm) {
        /* Lazily allocated: this map exists only for frames that decline
         * cascades.  Bail rather than bind an invalid framebuffer. */
        if (!sr_ensure_shadow_map(sr)) return;

        float shadow_vp[16];
        sr_compute_shadow_vp(sr, &shadow_dir, shadow_vp);

        bgfx_set_view_rect(shadow_view_0, 0, 0,
                           sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(shadow_view_0, sr->shadow_fbo);
        bgfx_set_view_clear(shadow_view_0, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bgfx_set_view_transform(shadow_view_0, identity, shadow_vp);
        jce_enc_set_uniform(sr->u_shadowVP, shadow_vp, 1);
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
                if (sr_try_submit_bindpose_shadow(sr, scene, e, i, shadow_view_0))
                    continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, i, shadow_view_0))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, shadow_view_0))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0, NULL))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh, NULL)) continue;
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
            if (sr_try_submit_bindpose_shadow(sr, scene, e, i, shadow_view_0))
                continue;
            if (sr_try_submit_skinned_shadow(sr, scene, e, i, shadow_view_0))
                continue;
            if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, shadow_view_0))
                continue;
            if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0, NULL))
                continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh, NULL)) continue;
            if (!mesh) continue;
            jce_enc_set_transform(model.raw[0], 1);
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

    /* r.shadow.near -- override the cascade partition's near end.
     *
     * The constant above is deliberate and its reasoning is sound, but it has
     * a consequence the comment does not mention: the partition is fitted to
     * [near, far] while the RECEIVERS occupy only part of that span, and a
     * camera that stands off its subject leaves the front of the range empty.
     * Measured on hidden_cove at the `slope` framing -- camera 95 m up, all
     * geometry between roughly 100 and 400 m -- the splits came out
     * 37/78/153/520, so cascades 0, 1 and 2 covered air and every receiver in
     * the frame was shadowed by cascade 3 alone, at its coarsest.  That is
     * what made distance=2000 look like a 75x improvement: it did not add
     * reach (shadow_far 520 already exceeded the farthest corner at 402), it
     * moved the split boundaries out over the geometry so three cascades did
     * the work instead of one.
     *
     * Exposed as a knob rather than changed outright because the fixed near is
     * load-bearing for zoom stability, and trading that away needs the
     * comparison to be measurable first. */
    {
        static JceCvar *s_cv_near;
        if (!s_cv_near)
            s_cv_near = jce_cvar_register_float(
                "r.shadow.near", 0.0f, JCE_CVAR_FLAG_NONE,
                "Cascade partition near plane. 0 = engine default "
                "(JCE_CSM_SHADOW_NEAR).");
        if (s_cv_near) {
            const float ov = jce_cvar_get_float(s_cv_near);
            if (ov > 0.0f) shadow_near = ov;
        }
    }

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
        /* The bound is the distance from the CAMERA to the farthest corner of
         * the scene -- NOT the scene's own diagonal.
         *
         * The shader compares against fragDepth, which is measured from the
         * camera, so a bound expressed in scene-local size answers a different
         * question than the one being asked.  A camera 1500 m out from a 547 m
         * island has every fragment at depth 1200-1800; capping the shadow
         * range at the 547 m diagonal puts the ENTIRE scene past the range and
         * renders all of it unshadowed.  Measured on this scene at the `slope`
         * framing: pixels shadowed by more than 24 levels went 0.001% at
         * distance 260, 0.205% at 800, 0.855% at 2000 -- a 75x recovery from
         * nothing but reach, with no cascade or bias change.  The first
         * version of this cap used the diagonal and was wrong in exactly that
         * way; it is kept in the history rather than the code.
         *
         * The farthest corner is still a real bound -- it never shadows past
         * the geometry -- and it is tight: the eight corners are evaluated
         * rather than the centre-plus-radius sphere, which would overshoot by
         * the radius on every axis. */
        float scene_cap = 0.0f;               /* 0 = scene extent unknown */
        if (sr->scene_world_valid && camera) {
            const jce_vec3 cp  = jce_camera_get_position(camera);
            const jce_vec3 lo  = sr->scene_world_min;
            const jce_vec3 hi  = sr->scene_world_max;
            for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
            for (int cz = 0; cz < 2; ++cz) {
                const jce_vec3 corner = jce_v3(cx ? hi.x : lo.x,
                                               cy ? hi.y : lo.y,
                                               cz ? hi.z : lo.z);
                const float dcorner = jce_v3_len(jce_v3_sub(corner, cp));
                if (dcorner > scene_cap) scene_cap = dcorner;
            }
            if (scene_cap > 1.0f) {
                if (view_reach > scene_cap) view_reach = scene_cap;
            } else {
                scene_cap = 0.0f;
            }
        }
        if (view_reach > shadow_far_target) {
            /* Snap up to the next power-of-two multiple of the authored distance
             * so the value only takes a handful of discrete steps across a full
             * zoom sweep (no per-frame wobble => texel-snap stability holds). */
            float bucket = shadow_far_target > 1.0f ? shadow_far_target : 1.0f;
            while (bucket < view_reach) bucket *= 2.0f;
            /* The bucket is deliberately NOT capped back to scene_cap.
             *
             * scene_cap is a camera-relative distance, so it changes every
             * frame; clamping the result to it would hand the cascade fit a
             * continuously-varying far plane and undo the whole reason this
             * value is bucketed -- per-frame wobble in shadow_far propagates
             * into every cascade radius and texel size, which is the
             * parallel-stripe shimmer the deadband below was added to kill.
             * Letting the ladder overshoot keeps the result on a handful of
             * discrete rungs, and overshooting the far bound costs only
             * precision, while undershooting it costs the shadows themselves.
             * That asymmetry is why the ladder rounds up. */

            /* HYSTERESIS on the way DOWN.
             *
             * The ladder's input is view_reach, bounded above by scene_cap --
             * the distance from the CAMERA to the farthest corner of the scene
             * bounds -- so it moves about a metre per metre walked (measured:
             * 0.2 m per frame over a 270 m walk).  Its output is a step
             * function of that input, so a sub-metre step across a rung
             * DOUBLES shadow_far.
             *
             * Doubling shadow_far is not a quiet internal change.  It doubles
             * every split, every cascade radius, the texel-footprint cull
             * threshold, and -- visibly -- u_csmSplits.w, which fs_pbr_body.sh
             * uses as the ORIGIN OF THE SHADOW-DISTANCE FADE:
             *
             *     float fade = smoothstep(far * 0.85, far, fragDepth);
             *     shadow = mix(shadow, 1.0, fade);
             *
             * so every receiver between 0.85*far and 2*far flips between fully
             * shadowed and fully lit in ONE FRAME.  That is "整块阴影一下子
             * 消失/出现, at moderate distance, when the view moves" exactly.
             * A player standing on a rung with sub-millimetre controller
             * jitter strobes that whole depth band at frame rate, and pays a
             * full four-cascade re-render every frame while doing it, because
             * csm_key.zfar changes and the cascade cache misses.
             *
             * The deadband below cannot filter it: a 2x change clears a 5%
             * trigger by a factor of twenty, in either direction.
             *
             * Measured on this engine: the game camera's cam_far is 1000, so
             * view_reach = min(250, scene_cap), while the shipped quality
             * presets author {20, 40, 150} -- view_reach exceeds the authored
             * distance in 99.9% of frames, i.e. the ladder is live by default
             * in Play.  (hidden_cove authors 260 and is therefore the one
             * scene where it never fires, which is why the campaign that
             * measured only that scene cleared everything and found nothing.)
             *
             * So: step UP at the rung, step DOWN only once the input has
             * fallen 10% past it.  The band is wider than the deadband below,
             * so the two do not interact. */
            shadow_far_target = bucket;
        }

        /* The guard has to sit OUTSIDE the branch above, because the
         * oscillation happens on ENTERING AND LEAVING it, not inside it.
         *
         * With an authored distance of 150 and view_reach dithering around
         * that value: at 150.1 the branch is taken and the ladder returns 300;
         * at 149.9 the branch is skipped entirely and the target is 150 again.
         * A guard placed inside could only ever see the first of those two. */
        if (sr->shadow_far_valid &&
            shadow_far_target < sr->shadow_far_cached &&
            view_reach > sr->shadow_far_cached * 0.45f)
            shadow_far_target = sr->shadow_far_cached;
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

    /* Companion to the CSMLOG probe at the uniform upload: the INPUTS that
     * chose this range.  Logged because inferring them from the resulting
     * splits was wrong twice -- the arithmetic is min, ladder, deadband and
     * cache in sequence, and reading the output tells you which of the four
     * bound only if you already knew the answer. */
    {
        static SDL_IOStream *s_reach_log;
        static int           s_reach_tried;
        if (!s_reach_tried) {
            const char *e = getenv("JCE_DBG_CSM_LOG");
            s_reach_tried = 1;
            if (e && e[0] && e[0] != '0') {
                char path[512];
                snprintf(path, sizeof path, "%s.reach", e);
                s_reach_log = SDL_IOFromFile(path, "wb");
            }
        }
        if (s_reach_log) {
            static uint32_t s_rf;
            jce_vec3 cp = camera ? jce_camera_get_position(camera) : jce_v3(0,0,0);
            float cap = 0.0f;
            if (sr->scene_world_valid) {
                for (int cx = 0; cx < 2; ++cx)
                for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz) {
                    jce_vec3 c = jce_v3(cx ? sr->scene_world_max.x : sr->scene_world_min.x,
                                        cy ? sr->scene_world_max.y : sr->scene_world_min.y,
                                        cz ? sr->scene_world_max.z : sr->scene_world_min.z);
                    float dd = jce_v3_len(jce_v3_sub(c, cp));
                    if (dd > cap) cap = dd;
                }
            }
            char rline[320];
            int rn = snprintf(rline, sizeof rline,
                    "REACH f=%u cam=(%.1f,%.1f,%.1f) cam_far=%.1f near=%.2f "
                    "far=%.1f scene_valid=%d cap=%.1f world=[%.1f,%.1f,%.1f]..[%.1f,%.1f,%.1f]\n",
                    s_rf++, (double)cp.x, (double)cp.y, (double)cp.z,
                    (double)cam_far, (double)shadow_near, (double)shadow_far,
                    sr->scene_world_valid ? 1 : 0, (double)cap,
                    (double)sr->scene_world_min.x, (double)sr->scene_world_min.y,
                    (double)sr->scene_world_min.z, (double)sr->scene_world_max.x,
                    (double)sr->scene_world_max.y, (double)sr->scene_world_max.z);
            if (rn > 0) SDL_WriteIO(s_reach_log, rline, (size_t)rn);
            SDL_FlushIO(s_reach_log);
        }
    }
    if (shadow_far <= shadow_near)
        shadow_far = shadow_near + CSM_DIST_MIN;

    jce_mat4 cam_view = camera ? jce_camera_view(camera) : jce_m4_identity();
    jce_vec3 light_dir = shadow_dir;

    /* ── Sun-motion quantisation ───────────────────────────────────────────
     * Everything downstream keys on light_dir by exact compare, which is right
     * -- but it means a day/night cycle rebuilds every cascade every frame for
     * a shadow map that is texel-for-texel identical.  Hold the sun the SHADOWS
     * use still until it has moved far enough to change a texel.
     *
     * The quantum is derived from cascade 0 (finest texels = strictest bound)
     * and the scene's vertical extent (the tallest caster).  Both come from
     * last frame; when either is unavailable the quantum is 0, which means
     * "re-render every frame" -- the safe direction, since the opposite failure
     * is a frozen shadow that nothing reports. */
    static int s_sunquant_off = -1;
    if (s_sunquant_off < 0) {
        const char *v = getenv("JCE_DISABLE_SUNQUANT");
        s_sunquant_off = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (!s_sunquant_off) {
        const float caster_h = sr->scene_world_valid
            ? (sr->scene_world_max.y - sr->scene_world_min.y) : 0.0f;
        const float extent = (sr->last_csm_valid && sr->csm_cascade_count > 0)
            ? (2.0f * sr->last_csm.radius[0]) : 0.0f;
        const float q = jce_shadow_sun_quantum(extent, sr->shadow_map_size,
                                               caster_h);
        const float in[3]  = { light_dir.x, light_dir.y, light_dir.z };
        float       out[3] = { in[0], in[1], in[2] };
        jce_shadow_sun_update(&sr->shadow_sun_hold, in, q, out);
        light_dir.x = out[0]; light_dir.y = out[1]; light_dir.z = out[2];
    }

    /* Skip the per-cascade frustum/sphere/matrix recompute when none of the
       inputs changed since last frame (static camera + light). The key compare
       (~25 float eqs + one mat4 memcmp) is far cheaper than jce_csm_compute().
       Exact compare is safe: identical inputs are bit-identical (jce_camera_view
       is deterministic, shadow_far is coarse-bucketed) and any change recomputes. */
    /* Cascade near-plane fit bound.  MUST bound dynamic casters too: jce_csm.c
     * uses this AABB only to size near_extend (the light-axis push toward the
     * sun so tall casters between the cascade sphere and the sun clear the near
     * plane).  Fitting it to a STATIC-only bound clips the shadow of any mover
     * that rises higher toward the sun than all static geometry in a cascade
     * footprint (projectile over a low wall, a jump on flat terrain) — a real
     * truncation regression (two independent audits).  VP stabilization against
     * movers therefore belongs in the DUAL shadow-map path (static map fits the
     * static-only bound safely because movers go in a SEPARATE full-range dynamic
     * map); the shared single-map path here stays on the all-entity bound. */
    const bool     fit_valid = sr->scene_world_valid;
    const jce_vec3 fit_min   = sr->scene_world_min;
    const jce_vec3 fit_max   = sr->scene_world_max;

    /* Split the change detection by CAUSE so JCE_DBG_CSM_STATS can attribute
     * each recompute to camera motion (view) vs caster-bound churn (bound) vs
     * config (other).  This makes the static-fit fix measurable independent of
     * mover positions: a working fix drives `bound` toward zero. */
    const bool csm_view_changed =
        memcmp(&sr->csm_key.view, &cam_view, sizeof(jce_mat4)) != 0;
    const bool csm_bound_changed =
        sr->csm_key.caster_valid != fit_valid ||
        (fit_valid &&
            (sr->csm_key.caster_min.x != fit_min.x ||
             sr->csm_key.caster_min.y != fit_min.y ||
             sr->csm_key.caster_min.z != fit_min.z ||
             sr->csm_key.caster_max.x != fit_max.x ||
             sr->csm_key.caster_max.y != fit_max.y ||
             sr->csm_key.caster_max.z != fit_max.z));
    const bool csm_other_changed =
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
        sr->csm_key.light_dir.z != light_dir.z;

    JceCsmData csm;
    bool csm_changed = csm_view_changed || csm_bound_changed || csm_other_changed;

    /* ── Mechanism instrumentation (JCE_DBG_CSM_STATS=1) ─────────────────── */
    static int s_dbg_csm = -1;
    if (s_dbg_csm < 0) {
        const char *v = getenv("JCE_DBG_CSM_STATS");
        s_dbg_csm = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    static uint64_t s_csm_frames = 0, s_csm_recompute = 0,
                    s_csm_view = 0, s_csm_bound = 0, s_csm_other = 0;
    /* All counters (these + the file-scope cascade-hit tallies incremented in
     * the loops below) are CUMULATIVE since process start — never reset.  Each
     * logged line is a running total; recover a per-window rate by subtracting
     * the previous line's value.  Cumulative is deliberate: it keeps the numbers
     * monotonic and lets any window be reconstructed post-hoc. */
    if (s_dbg_csm) {
        s_csm_frames++;
        if (csm_changed)       s_csm_recompute++;
        if (csm_view_changed)  s_csm_view++;
        if (csm_bound_changed) s_csm_bound++;
        if (csm_other_changed) s_csm_other++;
        if ((s_csm_frames % 120u) == 0u) {
            LOG_INFO(LOG_TAG,
                "csm-stats: frames=%llu recompute=%llu (view=%llu bound=%llu other=%llu) | "
                "cascade eval=%llu hit=%llu | pc_on=%d dyn=%u",
                (unsigned long long)s_csm_frames,
                (unsigned long long)s_csm_recompute,
                (unsigned long long)s_csm_view,
                (unsigned long long)s_csm_bound,
                (unsigned long long)s_csm_other,
                (unsigned long long)g_sr_dbg_cascade_eval,
                (unsigned long long)g_sr_dbg_cascade_hit,
                sr_shadow_percascade_active(sr) ? 1 : 0,
                sr->shadow_dyn_count);
        }
    }

    if (csm_changed) {
        jce_csm_compute(&csm, sr->csm_cascade_count,
                        shadow_near, shadow_far, cam_fov, aspect,
                        &cam_view, &light_dir,
                        sr->homogeneous_depth, sr->shadow_map_size,
                        split_lambda,
                        fit_valid ? &fit_min : NULL,
                        fit_valid ? &fit_max : NULL);
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
        sr->csm_key.caster_valid = fit_valid;
        sr->csm_key.caster_min   = fit_min;
        sr->csm_key.caster_max   = fit_max;
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
    /* V4: also populate the cascade planes when meshlet shadows can run, so
     * the hero cluster-cull works even when the instanced GPU-driven path is
     * off (the plane extraction is cheap and idempotent). */
    if (sr->gpu_driven_frame ||
        (sr_mlcull_enabled() && jce_gpu_scene_meshlet_supported(sr->gpu_scene))) {
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

    /* Dual shadow maps active this frame: needs the flag, percascade tracking
     * (dynamics in shadow_dyn), and a valid atlas.  When on, movers are NOT
     * gathered into the static cascade maps (they go to the dynamic atlas) and
     * the per-cascade dynamic cache gate is dropped (a mover no longer voids the
     * static cascade cache). */
    const bool dual_on = sr_shadow_dual_on()
                      && sr_shadow_percascade_active(sr)
                      && sr->dyn_csm_valid;

    /* #6 — build the shadow-caster grid ONCE so each cascade gathers only the
     * casters overlapping its (conservative) bounds.  On failure (or when there
     * are no AABB casters) the cascade falls back to the full-list scan — never a
     * correctness change.  has_grid gates the gather path per cascade. */
    uint64_t _ts_space = jce_time_perf_counter();
    const bool has_grid = sr_build_shadow_space(sr, list, scene);
    jce_perf_phase_add("sh_space", jce_time_perf_to_ms(_ts_space, jce_time_perf_counter()));
    /* Masked-last caster ordering (jce_shadow_bucket.h).  OFF by default, and the
     * off path deliberately does NOT call bgfx_set_view_mode at all: setting it
     * to the default explicitly is still a change, and any A/B taken against it
     * compares two configurations that both differ from what ships. */
    if (jce_shadow_masked_last_enabled()) {
        for (uint32_t c = 0; c < csm.cascade_count &&
                             c < JCE_CSM_MAX_CASCADES; c++)
            bgfx_set_view_mode((uint16_t)(view_id_base + 11u + c),
                               BGFX_VIEW_MODE_DEPTH_ASCENDING);
    }

    uint64_t _ts_gather = jce_time_perf_counter();

    /* Shadow-caster texel-footprint culling (standard-engine parity: UE
     * r.Shadow.RadiusThreshold / Unity small-shadow culling).  A caster whose
     * shadow spans fewer than `min_texels` shadow-map texels in a cascade is
     * sub-detail — skip it there.  The cascade radius scales the texel world
     * size, so a small caster stays in the tight NEAR cascades (small texels →
     * large footprint) and only drops from the FAR ones, exactly where distant
     * clutter dominates the caster count + the re-render cost.  A shadow below a
     * texel cannot resolve at all, so the 1.5-texel default is visually
     * conservative.  JCE_SHADOW_CASTER_MIN_TEXELS=0 disables (byte-identical). */
    static float s_shadow_min_texels = -1.0f;
    if (s_shadow_min_texels < 0.0f) {
        const char *v = getenv("JCE_SHADOW_CASTER_MIN_TEXELS");
        s_shadow_min_texels = (v && v[0]) ? (float)atof(v) : 1.5f;
        if (s_shadow_min_texels < 0.0f) s_shadow_min_texels = 0.0f;
    }
    const float shadow_lod_texels = s_shadow_min_texels;

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
            /* Per-cascade mode: dynamics are un-gridded — append them all;    \
             * the cascade body's fine cull drops the non-overlapping ones.    \
             * Dual mode: movers render into the SEPARATE dynamic atlas, so do  \
             * NOT append them to the static cascade (keeps it cacheable). */    \
            if (sr_shadow_percascade_active(sr) && !dual_on)                          \
                for (uint32_t n = 0; n < sr->shadow_dyn_count &&               \
                                     total < sr->shadow_query_cap; n++)        \
                    sr->shadow_query[total++] = sr->shadow_dyn[n];              \
            idx_buf = sr->shadow_query;                                         \
            idx_n   = (int)total;                                               \
        }

    /* ── CSM shadow-map cache (UE-style) ──────────────────────────────────
     * A cascade whose snapped light VP is bit-identical to the render its
     * csm_fbo[c] currently holds AND whose caster set is unchanged keeps that
     * depth — its clear+gather+submit is skipped, collapsing the per-cascade
     * gather cost to ~0.  Gated OFF when any dynamic caster exists (those mutate
     * without an xform_gen bump) and by JCE_DISABLE_SHADOWCACHE for A/B. */
    static int s_shadowcache_disabled = -1;
    if (s_shadowcache_disabled < 0) {
        const char *v = getenv("JCE_DISABLE_SHADOWCACHE");
        s_shadowcache_disabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    const uint64_t sh_struct_epoch = jce_scene_get_structural_epoch(scene);
    const bool sh_global_clean =
        !s_shadowcache_disabled
        && (sr_shadow_percascade_active(sr) || !sr->shadow_has_dynamic_caster)
        /* The GPU-driven shadow cull renders each cascade via the GPUScene compute
         * path; caching skips re-rendering a cascade whose VP + caster set are
         * unchanged (csm_fbo keeps the last valid depth), which is correct
         * regardless of HOW the cascade was produced — the map is identical.  So
         * the cache applies to the GPU-driven path too (it previously bailed here,
         * making static-camera GPU-driven scenes re-gather every frame). */
        /* Dual mode keys the static cascade cache on the STATIC-only caster key,
         * so a mover's xform_gen churn (folded into the full key) does not void
         * it — the mover is in the dynamic atlas, not this map. */
        && (dual_on
              ? (sr->shadow_cache_static_caster_key == sr->shadow_static_caster_key)
              : (sr->shadow_cache_caster_key        == sr->shadow_caster_key))
        && sr->shadow_cache_struct_epoch == sh_struct_epoch
        && sr->shadow_cache_cascades     == csm.cascade_count
        && sr->shadow_cache_map_size     == sr->shadow_map_size;
    #define SR_CASCADE_CACHE_HIT(c)                                            \
        (sh_global_clean && sr->shadow_cache_valid[c] &&                       \
         memcmp(&csm.vp[c], &sr->shadow_cache_vp[c], sizeof(jce_mat4)) == 0 && \
         /* per-cascade dynamic gate: a mover overlapping this cascade NOW,   \
          * or at its LAST render (must erase its old shadow), dirties it.    \
          * With percascade off shadow_dyn_count is irrelevant because        \
          * sh_global_clean already carried the ANY-dynamic veto.  DUAL mode  \
          * drops this gate entirely: movers live in the dynamic atlas, so a  \
          * mover no longer dirties the static cascade cache (the whole win). */ \
         (dual_on ||                                                          \
          !(sr_shadow_percascade_active(sr) &&                                      \
            (sr->shadow_cache_dyn_c[c] ||                                      \
             sr_cascade_has_dyn(sr, scull_right, scull_up,                     \
                                csm.center[c], csm.radius[c])))))
    #define SR_CASCADE_CACHE_STORE(c)                                          \
        do { sr->shadow_cache_vp[c] = csm.vp[c];                              \
             sr->shadow_cache_valid[c] = true;                                 \
             sr->shadow_cache_dyn_c[c] =                                       \
                 sr_shadow_percascade_active(sr) &&                                  \
                 sr_cascade_has_dyn(sr, scull_right, scull_up,                 \
                                    csm.center[c], csm.radius[c]); } while (0)

    /* Per-frame counters for the cascade gather (JCE_DBG_SHPROF=1).  Counters,
     * not timers: this loop runs once per candidate PER CASCADE -- ~200k x 4 on
     * a moving camera -- and bracketing it would cost more than it measures. */
    static int s_shprof = -1;
    if (s_shprof < 0) { const char *v = getenv("JCE_DBG_SHPROF");
                        s_shprof = (v && v[0] && v[0] != '0') ? 1 : 0; }
    uint32_t shp_casc = 0, shp_cand = 0, shp_full = 0, shp_dis = 0, shp_nocast = 0,
             shp_lod = 0, shp_frust = 0, shp_texel = 0, shp_ladder = 0;
    /* Per-cascade split: the aggregate said the frustum cull drops 1.5-4%, but
     * a near cascade and a far one have nothing in common -- one number over
     * both is an average of two different questions. */
    uint32_t shp_c_cand[JCE_CSM_MAX_CASCADES] = {0};
    uint32_t shp_c_frust[JCE_CSM_MAX_CASCADES] = {0};
    uint32_t shp_c_ladder[JCE_CSM_MAX_CASCADES] = {0};
    float    shp_c_radius[JCE_CSM_MAX_CASCADES] = {0};

    /* Arm the per-pass mesh-resolve memo (see sh_res_* in jce_sr_internal.h).
     * One memset of list->count bytes replaces up to (cascades-1) x list->count
     * ECS lookups. */
    if (list->count > 0) {
        if (sr->sh_res_cap < (uint32_t)list->count) {
            uint32_t nc = sr->sh_res_cap ? sr->sh_res_cap : 1024u;
            while (nc < (uint32_t)list->count) nc *= 2u;
            JceMesh **nm = (JceMesh **)JCE_REALLOC(sr->sh_res_mesh,
                                                   nc * sizeof(JceMesh *));
            uint8_t  *ns = (uint8_t *)JCE_REALLOC(sr->sh_res_state, nc);
            if (nm) sr->sh_res_mesh  = nm;
            if (ns) sr->sh_res_state = ns;
            if (nm && ns) sr->sh_res_cap = nc;
        }
        if (sr->sh_res_state && sr->sh_res_cap >= (uint32_t)list->count)
            memset(sr->sh_res_state, 0, (size_t)list->count);
    }

    if (use_rq_shadow) {
        jce_rq_clear(sr->render_queue);
        jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
        for (uint32_t c = 0; c < csm.cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
            uint16_t cv = (uint16_t)(view_id_base + 11 + c);

            /* Cache hit: skip clear+gather+submit; csm_fbo[c] keeps last render. */
            { const bool _cache_hit = SR_CASCADE_CACHE_HIT(c);
              if (s_dbg_csm) { g_sr_dbg_cascade_eval++;
                               if (_cache_hit) g_sr_dbg_cascade_hit++; }
              if (_cache_hit) continue; }
            /* Far-cascade round-robin (pixel-changing). JCE_CSM_FAR_INTERVAL,
             * default 3 -- see sr_csm_far_interval, which is the authority.
             * This said "default off" while the function returned 3. */
            if (sr_csm_far_defer(sr, c, dual_on, scull_right, scull_up,
                                 csm.center[c], csm.radius[c])) continue;

            bgfx_set_view_rect(cv, 0, 0, sr->shadow_map_size, sr->shadow_map_size);
            bgfx_set_view_frame_buffer(cv, sr->csm_fbo[c]);
            bgfx_set_view_clear(cv, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            bgfx_set_view_transform(cv, identity, csm.vp[c].raw[0]);
            bgfx_touch(cv);
            SR_CASCADE_CACHE_STORE(c);  /* this cascade is (re)rendered this frame */

            SR_CASCADE_GATHER(c, cand, cand_n);
            const int iter_n = (cand_n >= 0) ? cand_n : list->count;
            shp_casc++;
            if (cand_n >= 0) shp_cand += (uint32_t)cand_n;
            else             shp_full += (uint32_t)list->count;
            if (c < JCE_CSM_MAX_CASCADES) {
                shp_c_cand[c]  += (uint32_t)iter_n;
                shp_c_radius[c] = csm.radius[c];
            }
            const bool kc = sr->kindcache_on;
            for (int k = 0; k < iter_n; k++) {
                const int i = (cand_n >= 0) ? (int)cand[k] : k;
                JceEntity e = list->entities[i];
                const SrEntityCull *ec = &sr->ecull[i];
                /* Fix #1: a cached PRIM_MESH/MODEL skips the skinned/terrain probes
                 * (provably false); the mesh-renderer shadow batch still runs. */
                const bool kc_fast = kc && SR_RK_IS_FAST(ec->render_kind);
                if (kc ? (ec->render_kind == SR_RK_DISABLED)
                       : !entity_enabled(scene, e)) { shp_dis++; continue; }
                if (!ec->casts_shadow) { shp_nocast++; continue; }
                if (ec->lod_culled) { shp_lod++; continue; }  /* LODGroup far-cull (P1 #6) */
                if (ec->has_aabb &&
                    sr_caster_culled_for_cascade(&ec->wmin, &ec->wmax,
                                                 scull_right, scull_up,
                                                 csm.center[c], csm.radius[c]))
                    { shp_frust++; if (c < JCE_CSM_MAX_CASCADES) shp_c_frust[c]++;
                      continue; }
                /* Texel-footprint LOD (see shadow_lod_texels above): drop a
                 * caster whose shadow is sub-detail in this cascade. */
                if (shadow_lod_texels > 0.0f && ec->has_aabb) {
                    const float dx = ec->wmax.x - ec->wmin.x;
                    const float dy = ec->wmax.y - ec->wmin.y;
                    const float dz = ec->wmax.z - ec->wmin.z;
                    const float diag = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (!jce_shadow_caster_resolvable(diag, csm.radius[c],
                                                      sr->shadow_map_size,
                                                      shadow_lod_texels))
                        { shp_texel++; continue; }
                }
                shp_ladder++;
                sr->stat_shadow_ladder++;
                if (c < JCE_CSM_MAX_CASCADES) shp_c_ladder[c]++;
                if (!kc_fast && sr_try_submit_bindpose_shadow(sr, scene, e, i, cv))
                    continue;
                if (!kc_fast && sr_try_submit_skinned_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_meshlet_shadow(sr, scene, e, i, cv, c))
                    continue;
                /* kc classification trust: a kc_fast PRIM_MESH is provably not
                 * gltf-backed and (with FoliageCluster in the classify ladder)
                 * carries no foliage — skip both probes. */
                if ((ec->render_kind == SR_RK_MODEL || !kc_fast) &&
                    sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, cv))
                    continue;
                if (!kc_fast && sr_try_submit_foliage_shadow(sr, scene, e, cv,
                        (uint16_t)shadow_inst_sh.idx, (uint16_t)shadow_sh.idx))
                    continue;
                if (!kc_fast && sr_try_submit_terrain_shadow(sr, scene, e, cv, &csm.vp[c]))
                    continue;
                /* Mesh resolve is cascade-independent: do it on the first
                 * cascade that reaches this entity, reuse it on the rest.  The
                 * world matrix needs no memo -- when world_valid it is a copy
                 * out of ecull_world, which sr_build_entity_model itself uses. */
                jce_mat4 model;
                JceMesh *mesh = NULL;
                const bool memo_ok = sr->sh_res_state &&
                                     sr->sh_res_cap >= (uint32_t)list->count;
                if (memo_ok && sr->sh_res_state[i] != 0u) {
                    if (sr->sh_res_state[i] == 2u) continue;   /* known: no mesh */
                    mesh = sr->sh_res_mesh[i];
                    if (ec->world_valid) model = sr->ecull_world[i];
                    else if (!sr_build_entity_model(sr, scene, e, i, &model,
                                                    NULL, NULL)) continue;
                } else {
                    const bool built =
                        sr_build_entity_model(sr, scene, e, i, &model, &mesh, NULL);
                    if (memo_ok) {
                        sr->sh_res_mesh[i]  = mesh;
                        sr->sh_res_state[i] = (built && mesh) ? 1u : 2u;
                    }
                    if (!built || !mesh) continue;
                }
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
            /* 千万 ③: scatter fields cast shadows as ONE instanced depth draw
             * per cascade at a reduced LOD — outside the entity walk (scatter
             * carries no ecull caster AABB; the cache slots know the fields). */
            sr_submit_scatter_shadows(sr, scene, cv, (uint16_t)shadow_inst_sh.idx);
        }
        jce_perf_phase_add("sh_gather", jce_time_perf_to_ms(_ts_gather, jce_time_perf_counter()));
        if (s_shprof) {
            static uint32_t s_shn = 0;
            if ((s_shn++ % 120u) == 0u)
                LOG_INFO(LOG_TAG,
                    "shprof: %u/%u cascades rendered, cand=%u full=%u | dropped: "
                    "disabled=%u nocast=%u lod=%u cascade-frustum=%u texel=%u | "
                    "reached ladder=%u | texel_thresh=%.2f grid=%d | far_defer "
                    "declined: near=%llu uncached=%llu moverframe=%llu hasdyn=%llu"
                    " | dyn_casters=%u percascade=%d",
                    shp_casc, csm.cascade_count, shp_cand, shp_full, shp_dis,
                    shp_nocast, shp_lod, shp_frust, shp_texel, shp_ladder,
                    (double)shadow_lod_texels, (int)has_grid,
                    (unsigned long long)g_sr_dbg_defer_why[0],
                    (unsigned long long)g_sr_dbg_defer_why[1],
                    (unsigned long long)g_sr_dbg_defer_why[2],
                    (unsigned long long)g_sr_dbg_defer_why[3],
                    sr->shadow_dyn_count, (int)sr_shadow_percascade_active(sr));
            /* Does the cached cascade matrix ever DIFFER from this frame's?
             * That is the precondition for the atlas/static pairing to matter
             * at all -- if it is zero, sr_cascade_sample_vp is an expensive way
             * of writing csm->vp[c] and no capture can show the fix working. */
            LOG_INFO(LOG_TAG,
                "shprof:   cascade VP mismatch %llu/%llu %llu/%llu %llu/%llu %llu/%llu"
                " (mismatch/asked, per cascade)",
                (unsigned long long)g_sr_dbg_vp_mismatch[0],
                (unsigned long long)g_sr_dbg_vp_asked[0],
                (unsigned long long)g_sr_dbg_vp_mismatch[1],
                (unsigned long long)g_sr_dbg_vp_asked[1],
                (unsigned long long)g_sr_dbg_vp_mismatch[2],
                (unsigned long long)g_sr_dbg_vp_asked[2],
                (unsigned long long)g_sr_dbg_vp_mismatch[3],
                (unsigned long long)g_sr_dbg_vp_asked[3]);
            for (uint32_t q = 0; q < csm.cascade_count &&
                                 q < JCE_CSM_MAX_CASCADES; q++)
                LOG_INFO(LOG_TAG,
                    "shprof:   cascade %u radius=%.1f cand=%u frustum-dropped=%u "
                    "(%.1f%%) ladder=%u", q, (double)shp_c_radius[q],
                    shp_c_cand[q], shp_c_frust[q],
                    shp_c_cand[q] ? 100.0 * shp_c_frust[q] / shp_c_cand[q] : 0.0,
                    shp_c_ladder[q]);
        }
        uint64_t _ts_flush = jce_time_perf_counter();
        if (jce_rq_count(sr->render_queue) > 0) {
            jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
            sr_rq_flush_and_collect(sr);
        }
        jce_perf_phase_add("sh_flush", jce_time_perf_to_ms(_ts_flush, jce_time_perf_counter()));
    } else {
        for (uint32_t c = 0; c < csm.cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
            uint16_t cv = (uint16_t)(view_id_base + 11 + c);

            /* Cache hit: skip clear+gather+submit; csm_fbo[c] keeps last render. */
            { const bool _cache_hit = SR_CASCADE_CACHE_HIT(c);
              if (s_dbg_csm) { g_sr_dbg_cascade_eval++;
                               if (_cache_hit) g_sr_dbg_cascade_hit++; }
              if (_cache_hit) continue; }
            /* Far-cascade round-robin (pixel-changing). JCE_CSM_FAR_INTERVAL,
             * default 3 -- see sr_csm_far_interval, which is the authority.
             * This said "default off" while the function returned 3. */
            if (sr_csm_far_defer(sr, c, dual_on, scull_right, scull_up,
                                 csm.center[c], csm.radius[c])) continue;

            bgfx_set_view_rect(cv, 0, 0, sr->shadow_map_size, sr->shadow_map_size);
            bgfx_set_view_frame_buffer(cv, sr->csm_fbo[c]);
            bgfx_set_view_clear(cv, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            bgfx_set_view_transform(cv, identity, csm.vp[c].raw[0]);
            bgfx_touch(cv);
            SR_CASCADE_CACHE_STORE(c);  /* this cascade is (re)rendered this frame */

            SR_CASCADE_GATHER(c, cand, cand_n);
            const int iter_n = (cand_n >= 0) ? cand_n : list->count;
            const bool kc = sr->kindcache_on;
            for (int k = 0; k < iter_n; k++) {
                const int i = (cand_n >= 0) ? (int)cand[k] : k;
                JceEntity e = list->entities[i];
                const SrEntityCull *ec = &sr->ecull[i];
                /* Fix #1: a cached PRIM_MESH/MODEL skips the skinned/terrain probes
                 * (provably false); the mesh-renderer shadow batch still runs. */
                const bool kc_fast = kc && SR_RK_IS_FAST(ec->render_kind);
                if (kc ? (ec->render_kind == SR_RK_DISABLED)
                       : !entity_enabled(scene, e)) continue;
                if (!ec->casts_shadow) continue;
                if (ec->lod_culled) continue;  /* LODGroup far-cull (P1 #6) */
                if (ec->has_aabb &&
                    sr_caster_culled_for_cascade(&ec->wmin, &ec->wmax,
                                                 scull_right, scull_up,
                                                 csm.center[c], csm.radius[c]))
                    continue;
                /* Texel-footprint LOD (see shadow_lod_texels above): drop a
                 * caster whose shadow is sub-detail in this cascade. */
                if (shadow_lod_texels > 0.0f && ec->has_aabb && csm.radius[c] > 0.0f) {
                    const float dx = ec->wmax.x - ec->wmin.x;
                    const float dy = ec->wmax.y - ec->wmin.y;
                    const float dz = ec->wmax.z - ec->wmin.z;
                    const float diag = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (diag * (float)sr->shadow_map_size
                            < shadow_lod_texels * 2.0f * csm.radius[c])
                        continue;
                }
                if (!kc_fast && sr_try_submit_bindpose_shadow(sr, scene, e, i, cv))
                    continue;
                if (!kc_fast && sr_try_submit_skinned_shadow(sr, scene, e, i, cv))
                    continue;
                if (sr_try_submit_meshlet_shadow(sr, scene, e, i, cv, c))
                    continue;
                /* kc classification trust: a kc_fast PRIM_MESH is provably not
                 * gltf-backed and (with FoliageCluster in the classify ladder)
                 * carries no foliage — skip both probes. */
                if ((ec->render_kind == SR_RK_MODEL || !kc_fast) &&
                    sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, cv))
                    continue;
                if (!kc_fast && sr_try_submit_foliage_shadow(sr, scene, e, cv,
                        (uint16_t)shadow_inst_sh.idx, (uint16_t)shadow_sh.idx))
                    continue;
                if (!kc_fast && sr_try_submit_terrain_shadow(sr, scene, e, cv, &csm.vp[c]))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh, NULL)) continue;
                if (!mesh) continue;
                jce_enc_set_transform(model.raw[0], 1);
                jce_mesh_submit_shadow(mesh, sr->renderer, cv);
            }
            /* 千万 ③: scatter shadows (see the rq branch above). */
            sr_submit_scatter_shadows(sr, scene, cv, (uint16_t)shadow_inst_sh.idx);
        }
    }

    /* Dual shadow maps: after the static cascades (now mover-free + cached),
     * render the dynamic casters into the separate atlas the PBR shader min()s. */
    if (dual_on)
        sr_draw_dyn_csm_pass(sr, scene, list, &csm, view_id_base,
                             scull_right, scull_up);

    #undef SR_CASCADE_GATHER

    /* Commit the cache key for next frame: per-cascade VPs were stored as each
     * cascade rendered; record the caster-set state they correspond to. */
    sr->shadow_cache_caster_key   = sr->shadow_caster_key;
    sr->shadow_cache_static_caster_key = sr->shadow_static_caster_key;
    sr->shadow_cache_struct_epoch = sh_struct_epoch;
    sr->shadow_cache_cascades     = csm.cascade_count;
    sr->shadow_cache_map_size     = sr->shadow_map_size;
    #undef SR_CASCADE_CACHE_HIT
    #undef SR_CASCADE_CACHE_STORE
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

    /* Lazily allocated: the atlas exists only once a frame actually has a
     * local shadow caster.  Bail rather than bind an invalid framebuffer. */
    if (!sr_ensure_local_atlas(sr)) return;

    /* NOT cached across frames, and two attempts prove why it is not simple.
     *
     * (1) Skipping the whole pass on an unchanged key also skips the per-frame
     *     uniform bookkeeping the PBR pass reads (frame_local_vp[],
     *     frame_local_bias_slot[], frame_point_cube_vp[],
     *     frame_point_cube_base_slot[] -- the last reset to -1 at the top of
     *     the pass).  The lit frame fell back to the no-shadow picture while
     *     the depth was still valid: 0.008% of pixels differing from a build
     *     with the feature broken, UNDER a 0.025% noise floor.
     * (2) Keeping the bookkeeping and skipping only the clear + submits still
     *     produced a visibly different frame -- 5.95% of pixels against a
     *     0.079% floor -- so the atlas contents are not preserved across a
     *     frame in which its views are never submitted.  The cascade cache
     *     works because each cascade owns its own texture and FBO; the local
     *     tiles share ONE atlas cleared as a whole.
     *
     * A working cache has to establish that the shared atlas survives an
     * untouched frame (or keep touching the views), which is a bgfx-level
     * question, not a bookkeeping one.  Left unbuilt rather than shipped on a
     * guess: the pixel gate caught both attempts. */

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

    /* Frustum-cull casters against THIS tile's light matrix.
     *
     * The loop used to submit every shadow-casting entity in the scene to every
     * local tile -- two tests, no spatial rejection, no cache -- while the
     * cascade path next door has grid candidates, per-cascade caster culling,
     * an LOD and a 99.7%-hit skip cache.  A spot cone lights a small part of a
     * scene, so most of that work was submitting geometry the tile cannot see.
     * On graveyard, four spot tiles x ~269 casters is ~945 extra draws.
     *
     * The tile's own view-projection is the exact test -- tighter than the
     * light-space slab the cascades use, and it needs no extra state because
     * ecull[j] already holds the world AABB this frame. */
    jce_vec4 tile_planes[6];
    float    tile_absn[6][3];
    const bool tile_cull = sr->kindcache_on;   /* ecull AABBs are only valid then */
    if (tile_cull) {
        jce_frustum_extract_planes(vp, tile_planes);
        /* One frustum, every caster in the scene: hoist the sign selection. */
        jce_frustum_abs_normals(tile_planes, tile_absn);
    }

    for (int j = 0; j < list->count; j++) {
        JceEntity ee = list->entities[j];
        if (!entity_enabled(scene, ee)) continue;
        if (!sr->ecull[j].casts_shadow) continue;
        if (tile_cull && sr->ecull[j].has_aabb &&
            !jce_aabb_in_frustum_fast(tile_planes, tile_absn, sr->ecull[j].wmin,
                                      sr->ecull[j].wmax))
            continue;
        if (sr_try_submit_bindpose_shadow(sr, scene, ee, j, lv)) continue;
        if (sr_try_submit_skinned_shadow(sr, scene, ee, j, lv)) continue;
        if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, ee, j, lv)) continue;
        if (sr_try_submit_terrain_shadow(sr, scene, ee, lv, NULL)) continue;
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, ee, j, &model, &mesh, NULL)) continue;
        if (!mesh) continue;
        jce_enc_set_transform(model.raw[0], 1);
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

    /* Do NOT gate on local_atlas_valid here.
     *
     * The atlas became lazily allocated in d2df4f34 ("stop paying for unused
     * shadow targets"), which removed the eager assignment in
     * sr_create_shadow_targets but left this guard behind.  The only writer of
     * `true` is sr_ensure_local_atlas, whose only caller is
     * sr_local_shadow_render_tile -- downstream of this very return.  The
     * renderer is JCE_CALLOC'd, so the flag started false, this returned, the
     * allocator was never reached, and the flag stayed false forever: turning
     * on castsShadow for a spot or point light silently did nothing, while the
     * whole consumer side (s_localShadowMap at sampler 15, u_localShadowVP,
     * the 3x3 PCF in fs_pbr_body) shipped and waited.
     *
     * The lazy allocation is still lazy -- sr_local_shadow_render_tile calls
     * sr_ensure_local_atlas on the first real tile and bails if that fails, so
     * a frame with no local caster still allocates nothing. */
    if (!list) return;
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
    /* Neither shadow band claims a view range here, and that is correct --
     * I added claims for both and both were false positives.
     *
     * It looks like a gap: bgfx view state is last-write-wins, an overlap
     * makes one subsystem silently stop drawing, this engine has already had
     * that failure turn the whole screen black, and grepping
     * jce_view_bands_claim finds only scene-renderer and postfx. But
     * scene-renderer reserves the sparse bands itself in
     * jce_scene_renderer_view_order_build and claims them on their behalf:
     * measured, it claims [base+100..base+116] for the cube tiles. A second
     * claim over the same range reports the band colliding with its own
     * owner -- 300 lines a frame-run, none of them real.
     *
     * Recording it so the next reader does not re-derive the same wrong
     * conclusion from the same grep. Coverage lives at the reservation site,
     * not at the point of use. */
    /* 6 cardinal cube-face directions. ORDER MUST MATCH the shader major-axis
       face pick in fs_pbr_body.sh samplePointCubeShadow: {+X,-X,+Y,-Y,+Z,-Z}. */
    static const float CUBE_DIRS[6][3] = {
        { 1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f},
        { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f,-1.0f} };

    /* Spots — index aligns with u_spotLights[].  O(1) all-clear gate: a scene
     * with no spot lights (e.g. a 150k-primitive stress world) otherwise paid
     * a full 150k-entity has_spot_light probe walk EVERY frame (~6ms) inside
     * shadow_gather.  The byte-mirror pre-filter (ecull_light_byte) then skips
     * the non-light bulk cheaply when lights DO exist among many entities. */
    uint32_t spot_idx = 0;
    const bool kc_ls = sr->kindcache_on && sr->ecull_light_byte;
    if (jce_scene_count_spot_lights(scene) > 0)
    for (int li = 0; li < list->count && slot < JCE_MAX_LOCAL_SHADOWS; li++) {
        JceEntity e = list->entities[li];
        if (kc_ls && !sr->ecull_light_byte[li]) continue;
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
    if (jce_scene_count_point_lights(scene) > 0)
    for (int li = 0; li < list->count && slot < max_slot; li++) {
        JceEntity e = list->entities[li];
        if (kc_ls && !sr->ecull_light_byte[li]) continue;
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
