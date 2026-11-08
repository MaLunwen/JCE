/* Per-call CPU identity on the central submission scope, never a GPU join. */
#ifndef JCE_RENDERER_SUBMISSION_TRACE_INTERNAL_H
#define JCE_RENDERER_SUBMISSION_TRACE_INTERNAL_H
#include "os/core/jce_trace_scope_internal.h"

/* The generation is retained identity from the still-owned renderer, including
 * teardown after upload unbind. It does not assert current backend availability.
 * Backend serial zero/wrap and ordinal zero are valid observations. Unknown or
 * non-int64-representable generations stay unavailable, as in snapshots. */
static inline void jce_renderer_submission_trace_emit(JceTraceScope scope,
    uint64_t context_generation, uint32_t backend_before,
    uint32_t backend_after, uint32_t ordinal_before)
{
    if (!scope.id) return;
#define SUBMISSION_COUNT(name_, value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, name_, (int64_t)(value_), scope.id, scope.id)
    SUBMISSION_COUNT("renderer.submit.context_generation",
        context_generation && context_generation <= (uint64_t)INT64_MAX
            ? (int64_t)context_generation : -1);
    SUBMISSION_COUNT("renderer.submit.backend_serial_before", backend_before);
    SUBMISSION_COUNT("renderer.submit.backend_serial_after", backend_after);
    SUBMISSION_COUNT("renderer.submit.ordinal_before", ordinal_before);
#undef SUBMISSION_COUNT
}

/* Keep these edges separate from the existing four-field identity packet. */
static inline void jce_renderer_submission_clock_emit(JceTraceScope scope,
                                                       uint64_t begin, uint64_t end)
{
    const JceTracePerfIntervalNames names = {
        "renderer.submit_clock.frequency", "renderer.submit_clock.begin",
        "renderer.submit_clock.end"
    };
    jce_trace_perf_interval_emit(scope, JCE_TRACE_CATEGORY_RENDER, names, begin, end);
}
#endif
