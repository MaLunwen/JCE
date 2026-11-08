/* Same renderer init authority, bounded per-create observation across fallback. */
#ifndef JCE_RENDERER_DISPLAY_ADAPTER_INTERNAL_H
#define JCE_RENDERER_DISPLAY_ADAPTER_INTERNAL_H
#include "os/platform/jce_window_adapter_internal.h"
#include <bgfx/c99/bgfx.h>
typedef struct JceRendererDisplayAdapterState {
    bool queried,applied;
    uint16_t effective_vendor,effective_device;
    JceWindowAdapterObservation observation;
    bool performance_queried, performance_applied, performance_blocked;
    JceWindowAdapterObservation performance;
} JceRendererDisplayAdapterState;
static inline bool jce_renderer_adapter_key_usable(JceWindowAdapterObservation *candidate)
{
    if (candidate->reason != JCE_WINDOW_ADAPTER_MATCH) return false;
    /* Pinned bgfx searches only the actual Caps::gpu array. */
    if (candidate->adapter_index >= sizeof(((bgfx_caps_t *)0)->gpu) / sizeof(bgfx_caps_gpu_t)) {
        candidate->backend_expected_valid = false;
        candidate->reason = JCE_WINDOW_ADAPTER_BACKEND_LIMIT;
        return false;
    }
    if (!candidate->vendor_id || !candidate->device_id) {
        candidate->reason = JCE_WINDOW_ADAPTER_UNREPRESENTABLE;
        return false;
    }
    return true;
}

static inline bool jce_renderer_apply_display_adapter(bgfx_init_t *init,JceWindow *window,
    JceRendererDisplayAdapterState *state,
    bool (*observe)(JceWindow *,JceWindowAdapterObservation *),
    bool (*observe_performance)(JceWindow *,JceWindowAdapterObservation *))
{
    if(!init || !state) return false;
    state->effective_vendor=init->vendorId;state->effective_device=init->deviceId;state->applied=false;
    state->performance_applied = false;
    if(init->type!=BGFX_RENDERER_TYPE_DIRECT3D11 && init->type!=BGFX_RENDERER_TYPE_DIRECT3D12) return false;
    if(!state->queried) {
        state->queried=true;state->observation.reason=JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
        if(!observe || !observe(window,&state->observation)) {
            if(state->observation.reason==JCE_WINDOW_ADAPTER_MATCH)
                state->observation.reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;
        }
    }
    /* Window topology is diagnostic even when the caller owns GPU selection. */
    if(init->vendorId!=BGFX_PCI_ID_NONE || init->deviceId!=0) return false;
    if (window && observe_performance && !state->performance_blocked) {
        if (!state->performance_queried) {
            state->performance_queried = true;
            state->performance.reason = JCE_WINDOW_ADAPTER_INVALID_ARGUMENT;
            if (!observe_performance(window, &state->performance) &&
                state->performance.reason == JCE_WINDOW_ADAPTER_MATCH)
                state->performance.reason = JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;
        }
        if (jce_renderer_adapter_key_usable(&state->performance)) {
            init->vendorId = state->performance.vendor_id;
            init->deviceId = state->performance.device_id;
            state->effective_vendor = init->vendorId;
            state->effective_device = init->deviceId;
            state->performance_applied = true;
            return true;
        }
    }
    if (!jce_renderer_adapter_key_usable(&state->observation)) return false;
    init->vendorId=state->observation.vendor_id;
    init->deviceId=state->observation.device_id;
    state->effective_vendor=init->vendorId;state->effective_device=init->deviceId;state->applied=true;
    return true;
}
#endif
