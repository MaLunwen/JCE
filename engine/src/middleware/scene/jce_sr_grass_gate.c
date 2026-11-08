/* Existing colour request body, moved out of the oversized draw TU. */
#include "jce_sr_internal.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>

void sr_grass_draw_colour_request(JceSceneRenderer *sr, JceScene *scene,
    EntityList *list, JceEntity e, uint16_t view_id, const JceCamera *camera,
    const JceSceneRenderConfig *cfg, SrGrassGatePass *observation,
    struct SrGrassMaskBatch *mask_batch)
{
    const int s_no_grass = sr_grass_kill_enabled() ? 1 : 0;
    static int s_grass_cid = -2;
    if (s_grass_cid == -2) s_grass_cid = jce_component_find("GrassField");
    bool enabled = (s_grass_cid < 0) || jce_scene_comp_enabled(scene, e, s_grass_cid);
    const bool expanded = sr_grass_expanded_mode();
    bool caps_ok = expanded || (jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0;
    /* Preserve the MEDIUM floor and WebGL2/ANGLE LOW-tier exemption. The
     * project opt-in, not adapter-name heuristics, remains the real switch. */
    bool tier_ok = expanded || jce_renderer_get_tier() >= JCE_GPU_TIER_MEDIUM ||
        jce_renderer_get_active_backend() == JCE_BACKEND_OPENGLES;
    bool proj_ok = sr->grass_enabled;
    const uint32_t reasons = (s_no_grass ? 1u : 0u) | (!enabled ? 2u : 0u) |
        (!caps_ok ? 4u : 0u) | (!tier_ok ? 8u : 0u) | (!proj_ok ? 16u : 0u);
    const uint32_t first_reasons = reasons & ~sr->grass_gate_reported_reasons;
    if (first_reasons) {
        sr->grass_gate_reported_reasons |= first_reasons;
        const JceGrassFieldComponent *field = jce_scene_get_grass_field(scene, e);
        LOG_INFO(LOG_TAG,
            "grass admission rejected first_reason_bits=%u "
            "owner=%p view=%u entity=%llu enabled=%d caps=%d "
            "tier=%d tier_allowed=%d project=%d kill=%d "
            "program=%u program_tried=%d mask='%s'",
            first_reasons, (void *)sr, (unsigned)view_id, (unsigned long long)e,
            (int)enabled, (int)caps_ok, (int)jce_renderer_get_tier(), (int)tier_ok,
            (int)proj_ok, s_no_grass, (unsigned)sr->prog_grass.idx,
            (int)sr->grass_prog_tried, field ? field->density_mask_path : "");
    }
    const bool admitted = sr_grass_field_admitted(sr, scene, e);
    if (observation) sr_grass_gate_pass_add(observation, e, reasons, admitted);
    if (admitted) {
        const int _pp_g = jce_perf_phase_enabled();
        uint64_t _t0_gr = _pp_g ? jce_time_perf_counter() : 0u;
        sr_draw_grass(sr, scene, list, e, view_id, camera, cfg, mask_batch);
        if (_pp_g) jce_perf_phase_add("sr_grass",
            jce_time_perf_to_ms(_t0_gr, jce_time_perf_counter()));
    }
}
