/* Scene-renderer world/bounds miss replay; serial cache mutations. */
#include "jce_sr_internal.h"
#include "jce_sr_unique_index.h"
#include "jce_sr_nodraw.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>

/* Fix #1: one-shot per-entity render-kind classification (see SrEntityCull /
 * SR_RK_*).  Called ONCE per entity per frame in the ecull build region so the
 * color/shadow/depth-velocity passes read ecull[i].render_kind instead of each
 * re-running this same has_* dispatch ladder.  CONSERVATIVE: any diverting
 * component → SR_RK_OTHER, so the pass falls back to its full (unchanged) ladder
 * — only a provably-plain primitive mesh gets the fast path. */
void sr_classify_entity(JceScene *scene, JceEntity e, SrEntityCull *ec)
{
    ec->parallel_eligible = false;
    ec->kc_enabled = entity_enabled(scene, e);
    /* Cache "is a light" so the light gather + selection skip non-light entities
     * cheaply (a byte read) instead of re-probing the light components over
     * all N.
     *
     * EVERY light kind has to be in this OR.  The gather loop skips on this
     * byte BEFORE it looks at any component, so a kind missing here is a
     * light that parses, serialises, appears in the Inspector and reaches no
     * pixel -- which is exactly what the first area-light measurement showed:
     * the strip lit nothing, and lighting it with a 0.05 x 0.05 rectangle
     * produced a byte-identical frame. */
    ec->kc_has_light = jce_scene_has_dir_light(scene, e)
                    || jce_scene_has_point_light(scene, e)
                    || jce_scene_has_spot_light(scene, e)
                    || jce_scene_has_area_light(scene, e);
    if (!ec->kc_enabled) { ec->render_kind = SR_RK_DISABLED; return; }
    if (jce_scene_has_terrain(scene, e)            ||
        jce_scene_has_tilemap(scene, e)            ||
        jce_scene_has_vegetation_scatter(scene, e) ||
        jce_scene_has_line_renderer(scene, e)      ||
        jce_scene_has_trail_renderer(scene, e)     ||
        jce_scene_has_water(scene, e)              ||
        jce_scene_has_grass_field(scene, e)        ||
        jce_scene_has_skeletal_animator(scene, e)  ||
        jce_scene_has_sprite_animator(scene, e)    ||
        jce_scene_has_sprite_renderer(scene, e)    ||
        jce_scene_has_billboard_renderer(scene, e) ||
        jce_scene_has_video_player(scene, e)       ||
        jce_scene_has_foliage_cluster(scene, e)    ||
        jce_scene_has_lod_group(scene, e)) {
        ec->render_kind = SR_RK_OTHER;
        return;
    }
    if (jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr) {
            /* No skinned/LOD/special parts reached here → plain mesh.  glTF path →
             * MODEL (multi-primitive model submit); else → PRIM_MESH (shared mesh). */
            ec->render_kind = sr_is_gltf_model_path(mr->mesh_path)
                                  ? SR_RK_MODEL : SR_RK_PRIM_MESH;
            /* Parallel-gather eligible: a PRIM_MESH whose material is pure
             * factors (no texture maps, no custom .mat.json program) — its cmd
             * is a pure function of read-only state, buildable on a worker. */
            ec->parallel_eligible =
                (ec->render_kind == SR_RK_PRIM_MESH) &&
                !mr->mesh_path[0] &&   /* pure shape primitive → shared mesh, no cache load */
                mr->alpha_mode != JCE_ALPHA_BLEND &&  /* opaque queue only (no transparent drop) */
                !mr->has_albedo_runtime &&  /* runtime albedo → not factor-only (texture path) */
                !mr->albedo_tex[0] && !mr->mr_tex[0] && !mr->normal_tex[0] &&
                !mr->ao_tex[0] && !mr->emissive_tex[0] && !mr->material_path[0];
            return;
        }
    }
    ec->render_kind = sr_entity_has_no_draw(scene, e) ? SR_RK_NO_DRAW : SR_RK_OTHER;
}

/* True when this entity OR any ancestor carries a runtime-moved component, so it
 * must NOT be put in the persistent static cache.  PUBLIC (jce_scene.h): static
 * batching has to ask exactly the same question, and two definitions of
 * "static" is how a batch swallows an object that then cannot move. */
bool jce_scene_entity_is_dynamic(JceScene *scene, JceEntity e)
{
    JceEntity cur = e;
    for (int depth = 0; depth < 32 && cur != JCE_ENTITY_INVALID; depth++) {
        if (jce_scene_has_rigidbody(scene, cur)            ||
            jce_scene_has_rigidbody2d(scene, cur)          ||
            jce_scene_has_character_controller(scene, cur) ||
            jce_scene_has_skeletal_animator(scene, cur)    ||
            jce_scene_has_vehicle(scene, cur)              ||
            jce_scene_has_wheel_collider(scene, cur)       ||
            jce_scene_has_soft_body(scene, cur))
            return true;
        JceEntity p = jce_scene_get_parent(scene, cur);
        if (p == cur) break;          /* cycle guard */
        cur = p;
    }
    /* depth-limit reached without resolving a root → be conservative. */
    if (cur != JCE_ENTITY_INVALID) return true;
    return false;
}

void sr_ecull_metadata_begin(JceSceneRenderer *sr, JceScene *scene,
                             uint64_t list_generation, uint64_t structural_generation)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *value = getenv("JCE_DISABLE_ECULL_METADATA");
        disabled = value && value[0] && value[0] != '0';
    }
    sr->ecull_metadata_next = (SrMetadataKey){
        scene, list_generation, structural_generation,
        jce_scene_get_enable_gen(scene), jce_scene_get_cull_data_gen(scene)
    };
    sr->ecull_metadata_trusted = !disabled && sr->kindcache_on &&
        sr->ecull_metadata_valid && sr_metadata_key_matches(
            &sr->ecull_metadata_key, &sr->ecull_metadata_next);
}

void sr_ecull_metadata_commit(JceSceneRenderer *sr)
{
    sr->ecull_metadata_key = sr->ecull_metadata_next;
    sr->ecull_metadata_valid = sr->kindcache_on;
}

/* Diagnostic subdivision is opt-in: normal timing runs pay no timer reads. */
static bool sr_ecull_detail_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("JCE_PERF_ECULL_DETAIL");
        enabled = (v && v[0] == '1') ? 1 : 0;
    }
    return enabled && jce_perf_phase_enabled();
}

static void sr_ecull_detail_phase(bool enabled, uint64_t *start,
                                  const char *name)
{
    if (enabled) {
        const uint64_t end = jce_time_perf_counter();
        jce_perf_phase_add(name, jce_time_perf_to_ms(*start, end));
        *start = end;
    }
}

/* wcache-miss slow path: compose the world matrix, resolve the caster AABB,
 * classify, store statics for cross-frame reuse, and fold the caster into the
 * shadow-cache key.  Touches the model/mesh resolve caches => serial only. */
void sr_ecull_build_miss(JceSceneRenderer *sr, JceScene *scene,
                                const EntityList *list, int eci,
                                uint64_t struct_epoch, bool wcache_on)
{
    const bool detail = sr_ecull_detail_enabled();
    uint64_t start = detail ? jce_time_perf_counter() : 0;
    JceEntity e = list->entities[eci];
    const uint64_t e_gen = jce_scene_entity_xform_gen(scene, e);
    jce_vec3 mn = jce_v3(0, 0, 0), mx = jce_v3(0, 0, 0);
    /* #8 — compose the world matrix ONCE here (identical source to every
     * later jce_scene_get_world_matrix call) and cache it.  The shadow
     * cascades / depth prepass / color submit read sr->ecull_world[eci]
     * instead of re-walking the parent chain ~5×/entity/frame. */
    const jce_mat4 *wptr = NULL;
    if (jce_scene_has_transform(scene, e)) {
        sr->ecull_world[eci]       = jce_scene_get_world_matrix(scene, e);
        sr->ecull[eci].world_valid = true;
        wptr = &sr->ecull_world[eci];
    } else {
        sr->ecull[eci].world_valid = false;
    }
    sr_ecull_detail_phase(detail, &start, "ecull_world");
    sr->ecull[eci].has_aabb     = sr_shadow_caster_aabb(sr, scene, e, wptr, &mn, &mx);
    sr->ecull[eci].wmin         = mn;
    sr->ecull[eci].wmax         = mx;
    if (!sr->ecull_metadata_trusted)
        sr->ecull[eci].casts_shadow = sr_entity_casts_shadow(scene, e);

    sr_ecull_detail_phase(detail, &start, "ecull_bounds");

    /* Fix#1 (rank-1): classify render-kind HERE on the miss path so the wcache
     * store below captures it — every later frame's hit then skips the ~16
     * has_* probes.  Dynamics (never stored) re-run this each frame as before. */
    if (sr->kindcache_on && !sr->ecull_metadata_trusted)
        sr_classify_entity(scene, e, &sr->ecull[eci]);
    sr->ecull[eci].layer = jce_scene_get_entity_layer(scene, e);
    sr->ecull_light_byte[eci] = sr->ecull[eci].kc_has_light ? 1u : 0u;
    sr->ecull_fast_byte[eci]  = SR_RK_IS_FAST(sr->ecull[eci].render_kind) ? 1u : 0u;

    sr_ecull_detail_phase(detail, &start, "ecull_classify");

    /* Store STATIC entities for cross-frame reuse.  Dynamics (rigidbody /
     * character / skeletal / vehicle / wheel / softbody on the entity or any
     * ancestor) are mutated in place by the runtime without an epoch bump, so
     * they are never cached and always take the recompute path above. */
    if (!sr->ecull_metadata_trusted)
        sr->ecull[eci].transform_dynamic = jce_scene_entity_is_dynamic(scene, e);
    const bool e_is_dynamic = sr->ecull[eci].transform_dynamic;
    if (wcache_on && !e_is_dynamic)
        sr_wcache_store(sr, e, &sr->ecull[eci],
                        &sr->ecull_world[eci], struct_epoch, e_gen);

    sr_ecull_detail_phase(detail, &start, "ecull_static_store");

    /* Shadow-cache key (miss path): fold this caster into the key; a dynamic
     * caster (mutated in place without an xform_gen bump) can't be detected
     * by the key, so flag it to force the shadow pass to re-render. */
    sr->ecull[eci].is_dyn_caster =
        (e_is_dynamic && sr->ecull[eci].casts_shadow);
    if (sr->ecull[eci].casts_shadow) {
        sr->shadow_caster_key =
            sr_shadow_fold_caster(sr->shadow_caster_key, (uint64_t)e, e_gen);
        /* Dual mode: the static-only key folds NON-dynamic casters only, so a
         * mover's xform_gen churn leaves the static cascade cache valid. */
        if (!e_is_dynamic)
            sr->shadow_static_caster_key =
                sr_shadow_fold_caster(sr->shadow_static_caster_key,
                                      (uint64_t)e, e_gen);
        if (e_is_dynamic) {
            sr->shadow_has_dynamic_caster = true;
            /* The full build starts a new epoch; incremental repair keeps
             * it. Membership is constant time, including duplicate repairs. */
            uint64_t *stamp = &sr->ecull[eci].shadow_list_epoch;
            if (*stamp != sr->shadow_dyn_epoch) {
                if (sr->shadow_dyn_count >= sr->shadow_dyn_cap) {
                    uint32_t nc = sr->shadow_dyn_cap ? sr->shadow_dyn_cap * 2u : 64u;
                    uint32_t *g = (uint32_t *)JCE_REALLOC(
                        sr->shadow_dyn, nc * sizeof(uint32_t));
                    if (g) { sr->shadow_dyn = g; sr->shadow_dyn_cap = nc; }
                }
                if (!sr_unique_index_append(sr->shadow_dyn, &sr->shadow_dyn_count,
                        sr->shadow_dyn_cap, (uint32_t)eci, sr->shadow_dyn_epoch, stamp))
                    sr->shadow_dyn_overflow = true;
            }
        }
    }

    sr_ecull_detail_phase(detail, &start, "ecull_shadow_list");

    /* Persist the world AABB for next frame's focus-bounded collect (slice 6:
     * fused into the build; the standalone post-loop store walk is gone).
     * Serial path => full store (may insert/grow). */
    if (sr->ecull[eci].has_aabb)
        sr_focus_aabb_store(sr, e, sr->ecull[eci].wmin,
                            sr->ecull[eci].wmax);
    sr_ecull_detail_phase(detail, &start, "ecull_focus_store");
}

/* Epoch wrap is rare, but must never confuse a recycled slot with a hit. */
void sr_ecull_reset_dynamic_list(JceSceneRenderer *sr)
{
    sr->shadow_dyn_count = 0;
    sr->shadow_dyn_overflow = false;
    if (++sr->shadow_dyn_epoch == 0) {
        uint32_t i;
        for (i = 0; i < sr->ecull_cap; ++i)
            sr->ecull[i].shadow_list_epoch = 0;
        sr->shadow_dyn_epoch = 1;
    }
}
