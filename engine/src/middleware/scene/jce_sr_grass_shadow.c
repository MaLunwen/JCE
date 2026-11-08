/*
 * jce_sr_grass_shadow.c  Grass in the shadow cascades — the pure half.
 *
 * See jce_sr_grass_shadow.h.  The cell test here is the SAME test the colour
 * pass applies (sr_draw_grass_field in jce_sr_environment.c), down to the
 * wind-sway margin: if the two disagreed, a blade could cast a shadow it is
 * not drawn with, or be drawn with no shadow while its neighbour has one --
 * and either reads as a shading bug rather than as two culls that differ.
 */

#include "jce_sr_grass_shadow.h"

#include <jce/os/core/jce_frustum.h>

bool jce_grass_shadow_field_casts(const JceGrassFieldComponent *g,
                                  bool comp_enabled, bool visible,
                                  uint32_t resident_blades)
{
    if (!g) return false;
    /* This helper only admits a nonempty current resident request. A zero
     * authored result is accepted separately by the production transaction. */
    return g->cast_shadow && comp_enabled && visible && resident_blades > 0u;
}

uint32_t jce_grass_shadow_runs(const uint32_t *cell_start,
                               uint16_t gx, uint16_t gz,
                               float min_x, float min_z, float cell_size,
                               float ymin, float ymax,
                               const jce_vec4 *planes,
                               jce_vec3 cam, float fade_end,
                               JceGrassShadowRun *out, uint32_t max_runs)
{
    if (!cell_start || !planes || !out || max_runs == 0u ||
        gx == 0u || gz == 0u || cell_size <= 0.0f)
        return 0u;

    /* Same slack the colour pass uses, for the same reason: a blade sways and
     * has width, so its cell's box is not the box its geometry occupies. */
    const float marg = cell_size * 0.25f + 0.5f;
    const float fade = fade_end > 0.0f ? fade_end : 0.0f;
    const uint32_t ncells = (uint32_t)gx * (uint32_t)gz;

    uint32_t n_runs = 0u, run_start = 0u, run_len = 0u;
    bool have_run = false;

    for (uint32_t c = 0; c < ncells; ++c) {
        const uint32_t bs = cell_start[c], be = cell_start[c + 1u];
        bool vis = (be != bs);                     /* empty cells add nothing */
        float x0 = 0.0f, x1 = 0.0f, z0 = 0.0f, z1 = 0.0f;
        if (vis) {
            const uint16_t cx = (uint16_t)(c % gx), cz = (uint16_t)(c / gx);
            x0 = min_x + (float)cx * cell_size - marg;
            x1 = min_x + (float)(cx + 1) * cell_size + marg;
            z0 = min_z + (float)cz * cell_size - marg;
            z1 = min_z + (float)(cz + 1) * cell_size + marg;
            const jce_vec3 amn = { x0, ymin, z0 }, amx = { x1, ymax, z1 };
            vis = jce_aabb_in_frustum(planes, amn, amx);
        }
        if (vis && fade > 0.0f) {
            const float qx = cam.x < x0 ? x0 : (cam.x > x1 ? x1 : cam.x);
            const float qz = cam.z < z0 ? z0 : (cam.z > z1 ? z1 : cam.z);
            const float dx = cam.x - qx, dz = cam.z - qz;
            if (dx * dx + dz * dz > fade * fade) vis = false;
        }

        if (!vis) {
            if (have_run) {
                out[n_runs].start = run_start;
                out[n_runs].count = run_len;
                if (++n_runs >= max_runs) return n_runs;
                have_run = false;
            }
            continue;
        }
        /* Cells are packed contiguous, so adjacent visible cells merge into
         * one submit -- the common whole-field case is a single range. */
        if (!have_run) { run_start = bs; run_len = be - bs; have_run = true; }
        else           { run_len += be - bs; }
    }
    if (have_run && n_runs < max_runs) {
        out[n_runs].start = run_start;
        out[n_runs].count = run_len;
        ++n_runs;
    }
    return n_runs;
}

/* Current-content preparation and per-cascade streaming share one provider. */
#include "jce_sr_internal.h"
#include "jce_sr_grass_shadow_content.h"
#include "renderer/jce_shader_deferred.h"
#include "os/core/jce_trace_scope_internal.h"

typedef struct {
    JceSceneRenderer *sr;
    JceScene *scene;
    EntityList *list;
    const JceCamera *camera;
    const jce_vec4 *planes;
    uint16_t view;
    int prepared_slot;
} SrGrassShadowContext;

static JceSrGrassShadowRead sr_grass_shadow_read(uint32_t index,
    JceSrGrassShadowItem *item, void *user)
{
    SrGrassShadowContext *context = user;
    if (index >= (uint32_t)context->list->count) return JCE_GRASS_SHADOW_READ_FAILED;
    const JceEntity entity = context->list->entities[index];
    const JceGrassFieldComponent *field =
        jce_scene_get_grass_field(context->scene, entity);
    if (!field || !field->cast_shadow ||
        !sr_grass_field_admitted(context->sr, context->scene, entity))
        return JCE_GRASS_SHADOW_ABSENT;
    item->entity = entity;
    item->visible = item->cast_shadow = true;
    if (jce_scene_has_transform(context->scene, entity)) return JCE_GRASS_SHADOW_CASTER;
    if (!(context->sr->grass_gate_reported_reasons & 2048u)) {
        context->sr->grass_gate_reported_reasons |= 2048u;
        LOG_WARN(LOG_TAG, "grass shadow source rejected first_reason=missing_transform "
            "owner=%p view=%u entity=%llu mask='%.*s' current_shadow_ready=0",
            (void *)context->sr, (unsigned)context->view, (unsigned long long)entity,
            255, field->density_mask_path);
    }
    return JCE_GRASS_SHADOW_READ_FAILED;
}

static bool sr_grass_shadow_prepare(JceSrGrassShadowItem *item, void *user)
{
    SrGrassShadowContext *context = user;
    const int slot = sr_grass_prepare_roots(context->sr, context->scene,
        context->list, item->entity, true, NULL);
    context->prepared_slot = slot;
    if (slot < 0) return false;
    JceSceneRenderer *sr = context->sr;
    const JceGrassFieldComponent *field = jce_scene_get_grass_field(context->scene, item->entity);
    item->placement_hash = sr->grass_cache[slot].param_hash;
    item->count = sr->grass_cache[slot].inst_count;
    const jce_vec3 position = context->camera
        ? jce_camera_get_position(context->camera) : jce_v3(0.0f, 0.0f, 0.0f);
    item->geometry_hash = sr_grass_shadow_geometry_key(field,
        sr->grass_cache[slot].blade_h, sr->grass_cache[slot].blade_w,
        sr->grass_cache[slot].blade_cards, position,
        jce_environment_wind_speed_now(sr->env), sr->grass_time);
    return true;
}

static bool sr_grass_shadow_submit(const JceSrGrassShadowItem *item, void *user)
{
    SrGrassShadowContext *context = user;
    JceSceneRenderer *sr = context->sr;
    const int slot = context->prepared_slot;
    if (!item->count) return true;
    const bool expanded = sr_grass_expanded_mode();
    if (slot < 0 || !sr->grass_cache[slot].ready ||
        sr->grass_cache[slot].entity != item->entity ||
        sr->grass_cache[slot].inst_count != item->count ||
        sr->grass_cache[slot].param_hash != item->placement_hash) return false;
    if (!expanded && (sr->grass_cache[slot].inst_vb_count != item->count ||
        sr->grass_cache[slot].inst_vb_hash != item->placement_hash ||
        !BGFX_HANDLE_IS_VALID(sr->grass_cache[slot].inst_vb))) return false;
    const JceGrassFieldComponent *field = jce_scene_get_grass_field(context->scene, item->entity);
    JceMesh *blade = sr->grass_cache[slot].blade;
    if (!field || !blade) return false;
    const bgfx_program_handle_t program = {
        jce_shader_deferred_resolve(sr->prog_grass_shadow.idx)
    };
    const bgfx_vertex_buffer_handle_t vertices = {(uint16_t)jce_mesh_get_vbh(blade)};
    const bgfx_index_buffer_handle_t indices = {(uint16_t)jce_mesh_get_ibh(blade)};
    if (!BGFX_HANDLE_IS_VALID(program) || !BGFX_HANDLE_IS_VALID(vertices) ||
        !BGFX_HANDLE_IS_VALID(indices)) return false;

    /* At most one run per cell; the old 64-run scratch could silently omit
     * later alternating visible cells. Unbinned packing remains a full draw. */
    JceGrassShadowRun runs[JCE_GRASS_CELL_CAP];
    uint32_t run_count = 1u;
    runs[0].start = 0u;
    runs[0].count = item->count;
    if (sr->grass_cache[slot].grid_gx) {
        run_count = jce_grass_shadow_runs(sr->grass_cache[slot].cell_start,
            sr->grass_cache[slot].grid_gx, sr->grass_cache[slot].grid_gz,
            sr->grass_cache[slot].grid_min_x, sr->grass_cache[slot].grid_min_z,
            sr->grass_cache[slot].cell_size, sr->grass_cache[slot].field_ymin,
            sr->grass_cache[slot].field_ymax, context->planes,
            context->camera ? jce_camera_get_position(context->camera) : jce_v3(0.0f, 0.0f, 0.0f),
            field->fade_end, runs, JCE_GRASS_CELL_CAP);
    }
    const uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
    for (uint32_t index = 0u; index < run_count; ++index) {
        if (!runs[index].count) continue;
        if (runs[index].start > item->count ||
            runs[index].count > item->count - runs[index].start) return false;
        if (expanded) {
            if (!sr_grass_expanded_bind(sr, slot, runs[index].start, runs[index].count))
                return false;
        } else {
            bgfx_set_vertex_buffer(0, vertices, 0u, UINT32_MAX);
            bgfx_set_index_buffer(indices, 0u, jce_mesh_index_count(blade));
            bgfx_set_instance_data_from_dynamic_vertex_buffer(sr->grass_cache[slot].inst_vb,
                runs[index].start, runs[index].count);
        }
        sr_grass_bind_geometry_uniforms(sr, field, context->camera);
        bgfx_set_state(state, 0u);
        bgfx_submit(context->view, program, 0u, BGFX_DISCARD_ALL);
    }
    return true;
}

bool sr_prepare_grass_shadows(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list, const JceCamera *camera, uint64_t *key)
{
    if (!sr || !scene || !list || list->count < 0 || !key) return false;
    SrGrassShadowContext context = {sr, scene, list, camera, NULL, 0u, -1};
    const JceSrGrassShadowProvider provider = {
        sr_grass_shadow_read, sr_grass_shadow_prepare, NULL, &context
    };
    JceTraceScope scope = jce_trace_scope_begin("sr.grass.prepare");
    const bool complete = sr_grass_shadow_content_walk((uint32_t)list->count, &provider, key);
    jce_trace_scope_end(scope, complete ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return complete;
}

bool sr_submit_grass_shadows(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list, uint16_t view, const jce_vec4 *planes, const JceCamera *camera)
{
    if (!sr || !scene || !list || list->count < 0 || !planes) return false;
    SrGrassShadowContext context = {sr, scene, list, camera, planes, view, -1};
    const JceSrGrassShadowProvider provider = {
        sr_grass_shadow_read, sr_grass_shadow_prepare, sr_grass_shadow_submit, &context
    };
    uint64_t key;
    JceTraceScope scope = jce_trace_scope_begin("sr.grass.shadow.submit");
    const bool complete = sr_grass_shadow_content_walk((uint32_t)list->count, &provider, &key);
    jce_trace_scope_end(scope, complete ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return complete;
}
