/* CPU reset invocation, not backend application, presentation or GPU completion.
 * Included after the renderer owner's private bgfx declarations. */
#ifndef JCE_RENDERER_RESET_TRACE_INTERNAL_H
#define JCE_RENDERER_RESET_TRACE_INTERNAL_H
#include "os/core/jce_trace_scope_internal.h"

typedef enum JceRendererResetReason {
    JCE_RENDERER_RESET_RESIZE = 1,
    JCE_RENDERER_RESET_REBIND = 2,
    JCE_RENDERER_RESET_VSYNC = 3,
    JCE_RENDERER_RESET_MSAA = 4
} JceRendererResetReason;

static inline void renderer_reset_cpu(JceRendererResetReason reason,
    uint32_t requested_width, uint32_t requested_height,
    uint32_t call_width, uint32_t call_height, uint32_t flags)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.reset_cpu");
    if (scope.id) {
#define RESET_DETAIL(name_, value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, "renderer.reset." name_, (int64_t)(value_), scope.id, scope.parent)
        RESET_DETAIL("reason", reason);
        RESET_DETAIL("requested_width", requested_width);
        RESET_DETAIL("requested_height", requested_height);
        /* Known raw arguments can be zero (including vsync fallback sentinels).
         * MSAA has no requested extent; its zeros are unavailable placeholders. */
        RESET_DETAIL("requested_extent_known", reason == JCE_RENDERER_RESET_RESIZE ||
            reason == JCE_RENDERER_RESET_REBIND || reason == JCE_RENDERER_RESET_VSYNC);
        /* These are exactly the arguments passed below, not acknowledged size. */
        RESET_DETAIL("call_width", call_width);
        RESET_DETAIL("call_height", call_height);
        RESET_DETAIL("call_flags", flags);
        RESET_DETAIL("backend_result_known", 0);
        RESET_DETAIL("gpu_completion_known", 0);
    }
    bgfx_reset(call_width, call_height, flags, BGFX_TEXTURE_FORMAT_COUNT);
    if (scope.id) {
        RESET_DETAIL("call_returned", 1);
#undef RESET_DETAIL
    }
    /* The void call returned. No driver/backend/GPU outcome was observed. */
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}
#endif
