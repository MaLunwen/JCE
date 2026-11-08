/* Main-thread capture orchestration; physical IO belongs to the writer. */
#include "jce_engine_kpi_capture.h"
#include "jce_engine_kpi_clock.h"
#include <jce/os/core/jce_log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "engine"

static bool capture_busy(JceEngineKpiCapture *capture)
{
    if (!capture || !capture->writer) return false;
    JceFrameKpiWriterStatus status;
    jce_frame_kpi_writer_get_status(capture->writer, &status);
    return status.owner_operation_active;
}

bool jce_engine_kpi_capture_open(JceEngineKpiCapture *capture, const char *path,
    JceAsyncExecutor *executor, bool low_memory, uint32_t limit,
    uint32_t durable_interval)
{
    return jce_engine_kpi_capture_open_with_context(capture, path, executor,
        low_memory, limit, durable_interval, NULL);
}

bool jce_engine_kpi_capture_open_with_context(JceEngineKpiCapture *capture, const char *path,
    JceAsyncExecutor *executor, bool low_memory, uint32_t limit,
    uint32_t durable_interval, const char *context_selector)
{
    if (!capture || capture->writer || capture->failed || capture->reported)
        return false;
    if (!path || !path[0]) return true;
    if (context_selector && context_selector[0] && strcmp(context_selector, "0") &&
        strcmp(context_selector, "1") && strcmp(context_selector, "2") && strcmp(context_selector, "3")) {
        capture->failed = true;
        capture->failure_reason = "unknown JCE_FRAME_LOG_CONTEXT selector; expected 0, 1, 2, 3 or empty";
        LOG_ERROR(LOG_TAG, "%s", capture->failure_reason);
        return false;
    }
    capture->context_version = context_selector && strcmp(context_selector, "3") == 0 ? 3u :
        context_selector && strcmp(context_selector, "2") == 0 ? 2u :
        context_selector && strcmp(context_selector, "1") == 0 ? 1u : 0u;
    capture->context_enabled = capture->context_version != 0;
    /* Resolve once for this requested session, never in the export loop. */
    const char *backend_selector = getenv("JCE_KPI_BACKEND_TIMINGS");
    if (backend_selector && backend_selector[0] && strcmp(backend_selector, "0") &&
        strcmp(backend_selector, "1")) {
        capture->failed = true;
        capture->failure_reason = "unknown JCE_KPI_BACKEND_TIMINGS selector; expected 0, 1 or empty";
        LOG_ERROR(LOG_TAG, "%s", capture->failure_reason);
        return false;
    }
    capture->buffer.backend_timings = backend_selector && strcmp(backend_selector, "1") == 0;
    JceFrameKpiWriterConfig config;
    jce_frame_kpi_writer_config_init(&config, low_memory);
    if (!jce_engine_kpi_capture_queue_configure(&config, getenv("JCE_KPI_WRITER_QUEUE"))) {
        capture->failed = true;
        capture->failure_reason = "invalid JCE_KPI_WRITER_QUEUE; expected positive decimal bytes,records within 67108864,65536";
        LOG_ERROR(LOG_TAG, "%s", capture->failure_reason);
        return false;
    }
    SDL_IOStream *stream = SDL_IOFromFile(path, "w");
    if (!stream) {
        capture->failed = true;
        capture->failure_reason = "cannot open requested frame KPI stream";
        return false;
    }
    config.enabled = true;
    config.stream = stream;
    config.executor = executor;
    config.durable_interval = durable_interval;
    /* Conservative opt-in dispatch budget; existing polls observe oldest age.
     * This reduces small writes, not a guarantee against blocking storage. */
    config.min_batch_bytes = 16384u;
    config.max_delay_ms = 8u;
    config.context_json = capture->context_enabled;
    capture->writer = jce_frame_kpi_writer_create(&config);
    if (!capture->writer) {
        SDL_CloseIO(stream); /* NULL create did not take stream ownership. */
        capture->failed = true;
        capture->failure_reason = "cannot initialize bounded frame KPI writer";
        return false;
    }
    capture->limit = limit;
    if (!jce_engine_kpi_capture_poll(capture)) return false;
    jce_perf_phase_set_enabled(1);
    JceFrameKpiWriterStatus status;
    jce_frame_kpi_writer_get_status(capture->writer, &status);
    /* Queue and reusable scratch have separate finite allocation budgets. */
    capture->buffer.max_capacity = status.byte_capacity;
    LOG_INFO(LOG_TAG, "frame KPI capture enabled -> %s", path);
    LOG_INFO(LOG_TAG, "frame KPI writer mode=%s capacity=%llu records=%u",
        status.off_thread ? "threaded" : "cooperative",
        (unsigned long long)status.byte_capacity, status.record_capacity);
    LOG_INFO(LOG_TAG, "frame KPI durable flush interval=%u (0=shutdown only)",
        durable_interval);
    return true;
}

bool jce_engine_kpi_capture_context_active(const JceEngineKpiCapture *capture)
{
    return capture && capture->context_enabled && capture->writer && !capture->failed &&
        !capture->reported && (!capture->limit || capture->index < capture->limit);
}

bool jce_engine_kpi_capture_interval(JceEngineKpiCapture *capture,
    uint64_t frequency, uint64_t begin, uint64_t end)
{
    if (!capture || capture->failed) return false;
    if (capture->context_version != 3 || !jce_engine_kpi_capture_context_active(capture))
        return true;
    if (capture_busy(capture)) return false; /* Rejected reentry changes no data state. */
    capture->context.clock_valid = false;
    double wall_ms;
    if (!jce_engine_frame_clock_wall_ms(frequency, begin, end, &wall_ms)) {
        capture->failed = true;
        capture->failure_reason = "invalid completed frame KPI clock interval";
        return false;
    }
    capture->context.clock_frequency = frequency;
    capture->context.clock_begin = begin;
    capture->context.clock_end = end;
    capture->context.clock_valid = true;
    return true;
}

bool jce_engine_kpi_capture_poll(JceEngineKpiCapture *capture)
{
    if (!capture || capture->failed) return false;
    if (capture_busy(capture)) return false; /* Rejected reentry changes no data state. */
    if (capture->writer && !jce_frame_kpi_writer_poll(capture->writer)) {
        capture->failed = true;
        capture->failure_reason = "frame KPI writer failed";
    }
    return !capture->failed;
}

bool jce_engine_kpi_capture_export(JceEngineKpiCapture *capture,
    double wall_ms, bool phases_eligible)
{
    if (!capture || capture->failed) return false;
    if (capture_busy(capture)) return false; /* Rejected reentry changes no data state. */
    if (!capture->writer || (capture->limit && capture->index >= capture->limit))
        return true;
    if (capture->context_enabled && !capture->context_sealed) {
        capture->failed = true;
        capture->failure_reason = "completed frame KPI context is not sealed";
        return false;
    }
    JceFrameKpiScope whole = jce_frame_kpi_scope_begin("kpi_export");
    JceFrameKpiScope stats_scope = jce_frame_kpi_scope_begin("kpi_stats");
    JceGpuStats stats;
    const bool have_stats = jce_renderer_get_gpu_stats(&stats) && stats.valid;
    const double stats_ms = jce_frame_kpi_scope_end(stats_scope, true);
    size_t size = 0;
    bool succeeded = jce_frame_kpi_encode_with_context(&capture->buffer, capture->index,
        wall_ms, have_stats ? &stats : NULL, phases_eligible,
        capture->context_enabled ? &capture->context : NULL, &size);
    /* Preserve the entered getter even when encoding/submission fails. */
    capture->buffer.last_profile.stats_calls = stats_scope.sampled ? 1u : 0u;
    capture->buffer.last_profile.stats_ms = stats_ms;
    if (succeeded) {
        JceFrameKpiScope enqueue = jce_frame_kpi_scope_begin("kpi_enqueue");
        succeeded = jce_frame_kpi_writer_submit(capture->writer, capture->index,
                                               capture->buffer.bytes, size);
        jce_frame_kpi_scope_end(enqueue, succeeded);
        if (!succeeded) capture->failure_reason = "frame KPI submission failed";
    } else {
        capture->failure_reason = "frame KPI snapshot encoding failed";
    }
    if (succeeded) {
        ++capture->index;
        if (capture->context_enabled) capture->context_sealed = false;
    }
    else capture->failed = true;
    jce_frame_kpi_scope_end(whole, succeeded);
    return succeeded;
}

bool jce_engine_kpi_capture_finish(JceEngineKpiCapture *capture)
{
    if (!capture) return true;
    /* A callback cannot recursively join itself or close an active stream.
     * Deferral is not evidence that the capture data itself failed. */
    if (capture_busy(capture)) return false;
    bool succeeded = !capture->failed;
    JceFrameKpiWriterStatus status = {0};
    if (capture->writer) {
        if (!jce_frame_kpi_writer_finish(capture->writer)) succeeded = false;
        jce_frame_kpi_writer_get_status(capture->writer, &status);
        if (status.accepted_records != capture->index ||
            status.accepted_records != status.written_records ||
            status.accepted_bytes != status.written_bytes || !status.closed ||
            status.state != JCE_KPI_WRITER_COMPLETE) succeeded = false;
    }
    capture->failed = !succeeded;
    if (!capture->reported && (capture->writer || capture->failed)) {
        if (!succeeded) {
            const JceFrameKpiWriterFailure *first = &status.first_failure;
            LOG_ERROR(LOG_TAG, "frame KPI capture incomplete: %s; writer_error=%u %s "
                "accepted=%llu written=%llu accepted_bytes=%llu written_bytes=%llu",
                capture->failure_reason ? capture->failure_reason : "finalization failed",
                (unsigned)status.error, status.message,
                (unsigned long long)status.accepted_records,
                (unsigned long long)status.written_records,
                (unsigned long long)status.accepted_bytes,
                (unsigned long long)status.written_bytes);
            /* Separate finite records retain all fields within the logger's
             * existing 512-byte message cap. Emit once, only after real failure. */
            if (first->captured) {
                const JceFrameKpiWriterIoObservation *returned = first->write.return_available
                    ? &first->write : &status.failure_write_return;
                LOG_ERROR(LOG_TAG, "frame KPI first failure: ns=%llu queued_bytes=%llu "
                    "queued_records=%u capacity=%llu record_capacity=%u "
                    "task_state_available=%u task_state=%u task_submitted=%llu "
                    "task_started=%llu task_completed=%llu write_calls=%llu "
                    "failed_requested=%llu clock=process-monotonic-ns",
                    (unsigned long long)first->observed_ns,
                    (unsigned long long)first->queued_bytes, first->queued_records,
                    (unsigned long long)status.byte_capacity, status.record_capacity,
                    (unsigned)first->task_state_available, (unsigned)first->task_state,
                    (unsigned long long)first->submitted_tasks,
                    (unsigned long long)first->started_tasks,
                    (unsigned long long)first->completion_callbacks,
                    (unsigned long long)first->write_calls,
                    (unsigned long long)status.failed_requested_bytes);
                LOG_ERROR(LOG_TAG, "frame KPI failed write: serial=%llu active=%u data=%u "
                    "start_ns=%llu first_return_available=%u return_published_later=%u "
                    "return_available=%u return_ns=%llu requested=%llu "
                    "actual=%llu status=%d",
                    (unsigned long long)first->write.serial, (unsigned)first->write.active,
                    (unsigned)first->write.data, (unsigned long long)first->write.started_ns,
                    (unsigned)first->write.return_available,
                    (unsigned)status.failure_write_return.return_available,
                    (unsigned)returned->return_available,
                    (unsigned long long)returned->returned_ns,
                    (unsigned long long)first->write.requested_bytes,
                    (unsigned long long)returned->actual_bytes,
                    returned->return_available ? (int)returned->io_status : -1);
            }
        } else {
            jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                "frame KPI capture complete: accepted=%llu written=%llu "
                "accepted_bytes=%llu written_bytes=%llu peak_bytes=%llu peak_records=%u "
                "capacity_bytes=%llu capacity_records=%u",
                (unsigned long long)status.accepted_records,
                (unsigned long long)status.written_records,
                (unsigned long long)status.accepted_bytes,
                (unsigned long long)status.written_bytes,
                (unsigned long long)status.peak_bytes, status.peak_records,
                (unsigned long long)status.byte_capacity, status.record_capacity);
        }
        capture->reported = true;
    }
    return succeeded;
}

JceAppResult jce_engine_kpi_capture_result(JceEngineKpiCapture *capture,
                                          JceAppResult result)
{
    if (result != JCE_APP_CONTINUE && !jce_engine_kpi_capture_finish(capture))
        return JCE_APP_FAILURE;
    return result;
}

bool jce_engine_kpi_capture_destroy(JceEngineKpiCapture *capture)
{
    if (!capture) return true;
    if (capture_busy(capture)) return false;
    jce_engine_kpi_capture_finish(capture);
    if (capture->writer && !jce_frame_kpi_writer_destroy(capture->writer))
        return false; /* Ownership remains intact for the owner to retry. */
    jce_frame_kpi_buffer_destroy(&capture->buffer);
    memset(capture, 0, sizeof *capture);
    return true;
}
