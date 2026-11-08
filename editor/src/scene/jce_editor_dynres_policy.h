/* Editor-only dynamic-resolution policy for distinct latched GPU samples.
 * No allocation, device ownership, frame join or GPU completion observation. */
#ifndef JCE_EDITOR_DYNRES_POLICY_H
#define JCE_EDITOR_DYNRES_POLICY_H

#include <jce/renderer/jce_renderer.h>
#include <cmath>

struct JceEditorDynresPolicy {
    float scale = 1.0f;
    int over = 0, under = 0;
    bool has_sample = false;
    uint64_t context_generation = 0;
    uint32_t source_frame = 0;
    int64_t raw_begin = 0, raw_end = 0, timer_frequency = 0;
};

static inline bool jce_editor_dynres_consume(JceEditorDynresPolicy *state,
                                             const JceGpuTimingObservation *sample)
{
    if (!state) return false;
    if (!sample || sample->version != 1 || sample->struct_size < sizeof(*sample) ||
        !sample->context_available || !sample->context_generation) {
        state->over = state->under = 0;
        return false;
    }
    if (state->context_generation != sample->context_generation) {
        state->over = state->under = 0;
        state->has_sample = false;
        state->context_generation = sample->context_generation;
    }
    if (!sample->backend_available || !sample->timer_available ||
        sample->unavailable_reason != JCE_GPU_TIMING_AVAILABLE ||
        sample->timer_frequency <= 0 || sample->raw_begin < 0 ||
        sample->raw_end <= sample->raw_begin || !std::isfinite(sample->duration_ms) ||
        sample->duration_ms <= 0.01) {
        state->over = state->under = 0;
        return false;
    }
    /* Read-time host frames are intentionally absent: repeated backend timer
     * samples must not manufacture sustained load when CPU frames advance. */
    if (state->has_sample &&
        state->context_generation == sample->context_generation &&
        state->source_frame == sample->gpu_source_frame &&
        state->raw_begin == sample->raw_begin && state->raw_end == sample->raw_end &&
        state->timer_frequency == sample->timer_frequency) return false;
    state->has_sample = true;
    state->source_frame = sample->gpu_source_frame;
    state->raw_begin = sample->raw_begin;
    state->raw_end = sample->raw_end;
    state->timer_frequency = sample->timer_frequency;

    const double target = 16.0;
    const double lo = target * 0.65;
    if (sample->duration_ms > target) {
        if (state->over < 3) ++state->over;
        state->under = 0;
    } else if (sample->duration_ms < lo) {
        if (state->under < 12) ++state->under;
        state->over = 0;
    } else {
        state->over = state->under = 0;
    }
    if (state->over >= 3 && state->scale > 0.551f) {
        state->scale -= 0.1f;
        state->over = 0;
    } else if (state->under >= 12 && state->scale < 0.999f) {
        state->scale += 0.1f;
        state->under = 0;
    }
    if (state->scale < 0.55f) state->scale = 0.55f;
    if (state->scale > 1.0f) state->scale = 1.0f;
    return true;
}

#endif
