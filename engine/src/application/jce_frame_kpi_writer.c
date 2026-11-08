/* Optional bounded CSV transport over the existing structured executor. */
#include "jce_frame_kpi_writer.h"
#include "jce_frame_kpi.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <string.h>

typedef struct { size_t offset, length; uint64_t enqueued_ns; } KpiRecord;
struct JceFrameKpiWriter {
    JceFrameKpiWriterConfig config;
    JceFrameKpiWriterStatus status;
    JceMutex *mutex;
    unsigned char *bytes;
    KpiRecord *records;
    size_t tail;
    uint32_t first;
    JceAsyncTask *task; /* Owner ref; publication/clear and worker reads use mutex. */
};

/* No logging or IO while holding the queue lock. First error wins. */
static void writer_fail_locked(JceFrameKpiWriter *w, JceFrameKpiWriterError error,
                                const char *message, size_t requested, size_t actual,
                                SDL_IOStatus status)
{
    if (w->status.error == JCE_KPI_WRITER_ERROR_NONE) {
        JceFrameKpiWriterFailure *first = &w->status.first_failure;
        first->captured = true;
        first->observed_ns = jce_time_ticks_ns();
        first->queued_bytes = w->status.queued_bytes;
        first->queued_records = w->status.queued_records;
        first->submitted_tasks = w->status.submitted_tasks;
        first->started_tasks = w->status.started_tasks;
        first->completion_callbacks = w->status.completion_callbacks;
        first->write_calls = w->status.write_calls;
        first->write = w->status.write;
        first->task_state_available = w->task != NULL;
        first->task_state = w->task ? jce_async_task_state(w->task)
                                    : JCE_ASYNC_STATE_INVALID;
        w->status.error = error;
        w->status.failed_requested_bytes = requested;
        w->status.failed_actual_bytes = actual;
        w->status.failed_io_status = status;
        SDL_strlcpy(w->status.message, message && message[0] ? message : "KPI capture failed",
                     sizeof w->status.message);
    }
    w->status.state = JCE_KPI_WRITER_FAILED;
}
static bool writer_owner(JceFrameKpiWriter *w)
{
    return w && jce_async_executor_is_owner(w->config.executor);
}
/* Owner-only guard, independent of worker queue ownership. A rejected nested
 * operation changes no capture state and leaves its owner available to retry. */
static bool writer_operation_begin(JceFrameKpiWriter *w)
{
    if (!writer_owner(w)) return false;
    jce_mutex_lock(w->mutex);
    bool available = !w->status.owner_operation_active;
    if (available) w->status.owner_operation_active = true;
    jce_mutex_unlock(w->mutex);
    return available;
}
static void writer_operation_end(JceFrameKpiWriter *w)
{
    jce_mutex_lock(w->mutex);
    w->status.owner_operation_active = false;
    jce_mutex_unlock(w->mutex);
}
static bool writer_write(JceFrameKpiWriter *w, const void *bytes, size_t length,
                         JceFrameKpiWriterError error, bool data)
{
    /* One exclusive stream actor. Capture error while still on its IO thread. */
    jce_mutex_lock(w->mutex);
    ++w->status.write.serial;
    w->status.write.started_ns = jce_time_ticks_ns();
    w->status.write.returned_ns = 0;
    w->status.write.requested_bytes = length;
    w->status.write.actual_bytes = 0;
    w->status.write.io_status = SDL_IO_STATUS_READY; /* Unavailable until return. */
    w->status.write.active = true;
    w->status.write.data = data;
    w->status.write.return_available = false;
    jce_mutex_unlock(w->mutex);
    size_t actual = SDL_WriteIO(w->config.stream, bytes, length);
    uint64_t returned_ns = jce_time_ticks_ns();
    SDL_IOStatus status = SDL_GetIOStatus(w->config.stream);
    char message[160];
    SDL_strlcpy(message, SDL_GetError(), sizeof message);
    jce_mutex_lock(w->mutex);
    w->status.write.returned_ns = returned_ns;
    w->status.write.actual_bytes = actual;
    w->status.write.io_status = status;
    w->status.write.active = false;
    w->status.write.return_available = true;
    if (w->status.first_failure.captured && w->status.first_failure.write.active &&
        w->status.first_failure.write.serial == w->status.write.serial)
        w->status.failure_write_return = w->status.write;
    ++w->status.write_calls;
    if (data && actual <= length) w->status.written_bytes += actual;
    if (actual != length || status != SDL_IO_STATUS_READY)
        writer_fail_locked(w, error, message, length, actual, status);
    jce_mutex_unlock(w->mutex);
    return actual == length && status == SDL_IO_STATUS_READY;
}
static bool writer_flush(JceFrameKpiWriter *w, JceFrameKpiWriterError error)
{
    bool ok = SDL_FlushIO(w->config.stream);
    SDL_IOStatus status = SDL_GetIOStatus(w->config.stream);
    char message[160];
    SDL_strlcpy(message, SDL_GetError(), sizeof message);
    ok = ok && status == SDL_IO_STATUS_READY;
    jce_mutex_lock(w->mutex);
    ++w->status.flush_calls;
    if (!ok) writer_fail_locked(w, error, message, 0, 0, status);
    jce_mutex_unlock(w->mutex);
    return ok;
}
static JceAsyncRunResult writer_work_impl(JceAsyncContext *ctx, void *user)
{
    JceFrameKpiWriter *w = user;
    size_t length = 0, offset = 0;
    uint32_t count = 0, limit;
    bool cancel_requested = jce_async_context_cancel_requested(ctx);
    jce_mutex_lock(w->mutex);
    ++w->status.started_tasks; /* Actual work entry, including cancelled/failed work. */
    if (cancel_requested) {
        jce_mutex_unlock(w->mutex);
        goto cancelled;
    }
    if (w->status.state == JCE_KPI_WRITER_FAILED) {
        jce_mutex_unlock(w->mutex);
        return JCE_ASYNC_RUN_FAILED;
    }
    limit = w->config.drain_records;
    if (w->config.durable_interval) {
        uint32_t until_flush = w->config.durable_interval -
            (uint32_t)(w->status.written_records % w->config.durable_interval);
        if (limit > until_flush) limit = until_flush;
    }
    while (count < w->status.queued_records && count < limit) {
        KpiRecord record = w->records[((size_t)w->first + count) % w->config.record_capacity];
        if (count && record.length > w->config.drain_bytes - length) break;
        if (!count) offset = record.offset;
        length += record.length;
        ++count;
        if (length >= w->config.drain_bytes) break;
    }
    jce_mutex_unlock(w->mutex);
    if (!count) return JCE_ASYNC_RUN_SUCCESS;
    size_t first_span = w->config.byte_capacity - offset;
    if (first_span > length) first_span = length;
    if (!writer_write(w, w->bytes + offset, first_span, JCE_KPI_WRITER_ERROR_WRITE, true))
        return JCE_ASYNC_RUN_FAILED;
    if (first_span < length &&
        !writer_write(w, w->bytes, length - first_span, JCE_KPI_WRITER_ERROR_WRITE, true))
        return JCE_ASYNC_RUN_FAILED;
    /* A cancellation cannot silently certify its already-written prefix. */
    jce_mutex_lock(w->mutex);
    w->status.written_records += count;
    bool flush = w->config.durable_interval &&
        w->status.written_records % w->config.durable_interval == 0;
    jce_mutex_unlock(w->mutex);
    if (flush && !writer_flush(w, JCE_KPI_WRITER_ERROR_FLUSH))
        return JCE_ASYNC_RUN_FAILED;
    jce_mutex_lock(w->mutex);
    w->first = (uint32_t)(((size_t)w->first + count) % w->config.record_capacity);
    w->status.queued_records -= count;
    w->status.queued_bytes -= length;
    bool failed = w->status.state == JCE_KPI_WRITER_FAILED;
    jce_mutex_unlock(w->mutex);
    if (jce_async_context_cancel_requested(ctx)) goto cancelled;
    return failed ? JCE_ASYNC_RUN_FAILED : JCE_ASYNC_RUN_SUCCESS;
cancelled:
    jce_mutex_lock(w->mutex);
    writer_fail_locked(w, JCE_KPI_WRITER_ERROR_CANCELLED,
                        "KPI writer task cancelled; capture incomplete", 0, 0, SDL_IO_STATUS_ERROR);
    jce_mutex_unlock(w->mutex);
    return JCE_ASYNC_RUN_CANCELLED;
}
static JceAsyncRunResult writer_work(JceAsyncContext *ctx, void *user)
{
    JceFrameKpiWriter *w = user;
    /* The default cooperative pump may call work outside our public poll.
     * Guard its sink callbacks too; threaded workers must not block owner
     * submissions by claiming an owner-operation guard. */
    bool acquired = false;
    if (!w->status.off_thread && writer_owner(w)) {
        jce_mutex_lock(w->mutex);
        acquired = !w->status.owner_operation_active;
        if (acquired) w->status.owner_operation_active = true;
        jce_mutex_unlock(w->mutex);
    }
    JceAsyncRunResult result = writer_work_impl(ctx, user);
    if (acquired) writer_operation_end(w);
    return result;
}
static void writer_complete(JceAsyncTask *task, void *user)
{
    JceFrameKpiWriter *w = user;
    JceAsyncState state = jce_async_task_state(task);
    jce_mutex_lock(w->mutex);
    ++w->status.completion_callbacks;
    if (state != JCE_ASYNC_STATE_SUCCEEDED)
        writer_fail_locked(w, state == JCE_ASYNC_STATE_CANCELLED
            ? JCE_KPI_WRITER_ERROR_CANCELLED : JCE_KPI_WRITER_ERROR_TASK,
            "KPI writer task did not succeed", 0, 0, SDL_IO_STATUS_ERROR);
    w->status.task_outstanding = false;
    w->task = NULL;
    jce_mutex_unlock(w->mutex);
    jce_async_task_release(task); /* Drop owner ref; callback's ref remains live. */
}
/* Queue lock held. Only the existing owner poll observes an age budget;
 * no new timer, worker wait or physical IO is introduced by coalescing. */
static bool writer_batch_ready_locked(JceFrameKpiWriter *w)
{
    if (w->status.state == JCE_KPI_WRITER_FINISHING ||
        !w->config.min_batch_bytes || !w->config.max_delay_ms ||
        w->status.queued_bytes >= w->config.min_batch_bytes ||
        w->status.queued_records == w->config.record_capacity)
        return true;
    if (w->config.durable_interval) {
        uint32_t until_flush = w->config.durable_interval -
            (uint32_t)(w->status.written_records % w->config.durable_interval);
        if (w->status.queued_records >= until_flush) return true;
    }
    uint64_t oldest = w->records[w->first].enqueued_ns;
    uint64_t now = jce_time_ticks_ns();
    return now >= oldest && now - oldest >= (uint64_t)w->config.max_delay_ms * 1000000u;
}
static bool writer_schedule(JceFrameKpiWriter *w)
{
    jce_mutex_lock(w->mutex);
    bool failed = w->status.state == JCE_KPI_WRITER_FAILED;
    bool pending = w->status.queued_records != 0;
    bool ready = !failed && pending && writer_batch_ready_locked(w);
    jce_mutex_unlock(w->mutex);
    if (failed || !pending) return !failed;
    if (!ready) return true; /* Retain copied records until an existing poll/submit. */
    if (w->task) return true; /* Keep callback ownership without hiding failure. */
    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = writer_work;
    desc.complete = writer_complete;
    desc.user_data = w;
    desc.priority = JCE_ASYNC_PRIORITY_LOW;
    /* Existing async submit/execute supplies thread-aware task trace spans.
     * Never add worker time to the main-thread phase table. */
    desc.debug_name = "frame-kpi-write";
    /* Non-inline submission; no owner callback can run before the assignment. */
    JceAsyncTask *task = jce_async_submit(w->config.executor, &desc);
    jce_mutex_lock(w->mutex);
    w->task = task;
    if (task) ++w->status.submitted_tasks;
    w->status.task_outstanding = w->task != NULL;
    if (!w->task)
        writer_fail_locked(w, JCE_KPI_WRITER_ERROR_SUBMIT,
                            "KPI executor rejected drain task", 0, 0, SDL_IO_STATUS_ERROR);
    jce_mutex_unlock(w->mutex);
    return w->task != NULL;
}
void jce_frame_kpi_writer_config_init(JceFrameKpiWriterConfig *config, bool low_memory)
{
    if (!config) return;
    memset(config, 0, sizeof *config);
    config->enabled = true;
    config->byte_capacity = low_memory ? 262144u : 1048576u;
    config->record_capacity = low_memory ? 256u : 1024u;
    config->drain_bytes = 65536u;
    config->drain_records = 64u;
}
JceFrameKpiWriter *jce_frame_kpi_writer_create(const JceFrameKpiWriterConfig *config)
{
    if (!config || !config->enabled) return NULL;
    if (!config->stream || !config->executor ||
        !jce_async_executor_is_owner(config->executor) || !config->byte_capacity ||
        !config->record_capacity || !config->drain_bytes || !config->drain_records ||
        config->record_capacity > SIZE_MAX / sizeof(KpiRecord)) return NULL;
    JceFrameKpiWriter *w = JCE_NEW(JceFrameKpiWriter);
    if (!w) return NULL;
    w->bytes = JCE_MALLOC(config->byte_capacity);
    w->records = JCE_CALLOC(config->record_capacity, sizeof *w->records);
    w->mutex = jce_mutex_create();
    if (!w->bytes || !w->records || !w->mutex) {
        if (w->mutex) jce_mutex_destroy(w->mutex);
        JCE_FREE(w->records); JCE_FREE(w->bytes); JCE_FREE(w);
        return NULL; /* Stream ownership has not transferred. */
    }
    w->config = *config;
    w->status.byte_capacity = config->byte_capacity;
    w->status.record_capacity = config->record_capacity;
    w->status.mode = jce_async_executor_mode(config->executor);
    w->status.off_thread = w->status.mode == JCE_ASYNC_EXECUTION_THREADED;
    w->status.state = JCE_KPI_WRITER_OPEN;
    const char *header = jce_frame_kpi_csv_header(config->context_json);
    if (writer_write(w, header, strlen(header), JCE_KPI_WRITER_ERROR_HEADER_WRITE, false))
        writer_flush(w, JCE_KPI_WRITER_ERROR_HEADER_FLUSH);
    return w;
}
static bool writer_submit_impl(JceFrameKpiWriter *w, uint64_t frame,
                                const void *bytes, size_t length)
{
    if (!writer_owner(w)) return false;
    jce_mutex_lock(w->mutex);
    if (w->status.state != JCE_KPI_WRITER_OPEN) {
        jce_mutex_unlock(w->mutex); return false;
    }
    if (!bytes || !length || frame != w->status.accepted_records ||
        length > w->config.byte_capacity ||
        w->status.accepted_records == UINT64_MAX ||
        length > UINT64_MAX - w->status.accepted_bytes) {
        writer_fail_locked(w, JCE_KPI_WRITER_ERROR_INVALID_RECORD,
                            "Invalid or over-cap KPI record", length, 0, SDL_IO_STATUS_ERROR);
        jce_mutex_unlock(w->mutex); return false;
    }
    if (w->status.queued_records == w->config.record_capacity ||
        length > w->config.byte_capacity - w->status.queued_bytes) {
        writer_fail_locked(w, JCE_KPI_WRITER_ERROR_FULL,
                            "KPI queue capacity exhausted; capture failed", length, 0,
                            SDL_IO_STATUS_ERROR);
        jce_mutex_unlock(w->mutex); return false;
    }
    size_t first_span = w->config.byte_capacity - w->tail;
    if (first_span > length) first_span = length;
    memcpy(w->bytes + w->tail, bytes, first_span);
    if (first_span < length) memcpy(w->bytes, (const unsigned char *)bytes + first_span, length - first_span);
    KpiRecord *record = &w->records[((size_t)w->first + w->status.queued_records) % w->config.record_capacity];
    record->offset = w->tail;
    record->length = length;
    record->enqueued_ns = w->config.min_batch_bytes && w->config.max_delay_ms
        ? jce_time_ticks_ns() : 0;
    w->tail = length >= w->config.byte_capacity - w->tail
        ? length - (w->config.byte_capacity - w->tail) : w->tail + length;
    ++w->status.queued_records;
    w->status.queued_bytes += length;
    ++w->status.accepted_records;
    w->status.accepted_bytes += length;
    if (w->status.queued_bytes > w->status.peak_bytes) w->status.peak_bytes = w->status.queued_bytes;
    if (w->status.queued_records > w->status.peak_records) w->status.peak_records = w->status.queued_records;
    jce_mutex_unlock(w->mutex);
    return writer_schedule(w);
}
static bool writer_poll_impl(JceFrameKpiWriter *w)
{
    if (!writer_owner(w)) return false;
    jce_async_executor_pump(w->config.executor, NULL);
    return writer_schedule(w);
}
static bool writer_finish_impl(JceFrameKpiWriter *w)
{
    if (!writer_owner(w)) return false;
    jce_mutex_lock(w->mutex);
    if (w->status.closed) {
        bool ok = w->status.state == JCE_KPI_WRITER_COMPLETE;
        jce_mutex_unlock(w->mutex); return ok;
    }
    if (w->status.state == JCE_KPI_WRITER_OPEN) w->status.state = JCE_KPI_WRITER_FINISHING;
    jce_mutex_unlock(w->mutex);
    for (;;) {
        writer_poll_impl(w);
        jce_mutex_lock(w->mutex);
        bool done = !w->task && (!w->status.queued_records ||
                                 w->status.state == JCE_KPI_WRITER_FAILED);
        jce_mutex_unlock(w->mutex);
        if (done) break;
        jce_thread_sleep_ms(1);
    }
    /* All real owner callbacks have run; not merely task terminal state. */
    writer_flush(w, JCE_KPI_WRITER_ERROR_FLUSH);
    bool closed = SDL_CloseIO(w->config.stream);
    char message[160]; SDL_strlcpy(message, SDL_GetError(), sizeof message);
    w->config.stream = NULL;
    jce_mutex_lock(w->mutex);
    w->status.closed = true;
    if (!closed) writer_fail_locked(w, JCE_KPI_WRITER_ERROR_CLOSE, message, 0, 0, SDL_IO_STATUS_ERROR);
    bool complete = w->status.error == JCE_KPI_WRITER_ERROR_NONE &&
        w->status.accepted_records == w->status.written_records &&
        w->status.accepted_bytes == w->status.written_bytes && !w->status.queued_records;
    if (complete) w->status.state = JCE_KPI_WRITER_COMPLETE;
    else if (w->status.error == JCE_KPI_WRITER_ERROR_NONE)
        writer_fail_locked(w, JCE_KPI_WRITER_ERROR_TASK, "KPI drain accounting mismatch", 0, 0, SDL_IO_STATUS_ERROR);
    jce_mutex_unlock(w->mutex);
    return complete;
}
bool jce_frame_kpi_writer_submit(JceFrameKpiWriter *w, uint64_t frame,
                                const void *bytes, size_t length)
{
    if (!writer_operation_begin(w)) return false;
    bool ok = writer_submit_impl(w, frame, bytes, length);
    writer_operation_end(w);
    return ok;
}
bool jce_frame_kpi_writer_poll(JceFrameKpiWriter *w)
{
    if (!writer_operation_begin(w)) return false;
    bool ok = writer_poll_impl(w);
    writer_operation_end(w);
    return ok;
}
bool jce_frame_kpi_writer_finish(JceFrameKpiWriter *w)
{
    if (!writer_operation_begin(w)) return false;
    bool complete = writer_finish_impl(w);
    writer_operation_end(w);
    return complete;
}
bool jce_frame_kpi_writer_destroy(JceFrameKpiWriter *w)
{
    if (!w) return true;
    if (!writer_operation_begin(w)) return false;
    writer_finish_impl(w); /* A capture error still permits joined cleanup. */
    jce_mutex_lock(w->mutex);
    bool joined = w->status.closed && !w->task;
    jce_mutex_unlock(w->mutex);
    if (!joined) { writer_operation_end(w); return false; }
    jce_mutex_destroy(w->mutex);
    JCE_FREE(w->records); JCE_FREE(w->bytes); JCE_FREE(w);
    return true; /* Cleanup success, not capture success. Owner may clear ptr. */
}
void jce_frame_kpi_writer_get_status(JceFrameKpiWriter *w, JceFrameKpiWriterStatus *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!w) return;
    jce_mutex_lock(w->mutex);
    *out = w->status;
    jce_mutex_unlock(w->mutex);
}
