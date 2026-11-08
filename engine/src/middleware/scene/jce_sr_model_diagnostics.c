/* Diagnostics consume the existing model slots, never a second registry. */
#include "jce_sr_internal.h"
#include "jce_sr_model_diagnostics.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"

static const char *sr_model_failure_name(JceSrModelFailure reason)
{
    switch (reason) {
    case SR_MODEL_FAILURE_LOAD: return "load_failed";
    case SR_MODEL_FAILURE_ENTRY_BUDGET: return "entry_budget";
    case SR_MODEL_FAILURE_NO_ARCHIVE: return "archive_unavailable";
    case SR_MODEL_FAILURE_JOB_ALLOCATION: return "job_allocation";
    case SR_MODEL_FAILURE_TASK_ADMISSION: return "task_admission";
    case SR_MODEL_FAILURE_DECODE: return "decode_failed";
    case SR_MODEL_FAILURE_UPLOAD: return "upload_failed";
    case SR_MODEL_FAILURE_PATH_LENGTH: return "path_length";
    case SR_MODEL_FAILURE_ENTRY_ALLOCATION: return "entry_allocation";
    default: return "unknown";
    }
}

static const char *sr_model_retry_name(JceSrModelFailure reason)
{
    switch (reason) {
    case SR_MODEL_FAILURE_LOAD:
    case SR_MODEL_FAILURE_DECODE:
    case SR_MODEL_FAILURE_UPLOAD: return "cache_reset";
    case SR_MODEL_FAILURE_ENTRY_BUDGET: return "free_slot_or_cache_reset";
    case SR_MODEL_FAILURE_NO_ARCHIVE: return "bind_archive";
    case SR_MODEL_FAILURE_PATH_LENGTH: return "correct_path";
    case SR_MODEL_FAILURE_ENTRY_ALLOCATION: return "retry_after_memory_recovery";
    default: return "next_request";
    }
}

static void sr_model_count(JceTraceScope scope, const char *name, uint64_t value)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, name,
        (int64_t)value, scope.id, scope.id);
}
#define MODEL_COUNT(name_, value_) sr_model_count(scope, name_, (uint64_t)(value_))
#define MODEL_WIDE(name_, value_) do { \
    const uint64_t value_bits_ = (uint64_t)(value_); \
    MODEL_COUNT(name_ "_lo", (uint32_t)value_bits_); \
    MODEL_COUNT(name_ "_hi", (uint32_t)(value_bits_ >> 32u)); \
} while (0)

static void sr_model_identity(JceTraceScope scope, const JceSceneRenderer *sr,
    const SrModelFailureIdentity *identity)
{
    MODEL_COUNT("model.cache.owner", (uintptr_t)sr);
    MODEL_COUNT("model.cache.generation", identity->cache_generation);
    MODEL_COUNT("model.cache.capture_generation", identity->capture_generation);
    MODEL_COUNT("model.cache.published_capture", jce_trace_capture_generation());
    MODEL_COUNT("model.cache.context_available", identity->bound);
    MODEL_WIDE("model.cache.scene", (uintptr_t)identity->scene);
    MODEL_WIDE("model.cache.context", identity->context);
    MODEL_COUNT("model.cache.backend_cpu_frame", identity->frame);
}

static bool sr_model_flush_pending(JceSceneRenderer *sr, SrModelDiagnosticsFlush reason)
{
    SrModelDiagnostics *state = &sr->model_diagnostics;
    for (unsigned i = 0; i < SR_MODEL_FAILURE_REASONS; ++i) {
        SrModelFailureCounts *counts = &state->reasons[i];
        if (!counts->pending) continue;
        JceTraceScope scope = jce_trace_scope_begin("sr.model.cache.reject_summary");
        if (!scope.id) return false;
        MODEL_COUNT("model.cache.schema", 1);
        sr_model_identity(scope, sr, &state->batch);
        MODEL_COUNT("model.cache.reason", i + 1u);
        MODEL_COUNT("model.cache.flush_reason", reason);
        MODEL_WIDE("model.cache.requests", counts->pending);
        MODEL_WIDE("model.cache.total", counts->total);
        MODEL_COUNT("model.cache.first_entity", counts->first_entity);
        MODEL_COUNT("model.cache.last_entity", counts->last_entity);
        MODEL_COUNT("model.cache.first_path_hash", counts->first_path_hash);
        MODEL_COUNT("model.cache.last_path_hash", counts->last_path_hash);
        MODEL_COUNT("model.cache.count_saturated", counts->saturated);
        MODEL_COUNT("model.cache.count_complete", !counts->saturated);
        MODEL_COUNT("model.cache.current_capture",
            state->batch.capture_generation == jce_trace_capture_generation());
        MODEL_COUNT("model.cache.per_request_details", 0);
        MODEL_COUNT("model.cache.detail_budget", 1);
        jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
        counts->pending = 0;
    }
    return true;
}

/* Bindings describe CPU admission. They are not GPU-completion observations.
 * Old unpublished batches keep their original capture/context/cache identity. */
static bool sr_model_trace_sync(JceSceneRenderer *sr)
{
    if (!jce_trace_enabled()) return false;
    SrModelDiagnostics *state = &sr->model_diagnostics;
    SrModelFailureIdentity current = state->current;
    current.cache_generation = sr->model_cache_generation;
    current.capture_generation = jce_trace_capture_generation();
    const bool capture_changed = state->batch_valid &&
        current.capture_generation != state->batch.capture_generation;
    const bool identity_changed = !state->batch_valid || capture_changed ||
        current.cache_generation != state->batch.cache_generation ||
        current.scene != state->batch.scene || current.context != state->batch.context ||
        current.bound != state->batch.bound;
    const bool round_changed = state->batch_valid && current.frame != state->batch.frame;
    if (!identity_changed && !round_changed) return true;
    if ((identity_changed || round_changed) && state->batch_valid &&
        !sr_model_flush_pending(sr, capture_changed ? SR_MODEL_DIAGNOSTICS_CAPTURE :
            identity_changed ? SR_MODEL_DIAGNOSTICS_IDENTITY : SR_MODEL_DIAGNOSTICS_ROUND))
        return false;
    if (identity_changed) {
        memset(state->reasons, 0, sizeof(state->reasons));
        state->detailed_reasons = 0;
    }
    state->batch = current;
    state->batch_valid = true;
    return true;
}

void sr_model_cache_diagnostics_bind(JceSceneRenderer *sr, const void *scene,
    uint64_t context, uint32_t backend_frame)
{
    if (!sr) return;
    SrModelDiagnostics *state = &sr->model_diagnostics;
    if (!jce_trace_enabled() && !state->batch_valid) return;
    state->current.scene = scene;
    state->current.context = context;
    state->current.frame = backend_frame;
    state->current.bound = scene != NULL && context != 0;
    (void)sr_model_trace_sync(sr);
}

bool sr_model_cache_diagnostics_flush(JceSceneRenderer *sr, SrModelDiagnosticsFlush reason)
{
    if (!sr || !sr->model_diagnostics.batch_valid) return true;
    bool pending = false;
    for (unsigned i = 0; i < SR_MODEL_FAILURE_REASONS; ++i)
        pending |= sr->model_diagnostics.reasons[i].pending != 0;
    if (!pending) return true;
    return sr_model_trace_sync(sr) && sr_model_flush_pending(sr, reason);
}

void sr_model_cache_failure_hash(JceSceneRenderer *sr, uint32_t path_hash,
    uint32_t entity, JceSrModelFailure reason)
{
    if (!sr || reason < SR_MODEL_FAILURE_LOAD ||
        reason > SR_MODEL_FAILURE_ENTRY_ALLOCATION) return;
    const uint32_t bit = 1u << (unsigned)reason;
    const bool first = !(sr->model_cache_reported_reasons & bit);
    if (!first && !jce_trace_enabled()) return;
    const bool trace_ready = sr_model_trace_sync(sr);
    const bool first_detail = trace_ready && !(sr->model_diagnostics.detailed_reasons & bit);
    uint32_t loaded = 0, pending = 0, failed = 0, unavailable = 0;
    if (first || first_detail) for (uint32_t i = 0; i < sr->model_cache_capacity; ++i) {
        const SrModelCache *entry = sr->model_cache[i];
        if (!entry) continue;
        if (entry->pending) ++pending;
        else if (entry->failed) ++failed;
        else if (entry->model) ++loaded;
        else ++unavailable;
    }
    if (first) {
        sr->model_cache_reported_reasons |= bit;
        LOG_WARN(LOG_TAG, "model cache rejected first_reason=%s owner=%p "
            "entity=%u path_hash=%08x generation=%llu occupied=%u limit=%u "
            "loaded=%u pending=%u failed=%u unavailable=%u retry=%s",
            sr_model_failure_name(reason), (void *)sr, (unsigned)entity,
            (unsigned)path_hash, (unsigned long long)sr->model_cache_generation,
            (unsigned)(loaded + pending + failed + unavailable),
            (unsigned)sr->model_cache_capacity, (unsigned)loaded, (unsigned)pending,
            (unsigned)failed, (unsigned)unavailable, sr_model_retry_name(reason));
    }
    if (!trace_ready) return;
    SrModelFailureCounts *counts = &sr->model_diagnostics.reasons[(unsigned)reason - 1u];
    if (!counts->pending) {
        counts->first_entity = entity;
        counts->first_path_hash = path_hash;
    }
    counts->last_entity = entity;
    counts->last_path_hash = path_hash;
    if (counts->pending == UINT64_MAX || counts->total == UINT64_MAX)
        counts->saturated = true;
    if (counts->pending != UINT64_MAX) ++counts->pending;
    if (counts->total != UINT64_MAX) ++counts->total;
    if (!first_detail) return;
    JceTraceScope scope = jce_trace_scope_begin("sr.model.cache.reject");
    if (!scope.id) return;
    MODEL_COUNT("model.cache.schema", 2);
    sr_model_identity(scope, sr, &sr->model_diagnostics.batch);
    MODEL_COUNT("model.cache.entity", entity);
    MODEL_COUNT("model.cache.path_hash", path_hash);
    MODEL_COUNT("model.cache.reason", reason);
    MODEL_COUNT("model.cache.occupied", loaded + pending + failed + unavailable);
    MODEL_COUNT("model.cache.limit", sr->model_cache_capacity);
    MODEL_COUNT("model.cache.max_limit", SR_MODEL_CACHE_MAX);
    MODEL_COUNT("model.cache.loaded", loaded);
    MODEL_COUNT("model.cache.pending", pending);
    MODEL_COUNT("model.cache.failed", failed);
    MODEL_COUNT("model.cache.unavailable", unavailable);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
    sr->model_diagnostics.detailed_reasons |= bit;
}

void sr_model_cache_failure(JceSceneRenderer *sr, const char *path,
    uint32_t entity, JceSrModelFailure reason)
{
    if (!sr || reason < SR_MODEL_FAILURE_LOAD ||
        reason > SR_MODEL_FAILURE_ENTRY_ALLOCATION) return;
    const uint32_t bit = 1u << (unsigned)reason;
    if ((sr->model_cache_reported_reasons & bit) && !jce_trace_enabled()) return;
    sr_model_cache_failure_hash(sr, path ? jce_fnv1a32_str(path) : 0, entity, reason);
}

void sr_model_cache_diagnostics_reset(JceSceneRenderer *sr)
{
    if (!sr) return;
    (void)sr_model_cache_diagnostics_flush(sr, SR_MODEL_DIAGNOSTICS_RESET);
    ++sr->model_cache_generation;
    sr->model_cache_reported_reasons = 0;
    sr->model_lookup_hint = NULL;
    sr->model_upload_frame_valid = false;
}
#undef MODEL_WIDE
#undef MODEL_COUNT
