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
    /* resident_blades is inst_vb_count.  0 means no persistent instance
     * buffer, which is what LOW/MED tiers and JCE_PERSIST_GRASS=0 produce --
     * they degrade to no grass shadows with the colour pass untouched, the
     * same degrade the vegetation scatter takes. */
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

/* ── the submit half ─────────────────────────────────────────────────── */

#include "jce_sr_internal.h"
#include <jce/middleware/scene/jce_component_registry.h>

#include <bgfx/c99/bgfx.h>

void sr_submit_grass_shadows(struct JceSceneRenderer *sr_, struct JceScene *scene_,
                             uint16_t cv, uint16_t shadow_inst_idx,
                             const jce_vec4 *planes, jce_vec3 cam)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)sr_;
    JceScene *scene = (JceScene *)scene_;
    if (!sr || !scene || !planes || shadow_inst_idx == UINT16_MAX) return;

    const bgfx_program_handle_t prg = { shadow_inst_idx };
    /* Depth-only, matching sr_submit_scatter_shadows exactly.  Grass blades
     * are two-sided cards, so CULL_NONE rather than CULL_CW: culling them
     * would drop half of every blade's shadow depending on which way the
     * cascade happens to look at the field. */
    const uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                         | BGFX_STATE_MSAA;

    for (int slot = 0; slot < (int)(sizeof sr->grass_cache /
                                    sizeof sr->grass_cache[0]); ++slot) {
        if (!sr->grass_cache[slot].used) continue;
        const JceEntity e = sr->grass_cache[slot].entity;
        JceGrassFieldComponent *g = jce_scene_get_grass_field(scene, e);
        if (!g) continue;

        /* GrassField carries no legacy JCE_COMP_FLAG_* bit -- the 64-bit space
         * filled before it existed -- so its enable state is id-keyed through
         * the registry, resolved once. */
        static int s_grass_cid = -2;
        if (s_grass_cid == -2) s_grass_cid = jce_component_find("GrassField");
        const bool enabled = s_grass_cid < 0 ||
                             jce_scene_comp_enabled(scene, e, s_grass_cid);
        if (!jce_grass_shadow_field_casts(g, enabled, g->visible,
                                          sr->grass_cache[slot].inst_vb_count))
            continue;

        JceMesh *blade = sr->grass_cache[slot].blade
                       ? sr->grass_cache[slot].blade : sr->grass_blade;
        if (!blade) continue;

        JceGrassShadowRun runs[64];
        const uint32_t n = jce_grass_shadow_runs(
            sr->grass_cache[slot].cell_start,
            sr->grass_cache[slot].grid_gx, sr->grass_cache[slot].grid_gz,
            sr->grass_cache[slot].grid_min_x, sr->grass_cache[slot].grid_min_z,
            sr->grass_cache[slot].cell_size,
            sr->grass_cache[slot].field_ymin, sr->grass_cache[slot].field_ymax,
            planes, cam, g->fade_end,
            runs, (uint32_t)(sizeof runs / sizeof runs[0]));

        const bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(blade) };
        const bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(blade) };
        for (uint32_t i = 0; i < n; ++i) {
            if (runs[i].count == 0u) continue;
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (ibh.idx != UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(blade));
            bgfx_set_instance_data_from_dynamic_vertex_buffer(
                sr->grass_cache[slot].inst_vb, runs[i].start, runs[i].count);
            bgfx_set_state(state, 0);
            bgfx_submit(cv, prg, 0, BGFX_DISCARD_ALL);
        }
    }
}
