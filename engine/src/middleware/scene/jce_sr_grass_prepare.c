/* Grass CPU preparation and resident roots: one transaction for colour/depth. */
#include "jce_sr_internal.h"
#include "jce_sr_foliage_buffer.h"
#include "jce_sr_grass_shadow_content.h"
#include "jce_sr_grass_mask.h"
#include "renderer/jce_shader_deferred.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_alloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool sr_grass_kill_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = jce_env_flag_enabled(getenv("JCE_NO_GRASS"));
    return enabled != 0;
}

bool sr_grass_persist_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = sr_foliage_persist_enabled(getenv("JCE_PERSIST_GRASS"), false);
    return enabled != 0;
}

bool sr_grass_field_admitted(JceSceneRenderer *sr, JceScene *scene, JceEntity entity)
{
    const JceGrassFieldComponent *field = jce_scene_get_grass_field(scene, entity);
    static int component_id = -2;
    if (component_id == -2) component_id = jce_component_find("GrassField");
    const bool expanded = sr_grass_expanded_mode();
    return field && field->visible && entity_enabled(scene, entity) &&
        !sr_grass_kill_enabled() && sr->grass_enabled &&
        (component_id < 0 || jce_scene_comp_enabled(scene, entity, component_id)) &&
        (expanded || ((jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0 &&
         (jce_renderer_get_tier() >= JCE_GPU_TIER_MEDIUM ||
          jce_renderer_get_active_backend() == JCE_BACKEND_OPENGLES)));
}

static float sr_grass_density_scale(void)
{
    static float override_scale = -1.0f;
    static bool observed;
    if (!observed) {
        observed = true;
        const char *value = getenv("JCE_GRASS_DENSITY");
        if (value && value[0]) {
            const float parsed = (float)atof(value);
            override_scale = isfinite(parsed) && parsed >= 0.05f && parsed <= 1.0f
                ? parsed : 1.0f;
        }
    }
    if (override_scale >= 0.0f) return override_scale;
    return jce_renderer_get_tier() <= JCE_GPU_TIER_LOW ? 0.5f : 1.0f;
}

static int sr_grass_prepare_failure(JceSceneRenderer *sr, int slot,
    JceEntity entity, const JceGrassFieldComponent *field,
    uint32_t reason_bit, const char *reason)
{
    sr->grass_cache[slot].ready = false;
    if (!(sr->grass_gate_reported_reasons & reason_bit)) {
        sr->grass_gate_reported_reasons |= reason_bit;
        LOG_WARN(LOG_TAG, "grass preparation rejected first_reason=%s owner=%p "
            "entity=%llu mask='%.*s' project=%d caps=0x%x tier=%d "
            "program=%u depth_program=%u current_roots_ready=0", reason, (void *)sr,
            (unsigned long long)entity, 255, field ? field->density_mask_path : "",
            (int)sr->grass_enabled, (unsigned)jce_renderer_get_caps(),
            (int)jce_renderer_get_tier(), (unsigned)sr->prog_grass.idx,
            (unsigned)sr->prog_grass_shadow.idx);
    }
    return -1;
}

/* Retry failed shader admission only at the existing explicit cache reset.
 * Valid prepared programs survive; failed deferred programs release their
 * source identity through the ordinary shader owner before retry. */
void sr_grass_reset_failed_programs(JceSceneRenderer *sr)
{
    if (!sr) return;
    bgfx_program_handle_t *programs[2] = {&sr->prog_grass, &sr->prog_grass_shadow};
    bool *tried[2] = {&sr->grass_prog_tried, &sr->grass_shadow_prog_tried};
    for (unsigned i = 0; i < 2u; ++i) {
        if (BGFX_HANDLE_IS_VALID(*programs[i]) &&
            jce_shader_deferred_resolve(programs[i]->idx) != UINT16_MAX) continue;
        if (BGFX_HANDLE_IS_VALID(*programs[i])) {
            const JceShaderHandle handle = {programs[i]->idx};
            jce_shader_program_destroy(handle);
        }
        programs[i]->idx = UINT16_MAX;
        *tried[i] = false;
    }
    ++sr->grass_resource_generation;
    sr->grass_gate_reported_reasons = 0u;
}

static bool sr_grass_program_result(JceSceneRenderer *sr, JceEntity entity,
    bool need_shadow, uint32_t reason)
{
    JceTraceScope scope = jce_trace_scope_begin("sr.grass.program.admission");
    if (scope.id) {
#define PROGRAM_COUNT(name_, value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, name_, (int64_t)(value_), scope.id, scope.id)
        PROGRAM_COUNT("grass.program.owner", (uintptr_t)sr);
        PROGRAM_COUNT("grass.program.entity", entity);
        PROGRAM_COUNT("grass.program.generation", sr->grass_resource_generation);
        PROGRAM_COUNT("grass.program.colour", sr->prog_grass.idx);
        PROGRAM_COUNT("grass.program.depth", sr->prog_grass_shadow.idx);
        PROGRAM_COUNT("grass.program.need_shadow", need_shadow);
        PROGRAM_COUNT("grass.program.reason", reason);
#undef PROGRAM_COUNT
    }
    jce_trace_scope_end(scope, reason ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_SUCCEEDED);
    return reason == 0u;
}

static bool sr_grass_program_ready(JceSceneRenderer *sr,
    JceEntity entity, bool need_shadow)
{
    sr_grass_lazy_init(sr);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_grass) || !sr->grass_blade ||
        !BGFX_HANDLE_IS_VALID(sr->u_grass_time) ||
        !BGFX_HANDLE_IS_VALID(sr->u_grass_wind) ||
        !BGFX_HANDLE_IS_VALID(sr->u_grass_fade) ||
        !BGFX_HANDLE_IS_VALID(sr->u_grass_camera) ||
        !BGFX_HANDLE_IS_VALID(sr->u_grass_color) ||
        jce_shader_deferred_resolve(sr->prog_grass.idx) == UINT16_MAX) {
        if (!(sr->grass_gate_reported_reasons & 1024u)) {
            sr->grass_gate_reported_reasons |= 1024u;
            LOG_WARN(LOG_TAG, "grass colour resources unavailable owner=%p "
                "entity=%llu generation=%llu vertex='grass' fragment='grass' "
                "program=%u tried=%d "
                "time_uniform=%u wind_uniform=%u fade_uniform=%u camera_uniform=%u "
                "colour_uniform=%u current_roots_ready=0", (void *)sr,
                (unsigned long long)entity,
                (unsigned long long)sr->grass_resource_generation,
                (unsigned)sr->prog_grass.idx, (int)sr->grass_prog_tried,
                (unsigned)sr->u_grass_time.idx, (unsigned)sr->u_grass_wind.idx,
                (unsigned)sr->u_grass_fade.idx, (unsigned)sr->u_grass_camera.idx,
                (unsigned)sr->u_grass_color.idx);
        }
        return sr_grass_program_result(sr, entity, need_shadow, 1024u);
    }
    if (!need_shadow) return sr_grass_program_result(sr, entity, false, 0u);
    if (!sr->grass_shadow_prog_tried) {
        sr->grass_shadow_prog_tried = true;
        const char *vertex = sr_grass_expanded_mode() ? "grass_expanded" : "grass";
        sr->prog_grass_shadow.idx = jce_renderer_shader_load_named_internal(sr->pak, vertex, "grass_shadow").idx;
    }
    if (!BGFX_HANDLE_IS_VALID(sr->prog_grass_shadow) ||
        jce_shader_deferred_resolve(sr->prog_grass_shadow.idx) == UINT16_MAX) {
        if (!(sr->grass_gate_reported_reasons & 256u)) {
            sr->grass_gate_reported_reasons |= 256u;
            LOG_WARN(LOG_TAG, "grass depth program unavailable owner=%p "
                "entity=%llu generation=%llu vertex='grass' fragment='grass_shadow' "
                "current_shadow_ready=0", (void *)sr, (unsigned long long)entity,
                (unsigned long long)sr->grass_resource_generation);
        }
        return sr_grass_program_result(sr, entity, need_shadow, 256u);
    }
    return sr_grass_program_result(sr, entity, need_shadow, 0u);
}

static uint16_t sr_grass_roots_create(uint32_t count, bool compute_read, void *user)
{
    (void)user;
    if (compute_read) return UINT16_MAX;
    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD3, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);
    return bgfx_create_dynamic_vertex_buffer(count, &layout, BGFX_BUFFER_NONE).idx;
}

static bool sr_grass_roots_upload(uint16_t handle, const void *roots,
    uint32_t count, void *user)
{
    (void)user;
    if (count > UINT32_MAX / 80u) return false;
    const bgfx_memory_t *memory = bgfx_copy(roots, count * 80u);
    if (!memory) return false;
    const bgfx_dynamic_vertex_buffer_handle_t buffer = {handle};
    bgfx_update_dynamic_vertex_buffer(buffer, 0, memory);
    return true;
}

static void sr_grass_roots_destroy(uint16_t handle, void *user)
{
    (void)user;
    const bgfx_dynamic_vertex_buffer_handle_t buffer = {handle};
    bgfx_destroy_dynamic_vertex_buffer(buffer);
}

bool sr_grass_roots_ensure(JceSceneRenderer *sr, int slot)
{
    if (!sr->grass_cache[slot].ready || !sr_grass_persist_enabled()) return false;
    if (!sr->grass_cache[slot].inst_count) return true;
    bool compute_read = false;
    const JceSrFoliageBufferProvider provider = {
        sr_grass_roots_create, sr_grass_roots_upload, sr_grass_roots_destroy, NULL
    };
    return sr_foliage_buffer_ensure(&sr->grass_cache[slot].inst_vb.idx,
        &sr->grass_cache[slot].inst_vb_hash, &sr->grass_cache[slot].inst_vb_count,
        &compute_read, sr->grass_cache[slot].packed, sr->grass_cache[slot].inst_count,
        sr->grass_cache[slot].param_hash, false, &provider);
}


/* Publish current roots only after complete placement and packing. */
int sr_grass_prepare_roots(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list, JceEntity e, bool need_shadow, SrGrassMaskBatch *mask_batch)
{
    JceGrassFieldComponent *g = jce_scene_get_grass_field(scene, e);
    if (!g || !g->visible) return -1;
    if (!jce_scene_has_transform(scene, e)) return -1;

    if (!sr_grass_program_ready(sr, e, need_shadow)) return -1;

    /* ── Scatter cache (rebuild only on param change) ─────────────── */
    int slot = sr_grass_find_slot(sr, e);
    if (slot < 0) return -1;
    /* used reserves ownership; ready alone admits current CPU content. */
    sr->grass_cache[slot].entity = e;
    sr->grass_cache[slot].used = true;
    const jce_mat4 wm = jce_scene_get_world_matrix(scene, e);
    if (!sr_grass_inputs_finite(g, &wm) ||
        !isfinite(g->wind_speed * jce_environment_wind_speed_now(sr->env)))
        return sr_grass_prepare_failure(sr, slot, e, g, 128u, "invalid_grass_parameters");

    /* This field's own blade geometry.
     *
     * There used to be ONE shared mesh for every field, built once from the
     * literals (0.4, 0.05, 4). blade_height / blade_width / cards were live in
     * the inspector, parsed, serialized, folded into the scatter param hash
     * and used to size the pick proxy and the selection outline -- and the
     * geometry ignored all three. Dragging blade height re-ran the whole
     * scatter, repack and counting sort every frame of the drag, moved the
     * click box, and changed no pixel.
     *
     * A blade is a handful of cards, so one mesh per field costs nothing next
     * to the instance buffer already on this slot. Rebuilt only when one of
     * the three moves. */
    {
        const float bh = g->blade_height > 0.0f ? g->blade_height : 0.4f;
        const float bw = g->blade_width  > 0.0f ? g->blade_width  : 0.05f;
        int   bc = g->cards > 0 ? g->cards : 4;
        if (bc > 6) bc = 6;
        if (!sr->grass_cache[slot].blade ||
            sr->grass_cache[slot].blade_h     != bh ||
            sr->grass_cache[slot].blade_w     != bw ||
            sr->grass_cache[slot].blade_cards != bc) {
            JceMesh *nb = sr_grass_build_blade(bh, bw, bc);
            if (nb) {
                if (sr->grass_cache[slot].blade)
                    jce_mesh_destroy(sr->grass_cache[slot].blade);
                sr->grass_cache[slot].blade       = nb;
                sr->grass_cache[slot].blade_h     = bh;
                sr->grass_cache[slot].blade_w     = bw;
                sr->grass_cache[slot].blade_cards = bc;
            } else {
                return sr_grass_prepare_failure(sr, slot, e, g, 64u, "blade_allocation_failed");
            }
        }
    }
    JceMesh *blade = sr->grass_cache[slot].blade
                   ? sr->grass_cache[slot].blade : sr->grass_blade;
    if (!blade) return sr_grass_prepare_failure(sr, slot, e, g, 64u, "blade_unavailable");

    jce_vec3 origin = { wm.col[3].x, wm.col[3].y, wm.col[3].z };
    SrTerrainSurfaceSet surfaces;
    sr_terrain_surfaces_collect(sr, scene, list, origin.y, &surfaces);

    const bool expanded = sr_grass_expanded_mode();
    const float density_scale = sr_grass_density_scale();
    uint32_t ph = sr_grass_placement_input_key(g, &wm, surfaces.hash, density_scale);
    if (expanded) ph = jce_fnv1a32_append(ph, &expanded, sizeof expanded);
    JceFileSystem *content_fs = jce_fs_get_active();
    const JceFsActivePolicy content_policy = jce_fs_get_active_policy();
    const bool rebuild = !sr->grass_cache[slot].ready ||
        sr->grass_cache[slot].input_hash != ph ||
        sr->grass_cache[slot].content_fs != content_fs ||
        sr->grass_cache[slot].content_policy != content_policy;
    if (rebuild) {
        sr->grass_cache[slot].ready = false;
        uint32_t mask_content_hash = 0u;
        /* Zero FIRST, then assign.  Field-by-field initialisation leaves
         * every field added LATER as uninitialised stack -- and this
         * struct grew ridge_field/ridge_dim/ridge_min/ridge_max, which
         * this path never sets.  A garbage ridge_field passes the
         * `!= NULL` guard and is then dereferenced: an access violation
         * inside jce_foliage_scatter, reached simply by turning grass on. */
        JceFoliageScatterParams p;
        memset(&p, 0, sizeof p);
        p.seed          = g->seed;
        p.density       = g->density;
        p.density *= density_scale;
        p.area_x        = g->area_x;
        p.area_z        = g->area_z;
        p.max_slope_deg = g->max_slope_deg;
        p.want_normals  = false;   /* blades are authored upright */
        p.scale_min     = g->scale_min;
        p.scale_max     = g->scale_max;
        p.density_mask  = NULL;
        p.mask_dim      = 0;
        p.mask_world_size = g->mask_world_size;

        /* Diorama dirt-path support: gate blade placement on a density mask so
         * paths / water read through instead of being carpeted.  The reference
         * grows grass only where density >= threshold, so we HARD-threshold the
         * mask's GREEN channel (grass density) to 0/1 for crisp path edges. The
         * grid is freed right after the scatter below. */
        SrGrassMaskLease mask = {0};
        if (g->density_mask_path[0] && g->density_threshold > 0.0f) {
            SrGrassMaskBatch *memo = mask_batch && sr_grass_mask_batch_enabled()
                ? mask_batch : NULL;
            const SrGrassMaskStatus status = sr_grass_mask_acquire(sr, memo,
                g->density_mask_path, g->density_threshold, &mask);
            if (status == SR_GRASS_MASK_UNAVAILABLE)
                return sr_grass_prepare_failure(sr, slot, e, g, 32u,
                    "requested_mask_unavailable");
            if (status == SR_GRASS_MASK_ALLOCATION_FAILED)
                return sr_grass_prepare_failure(sr, slot, e, g, 128u,
                    "mask_grid_allocation_failed");
            mask_content_hash = mask.content_hash;
            p.density_mask = mask.cells;
            p.mask_dim = mask.dim;
        }

        const uint32_t root_limit = expanded
            ? sr_grass_expanded_root_limit(sr->grass_cache[slot].blade_cards)
            : JCE_FOLIAGE_MAX_INSTANCES;
        const double area = (double)p.area_x * (double)p.area_z;
        double desired = (double)p.density * (double)p.area_x * (double)p.area_z;
        if (!isfinite(desired) || !isfinite(p.scale_min) || !isfinite(p.scale_max)) {
            sr_grass_mask_release(&mask);
            return sr_grass_prepare_failure(sr, slot, e, g, 128u, "nonfinite_scatter_parameters");
        }
        if (expanded && root_limit == 0u) {
            sr_grass_mask_release(&mask);
            return sr_grass_prepare_failure(sr, slot, e, g, 64u, "expanded_blade_unavailable");
        }
        if (expanded && desired > (double)root_limit && area > 0.0) {
            /* Lower density before scattering so the capped samples still
             * cover the entire field and keep the authored mask. */
            p.density = (float)((double)root_limit / area);
            desired = (double)root_limit;
        }
        uint32_t want = desired >= (double)root_limit
            ? root_limit : desired > 0.0 ? (uint32_t)desired : 0u;
        if (want == 0) want = 1;

        if (sr->grass_cache[slot].inst_cap < want) {
            JceFoliageInstance *nb = (JceFoliageInstance *)JCE_REALLOC(
                sr->grass_cache[slot].insts,
                (size_t)want * sizeof(JceFoliageInstance));
            if (!nb) {
                sr_grass_mask_release(&mask);
                return sr_grass_prepare_failure(sr, slot, e, g, 128u, "roots_allocation_failed");
            }
            sr->grass_cache[slot].insts    = nb;
            sr->grass_cache[slot].inst_cap = want;
        }

        const uint32_t next_count = sr_foliage_scatter_surfaces(
            &p, &surfaces, &origin,
            sr->grass_cache[slot].insts, expanded ? want : sr->grass_cache[slot].inst_cap);
        sr_grass_mask_release(&mask);
        const uint32_t complete_hash = sr_grass_complete_placement_key(ph, mask_content_hash);

        /* Pre-pack the static stride-80 instance bytes (mat4 + tint) ONCE, in
         * SPATIAL-CELL order so the color pass can frustum + fade-distance cull
         * whole cells.  Blade TRS + index-derived tint do not vary per frame
         * (wind is shader-side via u_grass_time), so the color pass bulk-copies
         * these instead of rebuilding the per-blade matrix every frame.  The
         * tint keeps the blade's ORIGINAL index, and grass is opaque +
         * depth-tested, so the re-ordered draw is pixel-identical. */
        {
            const uint32_t stride80 = (uint32_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
            const uint32_t n        = next_count;
            const uint32_t need_b   = n * stride80;
            if (sr->grass_cache[slot].packed_cap < need_b) {
                uint8_t *pb = (uint8_t *)JCE_REALLOC(sr->grass_cache[slot].packed, need_b);
                if (pb) {
                    sr->grass_cache[slot].packed     = pb;
                    sr->grass_cache[slot].packed_cap = need_b;
                }
            }

            if (n && (!sr->grass_cache[slot].packed ||
                sr->grass_cache[slot].packed_cap < need_b))
                return sr_grass_prepare_failure(sr, slot, e, g, 128u, "packed_roots_allocation_failed");

            /* ── Spatial grid: XZ/Y bounds → grid → counting sort → order[] ── */
            const JceFoliageInstance *ins = sr->grass_cache[slot].insts;
            float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
            float mny = 1e30f, mxy = -1e30f;
            for (uint32_t i = 0; i < n; ++i) {
                float x = ins[i].pos[0], y = ins[i].pos[1], z = ins[i].pos[2];
                if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
                    !isfinite(ins[i].scale))
                    return sr_grass_prepare_failure(sr, slot, e, g, 128u,
                        "nonfinite_prepared_roots");
                if (x < mnx) mnx = x; if (x > mxx) mxx = x;
                if (z < mnz) mnz = z; if (z > mxz) mxz = z;
                if (y < mny) mny = y; if (y > mxy) mxy = y;
            }
            if (n == 0u) { mnx = mnz = mny = 0.0f; mxx = mxz = mxy = 1.0f; }
            float spanx = mxx - mnx, spanz = mxz - mnz;
            float span  = spanx > spanz ? spanx : spanz;
            if (!isfinite(span))
                return sr_grass_prepare_failure(sr, slot, e, g, 128u, "invalid_roots_extent");
            float cell  = span / (float)JCE_GRASS_CELL_DIM;
            if (cell < 1.0f) cell = 1.0f;
            uint16_t gx = (uint16_t)(spanx / cell) + 1u;
            uint16_t gz = (uint16_t)(spanz / cell) + 1u;
            if (gx > (uint16_t)JCE_GRASS_CELL_DIM) gx = (uint16_t)JCE_GRASS_CELL_DIM;
            if (gz > (uint16_t)JCE_GRASS_CELL_DIM) gz = (uint16_t)JCE_GRASS_CELL_DIM;
            if (gx < 1u) gx = 1u;
            if (gz < 1u) gz = 1u;
            uint32_t ncells = (uint32_t)gx * gz;

            uint32_t *cs     = sr->grass_cache[slot].cell_start;
            uint32_t *order  = (n > 0u) ? (uint32_t *)JCE_MALLOC((size_t)n * sizeof(uint32_t)) : NULL;
            uint32_t *cursor = (uint32_t *)JCE_MALLOC((size_t)ncells * sizeof(uint32_t));
            bool binned = false;
            if (n > 0u && order && cursor) {
                memset(cs, 0, (size_t)(ncells + 1u) * sizeof(uint32_t));
                for (uint32_t i = 0; i < n; ++i) {
                    int cx = (int)((ins[i].pos[0] - mnx) / cell);
                    int cz = (int)((ins[i].pos[2] - mnz) / cell);
                    if (cx < 0) cx = 0; if (cx >= (int)gx) cx = (int)gx - 1;
                    if (cz < 0) cz = 0; if (cz >= (int)gz) cz = (int)gz - 1;
                    cs[(uint32_t)cz * gx + (uint32_t)cx + 1u]++;
                }
                for (uint32_t c = 1u; c <= ncells; ++c) cs[c] += cs[c - 1u];
                memcpy(cursor, cs, (size_t)ncells * sizeof(uint32_t));
                for (uint32_t i = 0; i < n; ++i) {
                    int cx = (int)((ins[i].pos[0] - mnx) / cell);
                    int cz = (int)((ins[i].pos[2] - mnz) / cell);
                    if (cx < 0) cx = 0; if (cx >= (int)gx) cx = (int)gx - 1;
                    if (cz < 0) cz = 0; if (cz >= (int)gz) cz = (int)gz - 1;
                    order[cursor[(uint32_t)cz * gx + (uint32_t)cx]++] = i;
                }
                binned = true;
            }
            JCE_FREE(cursor);

            sr->grass_cache[slot].grid_gx    = binned ? gx : 0u;   /* 0 ⇒ no cull */
            sr->grass_cache[slot].grid_gz    = gz;
            sr->grass_cache[slot].grid_min_x = mnx;
            sr->grass_cache[slot].grid_min_z = mnz;
            sr->grass_cache[slot].cell_size  = cell;
            const float scale_extent = fmaxf(1.0f,
                fmaxf(fabsf(g->scale_min), fabsf(g->scale_max)));
            const float extent = scale_extent *
                (sr->grass_cache[slot].blade_h + sr->grass_cache[slot].blade_w) + 0.5f;
            if (!isfinite(extent) || !isfinite(mny - extent) || !isfinite(mxy + extent)) {
                JCE_FREE(order);
                return sr_grass_prepare_failure(sr, slot, e, g, 128u, "invalid_blade_extent");
            }
            sr->grass_cache[slot].field_ymin = mny - extent;
            sr->grass_cache[slot].field_ymax = mxy + extent;

            if (sr->grass_cache[slot].packed &&
                sr->grass_cache[slot].packed_cap >= need_b) {
                for (uint32_t j = 0; j < n; ++j) {
                    uint32_t oi = binned ? order[j] : j;   /* original blade idx */
                    const JceFoliageInstance *fi = &ins[oi];
                    jce_mat4 m = sr_foliage_instance_matrix(fi, 0.0f);
                    float    t  = (float)(oi & 7u) / 7.0f;
                    float    hh = (t - 0.5f) * 2.0f * g->hue_jitter;
                    jce_vec4 tint = { 1.0f - hh, 1.0f, 1.0f + hh, 1.0f };
                    uint8_t *pkp = sr->grass_cache[slot].packed + (size_t)j * stride80;
                    memcpy(pkp,                    JCE_M4_PTR(m), sizeof(jce_mat4));
                    memcpy(pkp + sizeof(jce_mat4), &tint,         sizeof(jce_vec4));
                }
            } else {
                sr->grass_cache[slot].grid_gx = 0u;  /* pack failed ⇒ no cull */
            }
            JCE_FREE(order);
        }
        if (expanded && !sr_grass_expanded_rebuild(sr, slot, next_count))
            return sr_grass_prepare_failure(sr, slot, e, g, 512u, "expanded_geometry_unavailable");
        sr->grass_cache[slot].inst_count = next_count;
        sr->grass_cache[slot].param_hash = complete_hash;
        sr->grass_cache[slot].input_hash = ph;
        sr->grass_cache[slot].mask_content_hash = mask_content_hash;
        sr->grass_cache[slot].content_fs = content_fs;
        sr->grass_cache[slot].content_policy = content_policy;
        sr->grass_cache[slot].ready = true;
    }
    if (need_shadow && !expanded && sr->grass_cache[slot].inst_count &&
        !sr_grass_roots_ensure(sr, slot))
        return sr_grass_prepare_failure(sr, slot, e, g, 512u, "resident_roots_unavailable");
    return slot;
}
