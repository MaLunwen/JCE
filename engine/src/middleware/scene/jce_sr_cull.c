/*
 * jce_sr_cull.c  Scene-renderer culling / TAA-velocity / baked-GI module
 * (split from jce_scene_renderer.c).
 *
 * The per-object TAA previous-world-matrix table, the SSAO + per-object motion
 * vector depth pre-pass, the distance/importance light selection, the
 * uniform-grid (broadphase) frustum cull, and the baked-GI (reflection probe +
 * light-probe SH9) consumption.  Pure move from the monolithic renderer:
 * cross-module entry points are declared in jce_sr_internal.h, everything else
 * stays file-static here.  No behaviour change.
 */

#include "jce_sr_internal.h"
#include <jce/os/core/jce_timer.h>   /* JCE_CULL_KPI broad-phase timing */

/* ── TAA per-object previous-world-matrix table ─────────────────────────
 * Look up an entity's previous-frame world matrix before drawing it into the
 * velocity G-buffer; update it after.  A simple linear-probe table keyed by
 * entity id, pruned each frame (untouched => evicted).  Only touched while TAA
 * wants velocity, so it is inert when TAA is off. */
static jce_mat4 *sr_prev_xform_find(JceSceneRenderer *sr, uint32_t entity)
{
    uint32_t h = (entity * 2654435761u) % SR_PREV_XFORM_MAX;
    for (uint32_t i = 0; i < SR_PREV_XFORM_MAX; i++) {
        uint32_t s = (h + i) % SR_PREV_XFORM_MAX;
        SrPrevXform *e = &sr->prev_xform[s];
        if (e->used && e->entity == entity) return &e->prev_world;
        if (!e->used) return NULL; /* open slot ends the probe chain */
    }
    return NULL;
}

/* Store/refresh an entity's world matrix for next frame and mark it touched. */
static void sr_prev_xform_store(JceSceneRenderer *sr, uint32_t entity,
                                const jce_mat4 *world)
{
    uint32_t h = (entity * 2654435761u) % SR_PREV_XFORM_MAX;
    int free_slot = -1;
    for (uint32_t i = 0; i < SR_PREV_XFORM_MAX; i++) {
        uint32_t s = (h + i) % SR_PREV_XFORM_MAX;
        SrPrevXform *e = &sr->prev_xform[s];
        if (e->used && e->entity == entity) {
            e->prev_world = *world;
            e->touched = true;
            return;
        }
        if (!e->used) { free_slot = (int)s; break; }
    }
    if (free_slot >= 0) {
        SrPrevXform *e = &sr->prev_xform[free_slot];
        e->used = true; e->entity = entity; e->prev_world = *world;
        e->touched = true;
    }
    /* Table full (>512 dynamic entities): silently skip — that entity falls
       back to camera-only motion (no per-object reprojection) this frame. */
}

/* Prune entries not touched this frame (dead/culled entities) so the table
   does not fill up over a long session. */
static void sr_prev_xform_prune(JceSceneRenderer *sr)
{
    for (uint32_t i = 0; i < SR_PREV_XFORM_MAX; i++) {
        SrPrevXform *e = &sr->prev_xform[i];
        if (e->used && !e->touched) { e->used = false; e->entity = 0; }
        e->touched = false;
    }
}

/* (Re)create the camera pre-pass targets at the viewport size:
 *   color0 = world-normal+roughness G-buffer (RGBA8, for SSR)
 *   color1 = TAA per-object motion vectors (RGBA16F, RG = NDC delta) — ONLY when
 *            want_velocity (TAA on); omitted otherwise so SSAO/SSR-only frames
 *            keep the legacy 2-attachment layout (byte-identical).
 *   depth  = the shared camera depth (SSAO + SSR both sample it).
 * Viewport-sized (not square) unlike sr_create_shadow_targets.  Recreated when
 * the size OR the velocity-attachment presence changes. */
void sr_ensure_ssao_target(JceSceneRenderer *sr, uint16_t w, uint16_t h,
                                  bool want_velocity)
{
    if (!sr || w == 0 || h == 0) return;
    if (sr->ssao_valid && sr->ssao_w == w && sr->ssao_h == h &&
        sr->ssao_has_velocity == want_velocity &&
        BGFX_HANDLE_IS_VALID(sr->ssao_depth_fbo))
        return;
    if (BGFX_HANDLE_IS_VALID(sr->ssao_depth_fbo)) {
        bgfx_destroy_frame_buffer(sr->ssao_depth_fbo);
        sr->ssao_depth_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->ssao_depth_tex)) {
        bgfx_destroy_texture(sr->ssao_depth_tex);
        sr->ssao_depth_tex.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->ssao_normal_tex)) {
        bgfx_destroy_texture(sr->ssao_normal_tex);
        sr->ssao_normal_tex.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->ssao_velocity_tex)) {
        bgfx_destroy_texture(sr->ssao_velocity_tex);
        sr->ssao_velocity_tex.idx = UINT16_MAX;
    }
    const uint64_t rt_flags = BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    sr->ssao_normal_tex = bgfx_create_texture_2d(w, h, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8, rt_flags, NULL);
    sr->ssao_depth_tex = bgfx_create_texture_2d(w, h, false, 1,
        sr->shadow_depth_fmt, rt_flags, NULL);
    if (want_velocity)
        sr->ssao_velocity_tex = bgfx_create_texture_2d(w, h, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA16F, rt_flags, NULL);

    bgfx_attachment_t at[3];
    memset(at, 0, sizeof(at));
    int n = 0;
    bgfx_attachment_init(&at[n++], sr->ssao_normal_tex, BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    if (want_velocity)
        bgfx_attachment_init(&at[n++], sr->ssao_velocity_tex, BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&at[n++], sr->ssao_depth_tex, BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    sr->ssao_depth_fbo = bgfx_create_frame_buffer_from_attachment((uint8_t)n, at, false);
    sr->ssao_w = w; sr->ssao_h = h;
    sr->ssao_has_velocity = want_velocity;
    sr->ssao_valid = BGFX_HANDLE_IS_VALID(sr->ssao_depth_fbo);
}

/* Camera-space depth pre-pass for SSAO: renders ALL opaque geometry depth-only
 * into ssao_depth_fbo at view (view_id_base+1), using the camera view/proj.
 * Reuses the depth-only shadow submit helpers (same CULL_CW as the main pass =
 * correct front-face depth); unlike the shadow pass it does NOT honor
 * shadow_cast_off — SSAO needs depth from every opaque mesh.
 * NOTE: base+1 (not +17): the scene-view base is JCE_VIEW_EDITOR_SCENE=3, and
 * postfx is the ABSOLUTE base JCE_VIEW_POST_BASE=20, so base+17 == 20 collides
 * with postfx.  Only base+1/+2/+3 are free below postfx. */
/* Build the velocity-pass context (programs + uniform idxs + cur/prev VP). */
static JceModelVelocityCtx sr_make_velocity_ctx(JceSceneRenderer *sr)
{
    JceModelVelocityCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.static_program.idx  = sr->prog_gbuffer_vel.idx;
    ctx.skinned_program.idx = sr->prog_gbuffer_vel_skinned.idx;
    ctx.u_prev_model_idx = sr->u_prevModel.idx;
    ctx.u_prev_bones_idx = sr->u_prevBones.idx;
    ctx.u_cur_vp_idx     = sr->u_curViewProj.idx;
    ctx.u_prev_vp_idx    = sr->u_prevViewProj.idx;
    memcpy(ctx.cur_view_proj.raw[0],  sr->cur_view_proj,   sizeof(float) * 16);
    memcpy(ctx.prev_view_proj.raw[0], sr->frame_prev_vp,   sizeof(float) * 16);
    return ctx;
}

/* Single-mesh (procedural primitive / standalone JceMesh) velocity submit:
   set cur transform (u_model[0]) + prev transform (u_prevModel) + cur/prev VP,
   then draw with the static velocity program (MRT normal+velocity). */
static void sr_submit_mesh_velocity(JceSceneRenderer *sr,
                                    const JceModelVelocityCtx *ctx,
                                    uint16_t view_id,
                                    const jce_mat4 *model,
                                    const jce_mat4 *prev_model,
                                    JceMesh *mesh)
{
    if (!mesh || ctx->static_program.idx == UINT16_MAX) return;
    bgfx_uniform_handle_t u_curvp     = { ctx->u_cur_vp_idx };
    bgfx_uniform_handle_t u_prevvp    = { ctx->u_prev_vp_idx };
    bgfx_uniform_handle_t u_prevmodel = { ctx->u_prev_model_idx };
    if (u_curvp.idx  != UINT16_MAX) bgfx_set_uniform(u_curvp,  ctx->cur_view_proj.raw[0],  1);
    if (u_prevvp.idx != UINT16_MAX) bgfx_set_uniform(u_prevvp, ctx->prev_view_proj.raw[0], 1);
    if (u_prevmodel.idx != UINT16_MAX)
        bgfx_set_uniform(u_prevmodel, (prev_model ? prev_model : model)->raw[0], 1);
    bgfx_set_transform(model->raw[0], 1);
    JceShaderHandle prog = { ctx->static_program.idx };
    jce_mesh_submit_pbr_with_program(mesh, sr->renderer, view_id, prog);
}

/* Frustum-cull a static glTF-model entity against the depth/velocity pre-pass
 * camera planes (set once per pass in sr_draw_depth_prepass).  That pass is
 * screen-space (SSAO/SSR normals + TAA velocity), so a model whose world AABB is
 * fully off-screen contributes nothing — skipping it keeps per-frame uniform
 * writes bounded (bgfx Vulkan uniform-scratch safety) and saves draw cost.
 * Skinned entities are dispatched before this is reached in both pre-pass
 * branches, so only static models hit it.  Returns true => cull (skip). */
static bool sr_prepass_model_culled(JceSceneRenderer *sr, JceScene *scene,
                                    JceEntity e, int cull_idx)
{
    /* Reuse the per-frame cull cache: sr->ecull[cull_idx] already holds this
     * static model's world AABB (sr_shadow_caster_aabb resolves the
     * mesh-renderer path identically to the recompute below).  has_aabb==true
     * => same wmin/wmax => identical cull decision, no second model resolve +
     * AABB transform.  Fall back to the recompute only when the cache doesn't
     * cover this index (defensive cull_idx<0/!ecull, or has_aabb==false for an
     * entity ecull couldn't bound). */
    if (cull_idx >= 0 && sr->ecull && sr->ecull[cull_idx].has_aabb)
        return !sr_aabb_in_frustum(sr->prepass_cull_planes,
                                   sr->ecull[cull_idx].wmin,
                                   sr->ecull[cull_idx].wmax);

    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path || !jce_scene_has_transform(scene, e)) return false;
    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;
    float lmn[3], lmx[3];
    if (!jce_model_get_aabb(mc->model, lmn, lmx)) return false;
    jce_mat4 model = (cull_idx >= 0 && sr->ecull &&
                      sr->ecull[cull_idx].world_valid)
                         ? sr->ecull[cull_idx].world
                         : jce_scene_get_world_matrix(scene, e);
    jce_vec3 wmn, wmx;
    sr_transform_aabb(&model, jce_v3(lmn[0], lmn[1], lmn[2]),
                      jce_v3(lmx[0], lmx[1], lmx[2]), &wmn, &wmx);
    return !sr_aabb_in_frustum(sr->prepass_cull_planes, wmn, wmx);
}

/* Skinned model velocity submit (mirrors sr_try_submit_skinned_shadow). Writes
   per-bone motion into the velocity G-buffer using the cur + prev skin palette;
   roughness for the SSR normal slot from the entity's MeshRenderer if any. */
static bool sr_try_submit_skinned_velocity(JceSceneRenderer *sr, JceScene *scene,
                                           JceEntity e, uint16_t view_id,
                                           const JceModelVelocityCtx *ctx)
{
    if (!jce_scene_has_skeletal_animator(scene, e)) return false;
    JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
    if (!sa || !sa->skeleton_path[0]) return false;
    SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
    if (!mc || !mc->model) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);
    SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
    const jce_mat4 *pal = (ai && ai->skin_palette_count > 0) ? ai->skin_palette : NULL;
    uint32_t pal_n = ai ? ai->skin_palette_count : 0;
    const jce_mat4 *prev_pal = (ai && ai->prev_skin_valid &&
                                ai->prev_skin_palette_count > 0)
                                   ? ai->prev_skin_palette : NULL;
    uint32_t prev_n = (ai && ai->prev_skin_valid) ? ai->prev_skin_palette_count : 0;

    /* Roughness for the SSR normal slot (matte default). */
    float rough = 0.8f;
    if (jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr) rough = mr->roughness;
    }
    float gmat[4] = { rough, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(sr->u_gbuffer_mat, gmat, 1);

    /* prev world = same world matrix as current (skinned entities animate via
       the bone palette, and the world TRS is captured for non-skinned dynamics;
       the per-bone delta carries the limb motion). */
    const jce_mat4 *prev_model = sr_prev_xform_find(sr, (uint32_t)e);
    jce_model_draw_velocity(mc->model, sr->renderer, view_id, ctx,
                            &model, pal, pal_n,
                            prev_model, prev_pal, prev_n);
    sr_prev_xform_store(sr, (uint32_t)e, &model);
    return true;
}

/* glTF MODEL (MeshRenderer, non-skinned) velocity submit. */
static bool sr_try_submit_model_velocity(JceSceneRenderer *sr, JceScene *scene,
                                         JceEntity e, int cull_idx,
                                         uint16_t view_id,
                                         const JceModelVelocityCtx *ctx)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path || !jce_scene_has_transform(scene, e)) return false;
    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    /* Screen-space pre-pass: cull off-screen models (uniform-scratch safety). */
    if (sr_prepass_model_culled(sr, scene, e, cull_idx)) return true;
    /* Reuse the per-frame composed world matrix (ecull build) instead of
     * re-walking the parent chain; byte-identical source. */
    jce_mat4 model = (cull_idx >= 0 && sr->ecull &&
                      sr->ecull[cull_idx].world_valid)
                         ? sr->ecull[cull_idx].world
                         : jce_scene_get_world_matrix(scene, e);
    float rough = 0.8f;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    if (mr) rough = mr->roughness;
    float gmat[4] = { rough, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(sr->u_gbuffer_mat, gmat, 1);

    const jce_mat4 *prev_model = sr_prev_xform_find(sr, (uint32_t)e);
    jce_model_draw_velocity(mc->model, sr->renderer, view_id, ctx,
                            &model, NULL, 0, prev_model, NULL, 0);
    sr_prev_xform_store(sr, (uint32_t)e, &model);
    return true;
}

void sr_draw_depth_prepass(JceSceneRenderer *sr, JceScene *scene,
                                  const JceCamera *camera, EntityList *list,
                                  uint16_t view_id_base, uint32_t vp_w, uint32_t vp_h)
{
    if (!sr || !sr->ssao_valid || !camera || !list) return;
    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(sr->renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    /* Lazy-load the SSR normal G-buffer program (vs_gbuffer + fs_gbuffer).
     * When absent, fall back to depth-only (SSR then has matte normals). */
    if (!sr->gbuffer_prog_tried) {
        sr->gbuffer_prog_tried = true;
        JceShaderHandle gh = shader_load_program(sr->pak, "gbuffer");
        sr->prog_gbuffer.idx = gh.idx;
        if (gh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "gbuffer shader not in PAK (SSR loses real normals)");
    }
    const bool have_gbuffer = BGFX_HANDLE_IS_VALID(sr->prog_gbuffer);

    /* TAA velocity mode: lazy-load the MRT velocity programs (normal+velocity).
     * write_velocity is true only when the FBO was created WITH the velocity
     * attachment AND the programs are present. */
    const bool want_vel = sr->ssao_has_velocity && sr->taa_want_velocity;
    if (want_vel && !sr->gbuffer_vel_prog_tried) {
        sr->gbuffer_vel_prog_tried = true;
        JceShaderHandle s  = shader_load_program(sr->pak, "gbuffer_vel");
        JceShaderHandle sk = shader_load_program(sr->pak, "gbuffer_vel_skinned");
        sr->prog_gbuffer_vel.idx         = s.idx;
        sr->prog_gbuffer_vel_skinned.idx = sk.idx;
        if (s.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "gbuffer_vel shader not in PAK (TAA per-object motion off)");
    }
    const bool write_velocity = want_vel &&
                                BGFX_HANDLE_IS_VALID(sr->prog_gbuffer_vel);
    sr->velocity_valid_frame = write_velocity;

    const uint16_t v = (uint16_t)(view_id_base + 1);
    float aspect = (vp_w > 0 && vp_h > 0) ? ((float)vp_w / (float)vp_h) : (16.0f / 9.0f);
    jce_mat4 view = jce_camera_view(camera);
    jce_mat4 proj = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
    /* Frustum planes for pre-pass culling (keeps per-frame uniform writes
     * bounded so bgfx's Vulkan uniform scratch buffer can't overflow). */
    { jce_mat4 _vp = jce_m4_multiply(&proj, &view);
      sr_extract_frustum_planes(&_vp, sr->prepass_cull_planes); }

    bgfx_set_view_rect(v, 0, 0, sr->ssao_w, sr->ssao_h);
    bgfx_set_view_frame_buffer(v, sr->ssao_depth_fbo);
    if (write_velocity) {
        /* MRT clear: ONE bgfx_set_view_clear color cannot give the velocity
         * attachment the exact neutral 0.5 it needs, so use palette clears.
         *   slot 0 (normal) = 0x8080ffff (up normal, roughness 1)
         *   slot 1 (velocity) = (0.5, 0.5, 0, 1) => decoded motion = 0 */
        float vel_clear[4] = { 0.5f, 0.5f, 0.0f, 1.0f };
        bgfx_set_palette_color_rgba8(0, 0x8080ffffu);
        bgfx_set_palette_color(1, vel_clear);
        bgfx_set_view_clear_mrt(v, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                1.0f, 0,
                                0,    /* attachment 0 -> palette 0 */
                                1,    /* attachment 1 -> palette 1 */
                                UINT8_MAX, UINT8_MAX, UINT8_MAX,
                                UINT8_MAX, UINT8_MAX, UINT8_MAX);
    } else {
        /* Clear normal G-buffer to up-ish normal + roughness=1 (matte => no SSR)
         * for pixels not covered by a G-buffer submit; clear depth to far. */
        bgfx_set_view_clear(v, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x8080ffff, 1.0f, 0);
    }
    bgfx_set_view_transform(v, view.raw[0], proj.raw[0]);
    bgfx_touch(v);

    /* Velocity context: un-jittered cur/prev view*proj + programs/uniform idxs. */
    JceModelVelocityCtx vctx;
    if (write_velocity) vctx = sr_make_velocity_ctx(sr);

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;

        if (write_velocity) {
            /* Skinned + model: write per-object/per-bone velocity (+ real
             * normals for SSR).  Terrain: depth + matte (rarely moves). */
            if (sr_try_submit_skinned_velocity(sr, scene, e, v, &vctx)) continue;
            if (sr_try_submit_model_velocity(sr, scene, e, i, v, &vctx))   continue;
            if (sr_try_submit_terrain_shadow(sr, scene, e, v))          continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
            if (!mesh) continue;
            float rough = 0.8f;
            if (jce_scene_has_mesh_renderer(scene, e)) {
                JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
                if (mr) rough = mr->roughness;
            }
            float gmat[4] = { rough, 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(sr->u_gbuffer_mat, gmat, 1);
            const jce_mat4 *prev_model = sr_prev_xform_find(sr, (uint32_t)e);
            sr_submit_mesh_velocity(sr, &vctx, v, &model, prev_model, mesh);
            sr_prev_xform_store(sr, (uint32_t)e, &model);
            continue;
        }

        /* SSAO/SSR-only path (no velocity): cull off-screen models so this pass
         * scales like the velocity path — the shadow-shared submit helpers must
         * not cull (shadows need off-screen casters), so the cull lives here at
         * the loop level, after the skinned dispatch (only static models reach it).
         * Uses the per-frame cull cache: sr->ecull[i] holds the SAME model world
         * AABB sr_prepass_model_culled would recompute (skinned entities were
         * dispatched on the line above, so only static mesh-renderer models reach
         * here — for those sr_shadow_caster_aabb resolves the mesh-renderer path
         * identically; has_aabb==false => no resolvable model => don't cull, exactly
         * as sr_prepass_model_culled returned false for a path-less entity). */
        if (sr_try_submit_skinned_shadow(sr, scene, e, i, v)) continue;
        if (sr->ecull[i].has_aabb &&
            !sr_aabb_in_frustum(sr->prepass_cull_planes,
                                sr->ecull[i].wmin, sr->ecull[i].wmax)) continue;
        if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, i, v)) continue;
        if (sr_try_submit_terrain_shadow(sr, scene, e, v)) continue;
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, i, &model, &mesh)) continue;
        if (!mesh) continue;
        bgfx_set_transform(model.raw[0], 1);
        if (have_gbuffer) {
            /* Write world normal + material roughness into the G-buffer. */
            float rough = 0.8f;
            if (jce_scene_has_mesh_renderer(scene, e)) {
                JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
                if (mr) rough = mr->roughness;
            }
            float gmat[4] = { rough, 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(sr->u_gbuffer_mat, gmat, 1);
            JceShaderHandle gprog = { sr->prog_gbuffer.idx };
            jce_mesh_submit_pbr_with_program(mesh, sr->renderer, v, gprog);
        } else {
            jce_mesh_submit_shadow(mesh, sr->renderer, v);
        }
    }

    if (write_velocity)
        sr_prev_xform_prune(sr);
}


/* ── Distance/importance light selection (Unity-style culling) ──────────────
 * Pick the most important point/spot lights for the view: importance =
 * intensity / distance^2 to the camera (nearer + brighter wins). When a scene
 * has more lights than the cap, the farthest/dimmest are dropped instead of an
 * arbitrary ECS-order suffix. Stores the chosen ENTITY sets on sr; the gather
 * and the shadow producer both skip non-selected lights (same filter, list
 * order) so the shader light index stays aligned with the shadow slot. */
typedef struct { JceEntity e; float score; } SrLightCand;

bool sr_light_selected(const JceEntity *arr, uint32_t n, JceEntity e)
{
    for (uint32_t i = 0; i < n; i++)
        if (arr[i] == e) return true;
    return false;
}

void sr_select_lights(JceSceneRenderer *sr, JceScene *scene,
                             EntityList *list, const JceCamera *camera)
{
    sr->frame_sel_point_n = 0;
    sr->frame_sel_spot_n  = 0;
    if (!scene || !list) return;

    jce_vec3 cam = camera ? jce_camera_get_position(camera) : jce_v3(0, 0, 0);

    enum { SR_LIGHT_CAND_MAX = 256 };
    SrLightCand pc[SR_LIGHT_CAND_MAX]; uint32_t pn = 0;
    SrLightCand sc[SR_LIGHT_CAND_MAX]; uint32_t sn = 0;

    for (int li = 0; li < list->count; li++) {
        JceEntity e = list->entities[li];
        if (!entity_enabled(scene, e)) continue;
        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 p  = xf ? xf->position : jce_v3(0, 0, 0);
        jce_vec3 d  = jce_v3_sub(p, cam);
        float dist2 = jce_v3_dot(d, d);
        if (dist2 < 0.01f) dist2 = 0.01f;

        if (pn < SR_LIGHT_CAND_MAX
            && jce_scene_has_point_light(scene, e)
            && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_POINT_LIGHT)) {
            JcePointLight *pl = jce_scene_get_point_light(scene, e);
            if (pl) {
                float intensity = pl->intensity > 0.0f ? pl->intensity : 1.0f;
                pc[pn].e = e; pc[pn].score = intensity / dist2; pn++;
            }
        }
        if (sn < SR_LIGHT_CAND_MAX
            && jce_scene_has_spot_light(scene, e)
            && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPOT_LIGHT)) {
            JceSpotLight *sl = jce_scene_get_spot_light(scene, e);
            if (sl) {
                float intensity = sl->intensity > 0.0f ? sl->intensity : 1.0f;
                sc[sn].e = e; sc[sn].score = intensity / dist2; sn++;
            }
        }
    }

    /* Partial selection sort: pull the top-N by score to the front and store
       their entities as the membership set (order inside the set is irrelevant —
       the gather/producer re-process them in list order). */
    uint32_t pkeep = pn < (uint32_t)JCE_MAX_POINT_LIGHTS
                     ? pn : (uint32_t)JCE_MAX_POINT_LIGHTS;
    for (uint32_t i = 0; i < pkeep; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < pn; j++)
            if (pc[j].score > pc[best].score) best = j;
        SrLightCand t = pc[i]; pc[i] = pc[best]; pc[best] = t;
        sr->frame_sel_point[i] = pc[i].e;
    }
    sr->frame_sel_point_n = pkeep;

    uint32_t skeep = sn < (uint32_t)JCE_MAX_SPOT_LIGHTS
                     ? sn : (uint32_t)JCE_MAX_SPOT_LIGHTS;
    for (uint32_t i = 0; i < skeep; i++) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < sn; j++)
            if (sc[j].score > sc[best].score) best = j;
        SrLightCand t = sc[i]; sc[i] = sc[best]; sc[best] = t;
        sr->frame_sel_spot[i] = sc[i].e;
    }
    sr->frame_sel_spot_n = skeep;
}

/* ── Frustum culling (uniform-grid broadphase) ────────────────────── */

/* Extract 6 frustum planes from a column-major view*proj matrix.
 * Convention: plane.xyz = normal, plane.w = signed distance such that
 *             dot(plane.xyz, p) + plane.w >= 0  iff  p is INSIDE the frustum.
 * Works for D3D-style NDC ([0,1] depth) and OpenGL-style ([-1,1]) alike for
 * left/right/top/bottom; near plane uses (m3 + m2) which is correct for
 * GL and conservative (looser) for D3D — fine for broadphase culling. */
void sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6])
{
    jce_frustum_extract_planes(m, planes);   /* shared jce_frustum.h */
}

/* Pass-A scratch: per-entity world matrix + local AABB resolved on the
 * main thread, consumed by the parallel corner-transform pass.  `mode`
 * 0 = keep (no transform/model — always visible, skip transform),
 * 1 = transform the local AABB by `model`,
 * 2 = world AABB already resolved (reused from the per-frame ecull cache):
 *     `wmn`/`wmx` hold the world bounds, no corner transform needed. */
struct SrCullPrep {
    jce_mat4 model;
    float    lmn[3];
    float    lmx[3];
    jce_vec3 wmn, wmx;
    uint8_t  mode;
};

/* Pass-B worker context (read-only inputs + disjoint per-index outputs). */
typedef struct {
    const struct SrCullPrep *prep;
    JceAABB                 *aabbs;
    bool                    *visible;
} SrCullXformCtx;

/* Transform entities [begin,end) local AABBs into world AABBs.  Writes
 * only aabbs[i]/visible[i] for i in range → safe to run in parallel. */
static void sr_cull_xform_range(int begin, int end, void *user)
{
    SrCullXformCtx *c = (SrCullXformCtx *)user;
    for (int i = begin; i < end; i++) {
        if (c->prep[i].mode == 0) {
            c->aabbs[i].min = c->aabbs[i].max = jce_v3(0, 0, 0);
            c->visible[i] = true;   /* no transform/model → always kept */
            continue;
        }
        if (c->prep[i].mode == 2) {
            /* World AABB reused from the per-frame ecull cache — no corner
             * transform needed (identical result, just resolved once). */
            c->aabbs[i].min = c->prep[i].wmn;
            c->aabbs[i].max = c->prep[i].wmx;
            c->visible[i]   = false;   /* flipped to true by the frustum query */
            continue;
        }
        float lmn[3], lmx[3];
        lmn[0] = c->prep[i].lmn[0]; lmn[1] = c->prep[i].lmn[1]; lmn[2] = c->prep[i].lmn[2];
        lmx[0] = c->prep[i].lmx[0]; lmx[1] = c->prep[i].lmx[1]; lmx[2] = c->prep[i].lmx[2];

        const jce_vec3 corners[8] = {
            { lmn[0], lmn[1], lmn[2] }, { lmx[0], lmn[1], lmn[2] },
            { lmn[0], lmx[1], lmn[2] }, { lmx[0], lmx[1], lmn[2] },
            { lmn[0], lmn[1], lmx[2] }, { lmx[0], lmn[1], lmx[2] },
            { lmn[0], lmx[1], lmx[2] }, { lmx[0], lmx[1], lmx[2] },
        };
        jce_vec3 bmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
        jce_vec3 bmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int k = 0; k < 8; k++) {
            const jce_vec4 cv = { corners[k].x, corners[k].y, corners[k].z, 1.0f };
            const jce_vec4 wv = jce_m4_mul_v4(&c->prep[i].model, cv);
            const float wx = wv.x, wy = wv.y, wz = wv.z;
            if (wx < bmn.x) bmn.x = wx; if (wx > bmx.x) bmx.x = wx;
            if (wy < bmn.y) bmn.y = wy; if (wy > bmx.y) bmx.y = wy;
            if (wz < bmn.z) bmn.z = wz; if (wz > bmx.z) bmx.z = wz;
        }
        c->aabbs[i].min = bmn;
        c->aabbs[i].max = bmx;
        c->visible[i]   = false;   /* flipped to true by the frustum query */
    }
}

/* ── Persistent cull broad-phase: per-entity space-handle map ─────────────
 * The cull grid is kept across frames (insert once, update only moved, remove
 * despawned).  This open-addressed table maps a STABLE entity id to its space
 * handle + the AABB it was last bucketed with, so the per-frame maintenance is
 * proportional to entities that actually moved/appeared/vanished — a static
 * city pays ~0/frame.  Mirrors the wcache hashing. */
static uint32_t sr_cull_map_hash(uint32_t e) { return e * 2654435761u; }

static struct SrCullSpaceEntry *sr_cull_map_find(JceSceneRenderer *sr,
                                                 uint32_t entity)
{
    if (!sr->cull_map || sr->cull_map_cap == 0 || entity == 0) return NULL;
    uint32_t mask = sr->cull_map_cap - 1;
    uint32_t h = sr_cull_map_hash(entity) & mask;
    for (uint32_t i = 0; i < sr->cull_map_cap; i++) {
        uint32_t s = (h + i) & mask;
        struct SrCullSpaceEntry *en = &sr->cull_map[s];
        if (!en->used) return NULL;                  /* open slot ends probe */
        if (en->entity == entity) return en;
    }
    return NULL;
}

static bool sr_cull_map_grow(JceSceneRenderer *sr, uint32_t need)
{
    uint32_t new_cap = sr->cull_map_cap ? sr->cull_map_cap : 1024;
    while (new_cap < need * 2u) new_cap *= 2u;       /* load factor < 0.5 */
    struct SrCullSpaceEntry *ns = (struct SrCullSpaceEntry *)
        JCE_CALLOC(new_cap, sizeof(struct SrCullSpaceEntry));
    if (!ns) return false;
    if (sr->cull_map) {
        uint32_t nmask = new_cap - 1;
        for (uint32_t i = 0; i < sr->cull_map_cap; i++) {
            struct SrCullSpaceEntry *o = &sr->cull_map[i];
            if (!o->used) continue;
            uint32_t h = sr_cull_map_hash(o->entity) & nmask;
            while (ns[h].used) h = (h + 1) & nmask;
            ns[h] = *o;
        }
        JCE_FREE(sr->cull_map);
    }
    sr->cull_map = ns;
    sr->cull_map_cap = new_cap;
    return true;
}

static struct SrCullSpaceEntry *sr_cull_map_insert_slot(JceSceneRenderer *sr,
                                                        uint32_t entity)
{
    if (sr->cull_map_cap == 0 || (sr->cull_map_count + 1) * 2u >= sr->cull_map_cap) {
        if (!sr_cull_map_grow(sr, sr->cull_map_count + 1)) return NULL;
    }
    uint32_t mask = sr->cull_map_cap - 1;
    uint32_t h = sr_cull_map_hash(entity) & mask;
    while (sr->cull_map[h].used && sr->cull_map[h].entity != entity)
        h = (h + 1) & mask;
    struct SrCullSpaceEntry *en = &sr->cull_map[h];
    if (!en->used) { en->used = true; en->entity = entity; sr->cull_map_count++; }
    return en;
}

/* Drop EVERY persistent handle + the map (used when the grid must be rebuilt
 * because its world bounds/resolution changed).  The grid itself is reset by
 * the caller; here we just forget the handle bookkeeping so everything is
 * reinserted fresh. */
static void sr_cull_map_clear(JceSceneRenderer *sr)
{
    if (sr->cull_map)
        memset(sr->cull_map, 0,
               sr->cull_map_cap * sizeof(struct SrCullSpaceEntry));
    sr->cull_map_count = 0;
}

/* Build a transient grid from the entity list and frustum-cull it.
 * Output: visible[i] = true if entity list->entities[i] passes culling.
 * Returns the visible entity count.
 *
 * Three phases: (A) resolve each entity's world matrix + local AABB on
 * the main thread — this touches the shared model/mesh/world-matrix
 * caches and so cannot run concurrently; (B) transform the corners into
 * world AABBs, fanned out across the shared job system (pure math,
 * disjoint per-index writes); (C) maintain the PERSISTENT broad-phase
 * (insert/update/remove only what changed) + run the grid query serially. */
uint32_t sr_compute_visible(JceSceneRenderer *sr,
                                    JceScene *scene,
                                    const EntityList *list,
                                    const jce_vec4 planes[6],
                                    bool *visible)
{
    /* Grow persistent AABB + prep arrays if needed. */
    if (sr->cull_aabb_cap < (uint32_t)list->count) {
        uint32_t new_cap = sr->cull_aabb_cap ? sr->cull_aabb_cap * 2u : 64u;
        while (new_cap < (uint32_t)list->count) new_cap *= 2u;
        JceAABB *grown = (JceAABB *)JCE_REALLOC(sr->cull_aabbs,
                                                 new_cap * sizeof(JceAABB));
        struct SrCullPrep *gp = (struct SrCullPrep *)JCE_REALLOC(
            sr->cull_prep, new_cap * sizeof(struct SrCullPrep));
        if (!grown || !gp) {
            if (grown) sr->cull_aabbs = grown;
            if (gp)    sr->cull_prep  = gp;
            for (int i = 0; i < list->count; i++) visible[i] = true;
            return (uint32_t)list->count;
        }
        sr->cull_aabbs    = grown;
        sr->cull_prep     = gp;
        sr->cull_aabb_cap = new_cap;
    }
    JceAABB           *aabbs = sr->cull_aabbs;
    struct SrCullPrep *prep  = sr->cull_prep;

    /* Pass A (serial, main thread): resolve world matrix + local AABB.
     * sr_build_entity_model / sr_resolve_mesh touch shared caches, so this
     * must NOT be parallelised. */
    for (int i = 0; i < list->count; i++) {
        /* Reuse the per-frame ecull cache for entities it covers (resolvable
         * glTF models — skeletal-animator skeleton or mesh-renderer model).
         * sr_shadow_caster_aabb computed the SAME world AABB the model-resolve
         * branch below would recompute, so reading it here avoids a second
         * model resolve + 8-corner transform with an identical cull decision.
         * Entities ecull doesn't bound (has_aabb==false: primitives, .obj
         * meshes, transform-less specials) fall through to the recompute. */
        if (sr->ecull && sr->ecull[i].has_aabb) {
            prep[i].wmn  = sr->ecull[i].wmin;
            prep[i].wmx  = sr->ecull[i].wmax;
            prep[i].mode = 2;
            continue;
        }

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, list->entities[i], i, &model, &mesh)) {
            prep[i].mode = 0;   /* keep; transform pass skips it */
            continue;
        }

        float lmn[3], lmx[3];
        bool have_bounds = false;
        if (mesh) {
            jce_mesh_get_aabb(mesh, lmn, lmx);
            have_bounds = true;
        } else {
            /* glTF mesh-renderer / skeletal-animator entities have no JceMesh —
             * resolve their JceModel and use its real local AABB so a large or
             * un-scaled model is not culled the instant its origin leaves the
             * frustum (the centre-point-cull bug).  Falls back to a unit box. */
            JceEntity e = list->entities[i];
            const char *mpath = sr_mesh_renderer_model_path(scene, e);
            if (!mpath && jce_scene_has_skeletal_animator(scene, e)) {
                JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
                if (sa && sa->skeleton_path[0]) mpath = sa->skeleton_path;
            }
            if (mpath) {
                SrModelCache *mc2 = sr_get_model(sr, mpath, (uint32_t)e);
                if (mc2 && mc2->model && jce_model_get_aabb(mc2->model, lmn, lmx))
                    have_bounds = true;
            }
            if (!have_bounds) {
                lmn[0] = lmn[1] = lmn[2] = -0.5f;
                lmx[0] = lmx[1] = lmx[2] =  0.5f;
            }
        }
        /* Degenerate AABB safeguard. */
        if (lmx[0] - lmn[0] < 1e-4f && lmx[1] - lmn[1] < 1e-4f &&
            lmx[2] - lmn[2] < 1e-4f) {
            lmn[0] = lmn[1] = lmn[2] = -0.5f;
            lmx[0] = lmx[1] = lmx[2] =  0.5f;
        }
        prep[i].model = model;
        prep[i].lmn[0] = lmn[0]; prep[i].lmn[1] = lmn[1]; prep[i].lmn[2] = lmn[2];
        prep[i].lmx[0] = lmx[0]; prep[i].lmx[1] = lmx[1]; prep[i].lmx[2] = lmx[2];
        prep[i].mode = 1;
    }

    /* Pass B: transform corners → world AABBs.  Fan out when the entity
     * count justifies the dispatch overhead; otherwise run inline. */
    SrCullXformCtx xc = { prep, aabbs, visible };
    JceJobSystem *jobs = jce_jobs_default();
    if (jobs && list->count >= 256)
        jce_jobs_parallel_for(jobs, list->count, 0, sr_cull_xform_range, &xc);
    else
        sr_cull_xform_range(0, list->count, &xc);

    /* Pass C (serial): reduce world bounds over the computed AABBs. */
    jce_vec3 wmin = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int i = 0; i < list->count; i++) {
        if (prep[i].mode == 0) continue;   /* kept entity, no bounds */
        const jce_vec3 bmn = aabbs[i].min, bmx = aabbs[i].max;
        if (bmn.x < wmin.x) wmin.x = bmn.x;
        if (bmn.y < wmin.y) wmin.y = bmn.y;
        if (bmn.z < wmin.z) wmin.z = bmn.z;
        if (bmx.x > wmax.x) wmax.x = bmx.x;
        if (bmx.y > wmax.y) wmax.y = bmx.y;
        if (bmx.z > wmax.z) wmax.z = bmx.z;
    }

    /* Pad to avoid degenerate dimensions. */
    const float pad = 1.0f;
    wmin.x -= pad; wmin.y -= pad; wmin.z -= pad;
    wmax.x += pad; wmax.y += pad; wmax.z += pad;

    /* Publish the scene bounds for next frame's directional-shadow cascade
     * near-plane extension (only when at least one entity contributed; FLT_MAX
     * sentinels mean an empty list — leave the previous valid bounds). */
    if (wmax.x >= wmin.x && wmin.x < 1e30f && wmax.x > -1e30f) {
        sr->scene_world_min = wmin;
        sr->scene_world_max = wmax;
        sr->scene_world_valid = true;
    }

    JceAABB world = { wmin, wmax };

    /* ── Correctness self-check (JCE_CULL_VERIFY=1) ────────────────────────
     * Ground-truth the visible set with a brute-force per-entity frustum test
     * (exactly what the grid is accelerating), so any divergence — a grid bug, a
     * stale persistent handle, a wrong remap — is caught + logged the frame it
     * happens.  Off by default (zero overhead).  ref_vis[i] holds the truth;
     * compared against `visible[]` at the end of the function. */
    static int s_cull_verify = -1;
    if (s_cull_verify < 0) {
        const char *vv = getenv("JCE_CULL_VERIFY");
        s_cull_verify = (vv && vv[0] && vv[0] != '0') ? 1 : 0;
    }
    bool *ref_vis = NULL;
    if (s_cull_verify) {
        ref_vis = (bool *)JCE_MALLOC((size_t)list->count * sizeof(bool));
        if (ref_vis) {
            for (int i = 0; i < list->count; i++) {
                if (prep[i].mode == 0) { ref_vis[i] = true; continue; } /* always kept */
                ref_vis[i] = sr_aabb_in_frustum(planes, aabbs[i].min, aabbs[i].max);
            }
        }
    }

    /* ── Persistent broad-phase maintenance ───────────────────────────────
     * Keep the grid + its objects across frames.  We must (re)build it only
     * when its world bounds no longer contain this frame's union (so the cell
     * mapping covers every entity).  When building we inflate the bounds by a
     * generous margin so ordinary streaming wobble (an entity sliding in/out at
     * the edge) doesn't trigger a rebuild every frame.  In steady state the grid
     * is built once and only moved/new/despawned entities touch it. */
    sr->stat_cull_updated = sr->stat_cull_inserted = sr->stat_cull_removed = 0;

    /* Optional broad-phase KPI (JCE_CULL_KPI=1): time ONLY the grid
     * maintenance + frustum query (the part this change targets) and log it
     * every 120 calls with the churn counts.  Off by default. */
    static int s_cull_kpi = -1;
    if (s_cull_kpi < 0) {
        const char *kv = getenv("JCE_CULL_KPI");
        s_cull_kpi = (kv && kv[0] && kv[0] != '0') ? 1 : 0;
    }
    const uint64_t kpi_t0 = s_cull_kpi ? jce_time_perf_counter() : 0;

    /* A/B + safety hatch: JCE_DISABLE_PERSIST_CULL=1 reverts to the legacy
     * reset+reinsert-ALL-every-frame path (still benefits from extent sizing +
     * occupied-cell iteration, just not the persistent insert/update/remove). */
    static int s_persist_disabled = -1;
    if (s_persist_disabled < 0) {
        const char *dv = getenv("JCE_DISABLE_PERSIST_CULL");
        s_persist_disabled = (dv && dv[0] && dv[0] != '0') ? 1 : 0;
    }
    if (s_persist_disabled) {
        if (!sr->cull_space) {
            JceSpaceConfig cfg = { 0 };
            cfg.type         = JCE_SPACE_GRID;
            cfg.world_bounds = world;
            cfg.max_objects  = (uint32_t)list->count;
            sr->cull_space = jce_space_create(&cfg);
        } else {
            jce_space_reset(sr->cull_space, &world);
        }
        sr->cull_space_built = false;   /* persistent path must rebuild on re-enable */
        sr_cull_map_clear(sr);
        if (!sr->cull_space) {
            for (int i = 0; i < list->count; i++) visible[i] = true;
            return (uint32_t)list->count;
        }
        for (int i = 0; i < list->count; i++) {
            if (visible[i]) continue;
            jce_space_insert(sr->cull_space, aabbs[i], (uint32_t)i);
        }
        if (sr->cull_hit_cap < (uint32_t)list->count) {
            uint32_t nc = sr->cull_hit_cap ? sr->cull_hit_cap : 256u;
            while (nc < (uint32_t)list->count) nc *= 2u;
            uint32_t *g = (uint32_t *)JCE_REALLOC(sr->cull_hit_buf,
                                                  nc * sizeof(uint32_t));
            if (g) { sr->cull_hit_buf = g; sr->cull_hit_cap = nc; }
        }
        if (!sr->cull_hit_buf) {
            for (int i = 0; i < list->count; i++) visible[i] = true;
            return (uint32_t)list->count;
        }
        const uint32_t lh = jce_space_query_frustum(sr->cull_space, planes,
                                                    sr->cull_hit_buf,
                                                    sr->cull_hit_cap);
        for (uint32_t k = 0; k < lh; k++)
            if (sr->cull_hit_buf[k] < (uint32_t)list->count)
                visible[sr->cull_hit_buf[k]] = true;
        uint32_t lv = 0;
        for (int i = 0; i < list->count; i++) if (visible[i]) lv++;
        if (s_cull_kpi) {
            static uint32_t s_kc = 0;
            if ((s_kc++ % 120u) == 0)
                LOG_INFO(LOG_TAG, "CULL_KPI[legacy] %.3f ms (rebuild-all): %d ent, %u vis, %u occ cells",
                         jce_time_perf_to_ms(kpi_t0, jce_time_perf_counter()),
                         list->count, lv,
                         sr->cull_space ? jce_space_occupied_cell_count(sr->cull_space) : 0);
        }
        return lv;
    }

    bool need_build = !sr->cull_space || !sr->cull_space_built;
    if (sr->cull_space && sr->cull_space_built) {
        const JceAABB *bw = &sr->cull_space_world;
        if (wmin.x < bw->min.x || wmin.y < bw->min.y || wmin.z < bw->min.z ||
            wmax.x > bw->max.x || wmax.y > bw->max.y || wmax.z > bw->max.z)
            need_build = true;
    }

    if (need_build) {
        /* Inflate the union by 25% per axis (min 64 units) so a streamed world
         * that grows a little doesn't rebuild the grid every frame. */
        JceAABB gw = world;
        float mx_ = (gw.max.x - gw.min.x) * 0.25f; if (mx_ < 64.0f) mx_ = 64.0f;
        float my_ = (gw.max.y - gw.min.y) * 0.25f; if (my_ < 64.0f) my_ = 64.0f;
        float mz_ = (gw.max.z - gw.min.z) * 0.25f; if (mz_ < 64.0f) mz_ = 64.0f;
        gw.min.x -= mx_; gw.min.y -= my_; gw.min.z -= mz_;
        gw.max.x += mx_; gw.max.y += my_; gw.max.z += mz_;

        if (!sr->cull_space) {
            JceSpaceConfig cfg = { 0 };
            cfg.type         = JCE_SPACE_GRID;
            cfg.world_bounds = gw;
            cfg.max_objects  = (uint32_t)list->count;
            sr->cull_space = jce_space_create(&cfg);
        } else {
            jce_space_reset(sr->cull_space, &gw);  /* drops all objects */
        }
        sr_cull_map_clear(sr);                      /* forget all handles */
        sr->cull_space_world = gw;
        sr->cull_space_built = (sr->cull_space != NULL);
    }

    if (!sr->cull_space) {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        return (uint32_t)list->count;
    }

    /* New cull generation: entities seen this call are stamped with it; any map
     * entry NOT stamped is despawned (or off-list) and removed below. */
    const uint32_t gen = ++sr->cull_gen;

    for (int i = 0; i < list->count; i++) {
        if (visible[i]) continue;     /* transform-less; already kept, not gridded */
        uint32_t e = (uint32_t)list->entities[i];
        struct SrCullSpaceEntry *en = sr_cull_map_find(sr, e);
        if (!en) {
            en = sr_cull_map_insert_slot(sr, e);
            if (!en) { /* OOM: fall back to keeping it visible (never wrongly cull) */
                visible[i] = true;
                continue;
            }
            en->handle = jce_space_insert(sr->cull_space, aabbs[i], (uint32_t)i);
            en->last_min = aabbs[i].min;
            en->last_max = aabbs[i].max;
            sr->stat_cull_inserted++;
        } else {
            /* Re-bucket only when the AABB actually changed (moved/animated);
             * the cell-range guard inside jce_space_update makes a sub-cell move
             * free too, but skipping the call entirely keeps static entities at
             * literally zero work. */
            const jce_vec3 nmn = aabbs[i].min, nmx = aabbs[i].max;
            if (nmn.x != en->last_min.x || nmn.y != en->last_min.y ||
                nmn.z != en->last_min.z || nmx.x != en->last_max.x ||
                nmx.y != en->last_max.y || nmx.z != en->last_max.z) {
                jce_space_update(sr->cull_space, en->handle, aabbs[i]);
                en->last_min = nmn;
                en->last_max = nmx;
                sr->stat_cull_updated++;
            }
            /* Remap the stable handle to THIS frame's render-list index for the
             * query (cheap; no cell touch). */
            jce_space_set_user_id(sr->cull_space, en->handle, (uint32_t)i);
        }
        en->seen_gen = gen;
    }

    /* Sweep: remove entities that were in the map but are absent this frame
     * (despawned / unloaded / now off-list).  Two passes: (1) remove the dead
     * from the broad-phase + mark their slots empty; (2) if ANY died, rehash the
     * survivors into a clean table so the open-addressed probe chains have no
     * holes (no tombstones, no stale-handle UAF, no fragile backward-shift). */
    if (sr->cull_map && sr->cull_map_count > 0) {
        uint32_t dead = 0;
        for (uint32_t s = 0; s < sr->cull_map_cap; s++) {
            struct SrCullSpaceEntry *en = &sr->cull_map[s];
            if (!en->used || en->seen_gen == gen) continue;
            jce_space_remove(sr->cull_space, en->handle);
            *en = (struct SrCullSpaceEntry){0};   /* mark empty */
            dead++;
        }
        if (dead) {
            sr->stat_cull_removed = dead;
            sr->cull_map_count   -= dead;
            /* Compact-rehash survivors into a fresh same-capacity table. */
            struct SrCullSpaceEntry *clean = (struct SrCullSpaceEntry *)
                JCE_CALLOC(sr->cull_map_cap, sizeof(struct SrCullSpaceEntry));
            if (clean) {
                uint32_t mask = sr->cull_map_cap - 1;
                for (uint32_t s = 0; s < sr->cull_map_cap; s++) {
                    struct SrCullSpaceEntry *o = &sr->cull_map[s];
                    if (!o->used) continue;
                    uint32_t h = sr_cull_map_hash(o->entity) & mask;
                    while (clean[h].used) h = (h + 1) & mask;
                    clean[h] = *o;
                }
                JCE_FREE(sr->cull_map);
                sr->cull_map = clean;
            }
            /* On OOM keep the holey table; sr_cull_map_find stops at the first
             * empty slot, so a hole can cause a MISS (then re-insert as new = a
             * duplicate broad-phase object).  To stay correct under that rare
             * path, force a full rebuild next frame. */
            else {
                sr->cull_space_built = false;
            }
        }
    }

    /* Frustum query: heap hit buffer, grown to cover the worst case (every
     * entity visible).  Was a 128 KB stack array. */
    if (sr->cull_hit_cap < (uint32_t)list->count) {
        uint32_t nc = sr->cull_hit_cap ? sr->cull_hit_cap : 256u;
        while (nc < (uint32_t)list->count) nc *= 2u;
        uint32_t *g = (uint32_t *)JCE_REALLOC(sr->cull_hit_buf,
                                              nc * sizeof(uint32_t));
        if (g) { sr->cull_hit_buf = g; sr->cull_hit_cap = nc; }
    }
    if (!sr->cull_hit_buf || sr->cull_hit_cap == 0) {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        return (uint32_t)list->count;
    }

    const uint32_t hits = jce_space_query_frustum(sr->cull_space, planes,
                                                   sr->cull_hit_buf,
                                                   sr->cull_hit_cap);
    for (uint32_t k = 0; k < hits; k++) {
        if (sr->cull_hit_buf[k] < (uint32_t)list->count)
            visible[sr->cull_hit_buf[k]] = true;
    }

    /* Visible count: hits from the grid + the always-kept transform-less set
     * (mode 0 entities never entered the grid).  Recount `visible` to report the
     * true total (matches the pre-change return, which counted only grid hits;
     * but the caller derives culled = total - visible, so an accurate visible
     * count keeps the stat correct now that transform-less entities aren't
     * gridded). */
    uint32_t vis = 0;
    for (int i = 0; i < list->count; i++) if (visible[i]) vis++;

    if (s_cull_kpi) {
        static uint32_t s_kc = 0;
        if ((s_kc++ % 120u) == 0)
            LOG_INFO(LOG_TAG, "CULL_KPI[persist] %.3f ms: %d ent, %u vis, "
                     "ins=%u upd=%u rem=%u, %u/%u occ cells, %u objs",
                     jce_time_perf_to_ms(kpi_t0, jce_time_perf_counter()),
                     list->count, vis, sr->stat_cull_inserted,
                     sr->stat_cull_updated, sr->stat_cull_removed,
                     jce_space_occupied_cell_count(sr->cull_space),
                     jce_space_cell_count(sr->cull_space),
                     jce_space_object_count(sr->cull_space));
    }

    /* Self-check: the persistent-grid visible set must EXACTLY equal the
     * brute-force frustum truth.  Logs (loudly) the first few divergences. */
    if (ref_vis) {
        uint32_t mism = 0;
        for (int i = 0; i < list->count; i++) {
            if (visible[i] != ref_vis[i]) {
                if (mism < 8)
                    LOG_ERROR(LOG_TAG,
                        "CULL_VERIFY mismatch entity[%d] id=%u grid=%d truth=%d "
                        "(aabb [%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f])",
                        i, (uint32_t)list->entities[i], (int)visible[i],
                        (int)ref_vis[i], aabbs[i].min.x, aabbs[i].min.y,
                        aabbs[i].min.z, aabbs[i].max.x, aabbs[i].max.y,
                        aabbs[i].max.z);
                mism++;
            }
        }
        if (mism == 0)
            LOG_INFO(LOG_TAG, "CULL_VERIFY ok: %d entities, %u visible (grid==truth)",
                     list->count, vis);
        else
            LOG_ERROR(LOG_TAG, "CULL_VERIFY FAILED: %u/%d entities diverged",
                      mism, list->count);
        JCE_FREE(ref_vis);
    }
    return vis;
}

/* ── Baked GI consumption (P1-baked-gi-consume) ───────────────────────
 *
 * Reflection probe cubemaps are loaded once and cached by path; the SH9
 * coefficients live directly in the LightProbeGroup component. Each frame
 * sr_gather_baked_gi() picks the dominant probe / group nearest the camera
 * and the bind callbacks consume them. */

/* Find (or lazily load) a baked reflection-probe cubemap by path. Returns
 * the cache slot index, or -1 on failure. The cache is small and probes
 * are few, so a linear scan is fine. */
static int sr_rprobe_cache_get(JceSceneRenderer *sr, const char *path)
{
    if (!path || !path[0]) return -1;

    for (int i = 0; i < sr->rprobe_cache_count; i++) {
        if (strncmp(sr->rprobe_cache[i].path, path,
                    sizeof(sr->rprobe_cache[i].path)) == 0) {
            return sr->rprobe_cache[i].failed ? -1 : i;
        }
    }
    if (sr->rprobe_cache_count >= (int)(sizeof(sr->rprobe_cache) /
                                        sizeof(sr->rprobe_cache[0])))
        return -1;

    int slot = sr->rprobe_cache_count++;
    snprintf(sr->rprobe_cache[slot].path, sizeof(sr->rprobe_cache[slot].path),
             "%s", path);
    sr->rprobe_cache[slot].spec.idx = UINT16_MAX;
    sr->rprobe_cache[slot].irr.idx  = UINT16_MAX;
    sr->rprobe_cache[slot].used     = true;
    sr->rprobe_cache[slot].failed   = false;

    uint16_t spec = jce__ktx_load_cubemap(path);
    if (spec == UINT16_MAX) {
        sr->rprobe_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "reflection probe cubemap load failed: %s", path);
        return -1;
    }
    sr->rprobe_cache[slot].spec.idx = spec;
    /* The bake currently emits single-mip KTX cubemaps; a future specular
     * mip-chain bake should plumb the real count here so glossy reflections
     * pick the correct prefilter LOD. */
    sr->rprobe_cache[slot].spec_mips = 1;

    /* Irradiance sidecar: <stem>.irr.ktx (optional — fall back to the
     * specular cube for diffuse when absent). */
    char irr_path[256];
    snprintf(irr_path, sizeof(irr_path), "%s", path);
    char *dot = strrchr(irr_path, '.');
    if (dot && (size_t)(dot - irr_path) + 9u < sizeof(irr_path)) {
        memcpy(dot, ".irr.ktx", 9u); /* includes NUL */
        uint16_t irr = jce__ktx_load_cubemap(irr_path);
        if (irr != UINT16_MAX) sr->rprobe_cache[slot].irr.idx = irr;
    }
    return slot;
}

/* Per-frame: select the dominant baked reflection probe + light-probe SH9
 * group (nearest to the camera) and stash them on sr for the bind cbs. */
void sr_gather_baked_gi(JceSceneRenderer *sr, JceScene *scene,
                               const JceCamera *camera, EntityList *list)
{
    sr->gi_probe_active = false;
    sr->gi_sh9_active   = false;
    sr->gi_probe_spec.idx = UINT16_MAX;
    sr->gi_probe_irr.idx  = UINT16_MAX;

    jce_vec3 cam = camera ? jce_camera_get_position(camera) : jce_v3(0, 0, 0);

    float best_probe_d2 = FLT_MAX;
    float best_sh9_d2   = FLT_MAX;

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        JceTransform *xf = jce_scene_get_transform(scene, e);

        /* Reflection probe with a baked cubemap on disk. */
        if (jce_scene_has_reflection_probe(scene, e)) {
            JceReflectionProbeComponent *rp =
                jce_scene_get_reflection_probe(scene, e);
            { static int s_rp_cid = -2;
              if (s_rp_cid == -2) s_rp_cid = jce_component_find("ReflectionProbe");
              if (rp && s_rp_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_rp_cid)) rp = NULL; }
            if (rp && rp->baked_cubemap_path[0]) {
                jce_vec3 p = xf ? xf->position : jce_v3(0, 0, 0);
                p.x += rp->box_offset[0];
                p.y += rp->box_offset[1];
                p.z += rp->box_offset[2];
                jce_vec3 d = jce_v3_sub(p, cam);
                float d2 = jce_v3_dot(d, d);
                if (d2 < best_probe_d2) {
                    int slot = sr_rprobe_cache_get(sr, rp->baked_cubemap_path);
                    if (slot >= 0) {
                        best_probe_d2 = d2;
                        sr->gi_probe_spec = sr->rprobe_cache[slot].spec;
                        sr->gi_probe_irr  = sr->rprobe_cache[slot].irr;
                        sr->gi_probe_spec_mips = sr->rprobe_cache[slot].spec_mips;
                        sr->gi_probe_intensity =
                            rp->intensity > 0.0f ? rp->intensity : 1.0f;
                        sr->gi_probe_active = true;
                    }
                }
            }
        }

        /* Light probe group with baked SH9. */
        if (jce_scene_has_light_probe_group(scene, e)) {
            JceLightProbeGroupComponent *lpg =
                jce_scene_get_light_probe_group(scene, e);
            if (lpg && lpg->sh9_baked && lpg->probe_count > 0) {
                /* Nearest individual probe within the group. */
                jce_vec3 base = xf ? xf->position : jce_v3(0, 0, 0);
                int best_pi = -1;
                float best_pd2 = FLT_MAX;
                for (int pi = 0; pi < lpg->probe_count &&
                                 pi < JCE_LIGHT_PROBE_MAX; pi++) {
                    jce_vec3 p = jce_v3(base.x + lpg->positions[pi][0],
                                        base.y + lpg->positions[pi][1],
                                        base.z + lpg->positions[pi][2]);
                    jce_vec3 d = jce_v3_sub(p, cam);
                    float d2 = jce_v3_dot(d, d);
                    if (d2 < best_pd2) { best_pd2 = d2; best_pi = pi; }
                }
                if (best_pi >= 0 && best_pd2 < best_sh9_d2) {
                    best_sh9_d2 = best_pd2;
                    for (int c = 0; c < 9; c++) {
                        sr->gi_sh9[c][0] = lpg->sh9[best_pi][c][0];
                        sr->gi_sh9[c][1] = lpg->sh9[best_pi][c][1];
                        sr->gi_sh9[c][2] = lpg->sh9[best_pi][c][2];
                    }
                    sr->gi_sh9_active = true;
                }
            }
        }
    }
}

/* Apply baked-GI uniforms + (optional) reflection-probe cubemap override.
 * Shared by the queue and inline bind paths. Must run AFTER the sky-IBL
 * bind so the probe overrides stages 6/7 when present. ibl_params is the
 * 4-float vector the caller is about to upload as u_iblParams; this routine
 * forces IBL on (x=1) when a probe is active so fs_pbr takes the IBL path. */
void sr_bind_baked_gi(JceSceneRenderer *sr, float ibl_params[4])
{
    /* Reflection probe overrides the sky prefilter / irradiance. */
    if (sr->gi_probe_active && BGFX_HANDLE_IS_VALID(sr->gi_probe_spec) &&
        BGFX_HANDLE_IS_VALID(sr->brdf_lut)) {
        bgfx_texture_handle_t irr = sr->gi_probe_irr;
        if (!BGFX_HANDLE_IS_VALID(irr)) irr = sr->gi_probe_spec;
        bgfx_set_texture(6, sr->u_ibl_irradiance, irr,               UINT32_MAX);
        bgfx_set_texture(7, sr->u_ibl_prefilter,  sr->gi_probe_spec, UINT32_MAX);
        bgfx_set_texture(8, sr->u_ibl_brdf_lut,   sr->brdf_lut,      UINT32_MAX);
        ibl_params[0] = 1.0f;
        if (sr->gi_probe_spec_mips > 1)
            ibl_params[1] = (float)(sr->gi_probe_spec_mips - 1);
    }

    /* SH9 ambient + GI params (x=sh9 enabled, y=reflection-probe intensity). */
    float gi_params[4] = {
        sr->gi_sh9_active ? 1.0f : 0.0f,
        sr->gi_probe_active ? sr->gi_probe_intensity : 1.0f,
        0.0f, 0.0f
    };
    if (BGFX_HANDLE_IS_VALID(sr->u_gi_params))
        bgfx_set_uniform(sr->u_gi_params, gi_params, 1);

    if (BGFX_HANDLE_IS_VALID(sr->u_sh9)) {
        float sh9[9][4];
        for (int c = 0; c < 9; c++) {
            sh9[c][0] = sr->gi_sh9_active ? sr->gi_sh9[c][0] : 0.0f;
            sh9[c][1] = sr->gi_sh9_active ? sr->gi_sh9[c][1] : 0.0f;
            sh9[c][2] = sr->gi_sh9_active ? sr->gi_sh9[c][2] : 0.0f;
            sh9[c][3] = 0.0f;
        }
        bgfx_set_uniform(sr->u_sh9, sh9, 9);
    }

    /* Look Profile uniforms (plan 02). This is the SINGLE lit-submit funnel
     * (queue + inline both pass through here) → zero new call sites, three
     * driver paths inherit automatically.  Neutral packing when inactive. */
    if (BGFX_HANDLE_IS_VALID(sr->u_look_wrap))
        bgfx_set_uniform(sr->u_look_wrap, sr->look_gpu.wrap, 1);
    if (BGFX_HANDLE_IS_VALID(sr->u_look_rim))
        bgfx_set_uniform(sr->u_look_rim, sr->look_gpu.rim, 1);
    if (BGFX_HANDLE_IS_VALID(sr->u_look_hemi_ground))
        bgfx_set_uniform(sr->u_look_hemi_ground, sr->look_gpu.hemi, 1);
}

