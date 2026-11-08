#include "jce_renderer_display_adapter_internal.h"

/* ── GPU adapter selection ──────────────────────────────────────────────
 *
 * A laptop with both an integrated and a discrete GPU otherwise gets whatever
 * the driver hands out, which is not always what the user wants: the discrete
 * part is faster but costs battery and can be unavailable on a docked/eGPU
 * setup, and testing the low-end path requires *forcing* the integrated one.
 * bgfx picks by PCI vendor id at init, so the preference has to be resolved
 * BEFORE bgfx_init -- it cannot be changed afterwards without a full restart.
 *
 * Resolution order: explicit vendor id, then the JCE_GPU_ADAPTER preference,
 * then, for native D3D Auto, the DXGI high-performance hardware order.
 * Unavailable, unrepresentable or refused choices use the display and driver
 * fallback. Values: "auto" (default), "integrated", "discrete",
 * "intel", "nvidia", "amd", or a raw hex vendor id such as "0x10DE". */
static uint16_t adapter_vendor_from_name(const char *v)
{
    if (!v || !v[0]) return BGFX_PCI_ID_NONE;
    if (jce_strcasecmp(v, "auto") == 0)       return BGFX_PCI_ID_NONE;
    if (jce_strcasecmp(v, "integrated") == 0 ||
        jce_strcasecmp(v, "intel") == 0)      return BGFX_PCI_ID_INTEL;
    if (jce_strcasecmp(v, "nvidia") == 0)     return BGFX_PCI_ID_NVIDIA;
    if (jce_strcasecmp(v, "amd") == 0 ||
        jce_strcasecmp(v, "ati") == 0)        return BGFX_PCI_ID_AMD;
    /* "discrete": prefer NVIDIA, fall back to AMD if that adapter is absent.
     * bgfx has no "any discrete" selector, so this is a best-effort hint --
     * an unmatched vendor id makes bgfx fall back to the default adapter
     * rather than fail, which is the behaviour we want. */
    if (jce_strcasecmp(v, "discrete") == 0)   return BGFX_PCI_ID_NVIDIA;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        return (uint16_t)strtoul(v + 2, NULL, 16);
    LOG_WARN(LOG_TAG, "unknown GPU adapter preference '%s'; using auto", v);
    return BGFX_PCI_ID_NONE;
}

static uint16_t s_adapter_vendor_pref = BGFX_PCI_ID_NONE;
static bool     s_adapter_pref_read;

void jce_renderer_set_gpu_adapter_preference(const char *name)
{
    s_adapter_vendor_pref = adapter_vendor_from_name(name);
    s_adapter_pref_read   = true;
}

/* Stamp the resolved preference onto an init struct.  Called from EVERY
 * bgfx_init_ctor site: the fallback chain must honour the same choice as the
 * primary path, or a backend fallback would silently move the user to a
 * different GPU. */
static void apply_adapter_preference(bgfx_init_t *init)
{
    if (!s_adapter_pref_read) {
        jce_renderer_set_gpu_adapter_preference(getenv("JCE_GPU_ADAPTER"));
    }
    init->vendorId = s_adapter_vendor_pref;
    if (s_adapter_vendor_pref != BGFX_PCI_ID_NONE)
        LOG_INFO(LOG_TAG, "GPU adapter preference: vendor 0x%04X",
                 (unsigned)s_adapter_vendor_pref);
}

uint32_t jce_renderer_get_gpu_adapters(JceGpuAdapter *out, uint32_t cap)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps) return 0;
    const uint32_t n = caps->numGPUs < cap ? caps->numGPUs : cap;
    for (uint32_t i = 0; i < n && out; ++i) {
        out[i].vendor_id = caps->gpu[i].vendorId;
        out[i].device_id = caps->gpu[i].deviceId;
    }
    return caps->numGPUs;
}

/* Log what the driver actually offers.  Without this a user asking for a
 * specific GPU has no way to find out which ones exist, and no way to tell a
 * silently-ignored preference from an honoured one. */
static void log_gpu_adapters(void)
{
    /* The selected device identifies this run. Keep this startup observation
     * in DIST too; an enumerated or requested adapter is not the active one. */
    const bgfx_caps_t *selected = bgfx_get_caps();
    if (selected) {
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
            "GPU selected: vendor=0x%04X device=0x%04X adapter_request=0x%04X",
            (unsigned)selected->vendorId, (unsigned)selected->deviceId,
            (unsigned)s_adapter_vendor_pref);
    } else {
        jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
            "GPU selected identity unavailable: backend=%s",
            bgfx_get_renderer_name(bgfx_get_renderer_type()));
    }
    JceGpuAdapter list[8];
    const uint32_t total = jce_renderer_get_gpu_adapters(list, 8);
    if (total == 0) {
        LOG_INFO(LOG_TAG, "GPU adapters: backend does not enumerate");
        return;
    }
    const uint32_t shown = total < 8 ? total : 8;
    for (uint32_t i = 0; i < shown; ++i) {
        const char *vendor =
            list[i].vendor_id == BGFX_PCI_ID_INTEL  ? "Intel"  :
            list[i].vendor_id == BGFX_PCI_ID_NVIDIA ? "NVIDIA" :
            list[i].vendor_id == BGFX_PCI_ID_AMD    ? "AMD"    : "other";
        LOG_INFO(LOG_TAG,
                 "GPU adapter %u/%u: %s (vendor 0x%04X device 0x%04X)%s",
                 (unsigned)(i + 1), (unsigned)total, vendor,
                 (unsigned)list[i].vendor_id, (unsigned)list[i].device_id,
                 (s_adapter_vendor_pref != BGFX_PCI_ID_NONE &&
                  s_adapter_vendor_pref == list[i].vendor_id)
                     ? "  <- requested" : "");
    }
}

/* Exactly once after a successful create, or once after all attempts fail. */
static void log_display_adapter(JceWindow *window,const JceRendererDisplayAdapterState *state,
                                const bgfx_caps_t *selected)
{
    if (state->performance_queried) {
        const JceWindowAdapterObservation *preferred = &state->performance;
        const bool actual = state->performance_applied && selected &&
            selected->vendorId == preferred->vendor_id && selected->deviceId == preferred->device_id;
        jce_log_write(state->performance_applied && !actual ? JCE_LOG_LEVEL_WARN : JCE_LOG_LEVEL_INFO,
            LOG_TAG, __FILE__, __LINE__,
            "GPU render adapter: preference=dxgi_high_performance observed=0x%04X:0x%04X "
            "luid=%08X:%08X rank=%u index=%u selected=0x%04X:0x%04X "
            "reason=%s applied=%d exact_match=%d init_fallback=%d",
            (unsigned)preferred->vendor_id, (unsigned)preferred->device_id,
            (unsigned)(uint32_t)preferred->luid_high, (unsigned)preferred->luid_low,
            (unsigned)preferred->source_id, (unsigned)preferred->adapter_index,
            selected ? (unsigned)selected->vendorId : 0u, selected ? (unsigned)selected->deviceId : 0u,
            jce_window_adapter_reason_name(preferred->reason), state->performance_applied ? 1 : 0,
            actual ? 1 : 0, state->performance_blocked ? 1 : 0);
    }
    const bool matches=state->applied && selected &&
        selected->vendorId==state->effective_vendor && selected->deviceId==state->effective_device;
    const char *reason=state->queried ? jce_window_adapter_reason_name(state->observation.reason) : "not_queried";
    const JceLogLevel level=state->applied && !matches ? JCE_LOG_LEVEL_WARN : JCE_LOG_LEVEL_INFO;
    jce_log_write(level,LOG_TAG,__FILE__,__LINE__,
        "GPU display adapter: window=%p source=%s observed=0x%04X:0x%04X luid=%08X:%08X index=%u "
        "ccd_valid=%d ccd_luid=%08X:%08X ccd_source=%u ccd_paths=%u ccd_modes=%u "
        "adapter_count=%u luid_matches=%u key_matches=%u backend_expected_valid=%d "
        "backend_expected_luid=%08X:%08X backend_expected_index=%u "
        "policy=0x%04X requested=0x%04X:0x%04X selected=0x%04X:0x%04X reason=%s applied=%d exact_match=%d fallback=%s",
        (void *)window,state->queried ? "ccd_active_source" : "not_queried",
        (unsigned)state->observation.vendor_id,(unsigned)state->observation.device_id,
        (unsigned)(uint32_t)state->observation.luid_high,(unsigned)state->observation.luid_low,
        (unsigned)state->observation.adapter_index,state->observation.source_valid ? 1 : 0,
        (unsigned)(uint32_t)state->observation.source_luid_high,(unsigned)state->observation.source_luid_low,
        (unsigned)state->observation.source_id,(unsigned)state->observation.path_count,(unsigned)state->observation.mode_count,
        (unsigned)state->observation.adapter_count,(unsigned)state->observation.luid_match_count,(unsigned)state->observation.key_match_count,
        state->observation.backend_expected_valid ? 1 : 0,
        (unsigned)(uint32_t)state->observation.backend_expected_luid_high,(unsigned)state->observation.backend_expected_luid_low,
        (unsigned)state->observation.backend_expected_index,
        (unsigned)s_adapter_vendor_pref,(unsigned)state->effective_vendor,(unsigned)state->effective_device,
        selected ? (unsigned)selected->vendorId : 0u,selected ? (unsigned)selected->deviceId : 0u,
        reason,state->applied ? 1 : 0,matches ? 1 : 0,
        !selected ? "backend_init_failed" : (state->applied && !matches ? "backend_default" :
        (state->applied ? "none" : (state->performance_applied ? "performance_policy" : "original_policy"))));
    JceTraceScope scope=jce_trace_scope_begin("renderer.adapter.startup");
    if(scope.id) {
        if (state->performance_queried) {
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "renderer.adapter.performance_key",
                ((uint32_t)state->performance.vendor_id << 16) | state->performance.device_id, scope.id, scope.id);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "renderer.adapter.performance_reason",
                (int64_t)state->performance.reason, scope.id, scope.id);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "renderer.adapter.performance_applied",
                state->performance_applied, scope.id, scope.id);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "renderer.adapter.performance_blocked",
                state->performance_blocked, scope.id, scope.id);
        }
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.reason",
            state->queried ? (int)state->observation.reason : -1,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.observed",
            ((uint32_t)state->observation.vendor_id<<16)|state->observation.device_id,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.requested",
            ((uint32_t)state->effective_vendor<<16)|state->effective_device,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.selected",
            selected ? ((uint32_t)selected->vendorId<<16)|selected->deviceId : 0,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.exact_match",matches,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.luid_low",
            state->queried ? state->observation.luid_low : 0,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.luid_high",
            state->queried ? (uint32_t)state->observation.luid_high : 0,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.index",
            state->queried && state->observation.reason==JCE_WINDOW_ADAPTER_MATCH ?
            (int64_t)state->observation.adapter_index : -1,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.source_valid",
            state->observation.source_valid,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.source_luid_low",
            state->observation.source_luid_low,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.source_luid_high",
            (uint32_t)state->observation.source_luid_high,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.source_id",
            state->observation.source_valid ? (int64_t)state->observation.source_id : -1,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.path_count",
            state->observation.path_count,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.mode_count",
            state->observation.mode_count,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.adapter_count",
            state->observation.adapter_count,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.luid_matches",
            state->observation.luid_match_count,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.key_matches",
            state->observation.key_match_count,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.backend_expected_valid",
            state->observation.backend_expected_valid,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.backend_expected_luid_low",
            state->observation.backend_expected_luid_low,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.backend_expected_luid_high",
            (uint32_t)state->observation.backend_expected_luid_high,scope.id,scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,"renderer.adapter.backend_expected_index",
            state->observation.backend_expected_valid ? (int64_t)state->observation.backend_expected_index : -1,scope.id,scope.id);
    }
    jce_trace_scope_end(scope,selected ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
}
