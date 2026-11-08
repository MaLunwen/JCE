/* Optional bounded-ring consumer; default-off engine hook owns its lifetime. */
#include "jce_trace_capture_internal.h"
#include "jce_trace_clock_internal.h"
#include "jce_memory.h"
#include "jce_filesystem_host_internal.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <SDL3/SDL.h>
#include <string.h>

#define CAPTURE_BATCH 512u
#define CAPTURE_DRAIN_BATCHES 128u
#define CAPTURE_FLUSH_BYTES (1024u * 1024u)
#define CAPTURE_FLUSH_DELAY_NS UINT64_C(16000000)
#define CAPTURE_SIZE_BUCKETS 6u
#define CAPTURE_WALL_BUCKETS 7u
/* Flush after crossing 1 MiB, with room for one bounded encoder batch and
 * its terminating NUL. This is consumer-owned, lazy allocation, not a ring. */
#define CAPTURE_TEXT_LIMIT (CAPTURE_FLUSH_BYTES + \
    CAPTURE_BATCH * JCE_TRACE_CHROME_EVENT_MAX_BYTES + 1u)

typedef struct CaptureLossWitness {
    bool available, clock_valid, previous_valid;
    uint64_t read, observed_ns, loss_delta, lost_total;
    uint32_t phase; /* 0 threaded, 1 cooperative, 2 terminal owner drain. */
    JceTraceReadObservation snapshot, previous;
} CaptureLossWitness;

struct JceTraceCapture {
    JceTraceCursor cursor;
    JceTraceCaptureMode mode;
    JceThread *worker;
    bool worker_available; /* Historical creation fact, retained after quiesce. */
    SDL_IOStream *stream; /* Exclusive partial writer; handed to worker, joined before close. */
    SDL_AtomicInt stopping;
    char *path;
    char *final_path;
    char pending[128];
    uint32_t pending_size;
    uint32_t path_capacity;
    uint32_t poll_ms;
    uint32_t generation;
    uint64_t captured;
    uint64_t overwritten_before_capture;
    bool first;
    bool write_ok;
    bool generation_stable;
    bool sequence_exhausted;
    bool budget_exhausted;
    bool slow_io_reported;
    TraceJsonBuffer staging;
    uint64_t staged_events;
    uint64_t encode_batches;
    uint64_t append_calls;
    uint64_t payload_bytes;
    uint64_t max_encode_ns;
    uint64_t max_append_ns;
    uint64_t max_drain_ns;
    uint64_t staging_started_ns;
    uint64_t append_size_buckets[CAPTURE_SIZE_BUCKETS];
    uint64_t append_wall_buckets[CAPTURE_WALL_BUCKETS];
    uint64_t slowest_append_started_ns;
    uint64_t slowest_append_finished_ns;
    uint64_t slowest_append_requested_bytes;
    bool finalizing, reader_has_previous;
    uint64_t reader_reads, reader_loss_reads, reader_loss_events;
    uint64_t reader_stop_counts[JCE_TRACE_READ_STOP_COUNT];
    JceTraceReadObservation reader_previous;
    CaptureLossWitness first_loss, last_loss;
};

static bool capture_add(uint64_t *value, uint64_t amount)
{
    if (amount > UINT64_MAX - *value) return false;
    *value += amount;
    return true;
}

static void capture_duration_max(uint64_t *maximum, uint64_t started)
{
    uint64_t finished = jce_time_ticks_ns();
    uint64_t elapsed = finished >= started ? finished - started : 0;
    if (elapsed > *maximum) *maximum = elapsed;
}

/* Upper-inclusive fixed payload-write buckets; header/footer/close are excluded. */
static unsigned capture_size_bucket(uint64_t bytes)
{
    static const uint64_t upper[CAPTURE_SIZE_BUCKETS - 1u] = {
        UINT64_C(4096), UINT64_C(16384), UINT64_C(65536),
        UINT64_C(262144), UINT64_C(1048576)
    };
    unsigned bucket = 0;
    while (bucket + 1u < CAPTURE_SIZE_BUCKETS && bytes > upper[bucket]) ++bucket;
    return bucket;
}
static unsigned capture_wall_bucket(uint64_t ns)
{
    static const uint64_t upper[CAPTURE_WALL_BUCKETS - 1u] = {
        UINT64_C(100000), UINT64_C(1000000), UINT64_C(4000000),
        UINT64_C(16000000), UINT64_C(100000000), UINT64_C(1000000000)
    };
    unsigned bucket = 0;
    while (bucket + 1u < CAPTURE_WALL_BUCKETS && ns > upper[bucket]) ++bucket;
    return bucket;
}
static bool capture_flush(JceTraceCapture *capture)
{
    if (!capture->staging.size) return true;
    uint64_t started, finished;
    if (!jce_trace_clock_now_internal(&started)) {
        capture->write_ok = false;
        LOG_ERROR("trace_capture", "payload write clock unavailable before IO; capture retained incomplete");
        return false;
    }
    if (!capture_add(&capture->append_calls, 1)) {
        capture->write_ok = false;
        return false;
    }
    bool ok = jce__fs_host_stream_write(capture->stream, capture->staging.data,
                                       capture->staging.size);
    /* The bytes may already exist: refuse the capture and retain ownership,
     * never retry this staging or substitute an unrelated/zero clock. */
    if (!jce_trace_clock_now_internal(&finished) || finished < started) {
        capture->write_ok = false;
        LOG_ERROR("trace_capture", "payload write clock unavailable or reversed after IO; capture retained incomplete");
        return false;
    }
    uint64_t elapsed = finished - started;
    if (capture->append_calls == 1 || elapsed > capture->max_append_ns) {
        capture->max_append_ns = elapsed;
        capture->slowest_append_started_ns = started;
        capture->slowest_append_finished_ns = finished;
        capture->slowest_append_requested_bytes = capture->staging.size;
    }
    bool counted = capture_add(&capture->append_size_buckets[capture_size_bucket(capture->staging.size)], 1) &&
        capture_add(&capture->append_wall_buckets[capture_wall_bucket(elapsed)], 1);
    if (!capture->slow_io_reported && capture->max_append_ns >= UINT64_C(2000000000)) {
        capture->slow_io_reported = true;
        LOG_WARN("trace_capture", "persistent write exceeded 2s: max_write_ns=%llu bytes=%llu; no events sampled or discarded by writer",
            (unsigned long long)capture->max_append_ns,
            (unsigned long long)capture->staging.size);
    }
    if (!ok || !counted ||
        !capture_add(&capture->payload_bytes, capture->staging.size) ||
        !capture_add(&capture->captured, capture->staged_events)) {
        capture->write_ok = false;
        return false;
    }
    capture->staging.size = 0;
    capture->staged_events = 0;
    capture->staging_started_ns = 0;
    return true;
}
/* Deadline is measured from the first encoded pending event, not from each
 * poll or from producer submission. It bounds the requested launch policy,
 * never scheduler latency or the duration of synchronous host IO. */
static bool capture_flush_due(const JceTraceCapture *capture)
{
    if (!capture->staged_events) return false;
    uint64_t now = jce_time_ticks_ns();
    return now < capture->staging_started_ns ||
        now - capture->staging_started_ns >= CAPTURE_FLUSH_DELAY_NS;
}
static bool capture_finish_drain(JceTraceCapture *capture, bool force_flush)
{
    if (force_flush || capture->mode == JCE_TRACE_CAPTURE_COOPERATIVE ||
        capture_flush_due(capture)) return capture_flush(capture);
    return true;
}
/* A loss witness is a post-read detection point. Sampling only losing reads
 * adds no producer checks or clock reads for ordinary successful batches. */
static bool capture_observe_read(JceTraceCapture *capture,
                                 const JceTraceReadObservation *observation)
{
    if (observation->stop >= JCE_TRACE_READ_STOP_COUNT ||
        !capture_add(&capture->reader_reads, 1) ||
        !capture_add(&capture->reader_stop_counts[observation->stop], 1)) {
        capture->write_ok = false; return false;
    }
    if (observation->lost_delta) {
        CaptureLossWitness witness = {0};
        witness.available = true;
        witness.read = capture->reader_reads;
        witness.phase = capture->finalizing ? 2u :
            capture->mode == JCE_TRACE_CAPTURE_COOPERATIVE ? 1u : 0u;
        witness.clock_valid = jce_trace_clock_now_internal(&witness.observed_ns);
        if (!witness.clock_valid) witness.observed_ns = 0;
        witness.loss_delta = observation->lost_delta;
        witness.lost_total = capture->cursor.lost_events;
        witness.snapshot = *observation;
        witness.previous_valid = capture->reader_has_previous;
        witness.previous = capture->reader_previous;
        if (!capture->reader_loss_reads) capture->first_loss = witness;
        capture->last_loss = witness;
        if (!capture_add(&capture->reader_loss_reads, 1) ||
            !capture_add(&capture->reader_loss_events, observation->lost_delta)) {
            capture->write_ok = false; return false;
        }
    }
    capture->reader_previous = *observation;
    capture->reader_has_previous = true;
    return true;
}

static bool capture_drain_impl(JceTraceCapture *capture, bool force_flush)
{
    JceTraceEvent events[CAPTURE_BATCH];
    capture->budget_exhausted = false;
    for (unsigned batch = 0; batch < CAPTURE_DRAIN_BATCHES; ++batch) {
        if (!jce_trace_capture_valid_internal()) {
            capture->sequence_exhausted = true;
            return false;
        }
        if (jce_trace_capture_generation() != capture->generation) {
            capture->generation_stable = false;
            return false;
        }
        JceTraceReadObservation observation;
        uint32_t count = jce_trace_read_observed_internal(&capture->cursor,
            events, CAPTURE_BATCH, &observation);
        if (!capture_observe_read(capture, &observation)) return false;
        if (!count) return capture_finish_drain(capture, force_flush);
        if (!capture_add(&capture->encode_batches, 1)) {
            capture->write_ok = false;
            return false;
        }
        uint64_t started = jce_time_ticks_ns();
        bool was_empty = capture->staged_events == 0;
        bool ok = jce_trace_chrome_encode_batch_into(events, count,
            &capture->first, &capture->staging);
        capture_duration_max(&capture->max_encode_ns, started);
        if (ok) ok = capture_add(&capture->staged_events, count);
        if (!ok) {
            capture->write_ok = false;
            return false;
        }
        if (was_empty) capture->staging_started_ns = started;
        if ((capture->staging.size >= CAPTURE_FLUSH_BYTES ||
             (capture->mode == JCE_TRACE_CAPTURE_THREADED && capture_flush_due(capture))) &&
            !capture_flush(capture)) return false;
        if (count < CAPTURE_BATCH) return capture_finish_drain(capture, force_flush);
    }
    capture->budget_exhausted = true;
    return capture_finish_drain(capture, force_flush);
}
static bool capture_drain(JceTraceCapture *capture, bool force_flush)
{
    if (!jce_trace_suppress_begin()) { capture->write_ok = false; return false; }
    uint64_t started = jce_time_ticks_ns();
    bool ok = capture_drain_impl(capture, force_flush);
    capture_duration_max(&capture->max_drain_ns, started);
    if (!jce_trace_suppress_end()) { capture->write_ok = false; return false; }
    return ok;
}
static uint32_t capture_sleep_ms(const JceTraceCapture *capture)
{
    uint32_t sleep_ms = capture->poll_ms;
    if (capture->staged_events) {
        uint64_t now = jce_time_ticks_ns();
        if (now < capture->staging_started_ns) return 0;
        uint64_t elapsed = now - capture->staging_started_ns;
        if (elapsed >= CAPTURE_FLUSH_DELAY_NS) return 0;
        /* Millisecond sleep can round by <1ms and the scheduler can delay it.
         * Recheck the same deadline on waking; never reset it on new events. */
        uint32_t remaining_ms = (uint32_t)((CAPTURE_FLUSH_DELAY_NS - elapsed +
            UINT64_C(999999)) / UINT64_C(1000000));
        if (sleep_ms > remaining_ms) sleep_ms = remaining_ms;
    }
    return sleep_ms;
}
static void capture_worker(void *arg)
{
    JceTraceCapture *capture = arg;
    while (!SDL_GetAtomicInt(&capture->stopping)) {
        if (!capture_drain(capture, false)) break;
        /* A full bounded pass can still have producers ahead of its cursor.
         * Check stopping each pass; sleep only after an empty/short read. */
        if (!capture->budget_exhausted) jce_thread_sleep_ms(capture_sleep_ms(capture));
    }
}

JceTraceCapture *jce_trace_capture_start(const char *path, uint32_t poll_ms)
{
#if JCE_PLATFORM_WEB
    return jce_trace_capture_start_mode(path, poll_ms, JCE_TRACE_CAPTURE_COOPERATIVE);
#else
    return jce_trace_capture_start_mode(path, poll_ms, JCE_TRACE_CAPTURE_THREADED);
#endif
}

void jce_trace_capture_poll(JceTraceCapture *capture)
{
    if (capture && capture->mode == JCE_TRACE_CAPTURE_COOPERATIVE &&
        capture->write_ok && capture->generation_stable)
        capture_drain(capture, true);
}

static JceTraceCapture *capture_start_impl(const char *path, uint32_t poll_ms,
                                           JceTraceCaptureMode mode)
{
    if (!path || !path[0] || !jce_trace_capture_valid_internal() || poll_ms < 1 || poll_ms > 1000 ||
        (mode != JCE_TRACE_CAPTURE_THREADED && mode != JCE_TRACE_CAPTURE_COOPERATIVE))
        return NULL;
    size_t path_length = strlen(path);
    if (path_length > UINT32_MAX - 64u) return NULL;
    JceTraceCapture *capture = JCE_NEW(JceTraceCapture);
    if (!capture) return NULL;
    memset(capture, 0, sizeof(*capture));
    const char *failure_stage = "path-allocation";
    capture->path_capacity = (uint32_t)path_length + 64u;
    capture->final_path = JCE_MALLOC(path_length + 1u);
    capture->path = JCE_MALLOC(capture->path_capacity);
    if (capture->path) capture->path[0] = '\0';
    bool final_created = false, partial_created = false;
    if (!capture->final_path || !capture->path) goto failed;
    strcpy(capture->final_path, path);
    uint64_t nonce = jce_trace_next_id(), ticks = jce_time_ticks_ns();
    failure_stage = "reservation-name-format";
    int pending_length = SDL_snprintf(capture->pending, sizeof(capture->pending),
        "JCE_TRACE_PENDING %llu %llu\n", (unsigned long long)nonce,
        (unsigned long long)ticks);
    int partial_length = SDL_snprintf(capture->path, capture->path_capacity,
        "%s.partial-%llu-%llu", path, (unsigned long long)nonce,
        (unsigned long long)ticks);
    if (pending_length <= 0 || (size_t)pending_length >= sizeof(capture->pending) ||
        partial_length < 0 || (uint32_t)partial_length >= capture->path_capacity)
        goto failed;
    capture->pending_size = (uint32_t)pending_length;
    /* Reserve the exact formal path atomically with deliberately invalid JSON.
     * A failed stat/unique-name query cannot authorize overwriting old proof. */
    failure_stage = "final-reservation-open-write-close";
    if (!jce__fs_host_reserve_new(capture->final_path, capture->pending,
                                  capture->pending_size, &final_created)) goto failed;
    capture->mode = mode;
    capture->poll_ms = poll_ms;
    capture->first = true;
    capture->write_ok = true;
    capture->generation_stable = true;
    capture->staging.limit = CAPTURE_TEXT_LIMIT;
    capture->generation = jce_trace_capture_generation();
    JceTraceStats initial;
    jce_trace_get_stats(&initial);
    failure_stage = "sequence-validity";
    if (!jce_trace_capture_valid_internal()) goto failed;
    capture->overwritten_before_capture = initial.recorded_events > initial.capacity
        ? initial.recorded_events - initial.capacity : 0;
    jce_trace_cursor_init(&capture->cursor);
    capture->cursor.next_sequence = 1; /* Include/report pre-start prefix. */
    const char *header = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[";
    failure_stage = "partial-exclusive-open";
    capture->stream = jce__fs_host_stream_new(capture->path, &partial_created);
    if (!capture->stream) goto failed;
    failure_stage = "partial-header-write";
    if (!jce__fs_host_stream_write(capture->stream, header, strlen(header))) goto failed;
    if (mode == JCE_TRACE_CAPTURE_THREADED) {
        failure_stage = "worker-create";
        capture->worker = jce_thread_create(capture_worker, capture, "trace-capture");
        if (!capture->worker) goto failed;
        capture->worker_available = true;
    }
    return capture;
failed:
    if (capture->stream) {
        bool closed = jce__fs_host_stream_close(&capture->stream);
        if (!closed) LOG_ERROR("trace_capture", "failed startup partial close rejected");
    }
    LOG_ERROR("trace_capture", "capture startup rejected: stage=%s final=%s partial=%s final_created=%d partial_created=%d",
        failure_stage, path, capture->path && capture->path[0] ? capture->path : "(not-selected)",
        final_created, partial_created);
    JCE_FREE(capture->staging.data);
    JCE_FREE(capture->final_path);
    JCE_FREE(capture->path);
    JCE_FREE(capture);
    return NULL;
}

JceTraceCapture *jce_trace_capture_start_mode(const char *path, uint32_t poll_ms,
                                            JceTraceCaptureMode mode)
{
    if (!jce_trace_capture_valid_internal() || !jce_trace_enabled() ||
        !jce_trace_suppress_begin()) return NULL;
    JceTraceCapture *capture = capture_start_impl(path, poll_ms, mode);
    if (!jce_trace_suppress_end()) {
        if (capture) {
            capture->write_ok = false;
            jce_trace_capture_stop(capture, NULL);
        }
        return NULL;
    }
    return capture;
}

void jce_trace_capture_quiesce(JceTraceCapture *capture)
{
    if (!capture) return;
    SDL_SetAtomicInt(&capture->stopping, 1);
    if (capture->worker) {
        uint64_t wait_id = jce_trace_next_id();
        uint64_t wait_parent = jce_trace_task_current_id();
        uint64_t wait_started = jce_time_ticks_ns();
        jce_trace_wait_begin(wait_id, wait_parent, "trace.capture.join");
        jce_thread_join(capture->worker);
        capture->worker = NULL;
        jce_trace_wait_end(wait_id, wait_parent, "trace.capture.join",
                           jce_time_ticks_ns() - wait_started);
    }
}

static bool capture_read_json(char *json, size_t capacity,
                               const JceTraceReadObservation *read)
{
    int n = SDL_snprintf(json, capacity,
        "{\"write\":%u,\"earliest\":%u,\"next_before\":%u,\"next_after\":%u,"
        "\"count\":%u,\"stop\":%u,\"blocked\":%u,\"published\":%u,\"published_valid\":%s}",
        read->write_sequence, read->earliest_sequence, read->next_before,
        read->next_after, read->count, (unsigned)read->stop,
        read->blocked_sequence, read->published_sequence,
        read->published_valid ? "true" : "false");
    return n > 0 && (size_t)n < capacity;
}
static bool capture_loss_json(char *json, size_t capacity,
                               const CaptureLossWitness *loss)
{
    if (!loss->available) {
        if (capacity < 5) return false;
        memcpy(json, "null", 5); return true;
    }
    char snapshot[512], previous[512] = "null";
    if (!capture_read_json(snapshot, sizeof(snapshot), &loss->snapshot) ||
        (loss->previous_valid &&
         !capture_read_json(previous, sizeof(previous), &loss->previous))) return false;
    int n = SDL_snprintf(json, capacity,
        "{\"read\":%llu,\"phase\":%u,\"clock_valid\":%s,\"observed_ns\":%llu,"
        "\"loss_delta\":%llu,\"lost_total\":%llu,\"snapshot\":%s,\"previous\":%s}",
        (unsigned long long)loss->read, loss->phase,
        loss->clock_valid ? "true" : "false", (unsigned long long)loss->observed_ns,
        (unsigned long long)loss->loss_delta, (unsigned long long)loss->lost_total,
        snapshot, previous);
    return n > 0 && (size_t)n < capacity;
}
static bool capture_reader_json(char *json, size_t capacity,
                                 const JceTraceCapture *capture)
{
    char first[1536], last[1536], read[512] = "null";
    if (!capture_loss_json(first, sizeof(first), &capture->first_loss) ||
        !capture_loss_json(last, sizeof(last), &capture->last_loss) ||
        (capture->reader_has_previous &&
         !capture_read_json(read, sizeof(read), &capture->reader_previous))) return false;
    int n = SDL_snprintf(json, capacity,
        "{\"version\":1,\"clock_domain\":\"process-perf-ns\",\"read_calls\":%llu,"
        "\"loss_reads\":%llu,\"loss_events\":%llu,\"stop_counts\":[%llu,%llu,%llu,%llu,%llu],"
        "\"first_loss\":%s,\"last_loss\":%s,\"last_read\":%s}",
        (unsigned long long)capture->reader_reads,
        (unsigned long long)capture->reader_loss_reads,
        (unsigned long long)capture->reader_loss_events,
        (unsigned long long)capture->reader_stop_counts[0],
        (unsigned long long)capture->reader_stop_counts[1],
        (unsigned long long)capture->reader_stop_counts[2],
        (unsigned long long)capture->reader_stop_counts[3],
        (unsigned long long)capture->reader_stop_counts[4], first, last, read);
    return n > 0 && (size_t)n < capacity;
}

bool jce_trace_capture_stop(JceTraceCapture *capture, JceTraceCaptureResult *result)
{
    if (!capture) return false;
    const bool worker_available = capture->worker_available;
    jce_trace_capture_quiesce(capture);
    capture->finalizing = true;
    /* No producer may remain: one bounded final drain covers the whole ring
     * plus the capture worker's unregister and this owner's completed join. */
    if (capture->write_ok && capture->generation_stable) capture_drain(capture, true);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    capture->sequence_exhausted = capture->sequence_exhausted ||
        !jce_trace_capture_valid_internal();
    uint64_t accounted = capture->captured + capture->cursor.lost_events;
    uint64_t unexported = stats.recorded_events > accounted
        ? stats.recorded_events - accounted : 0;
    uint64_t cursor_lost = capture->cursor.lost_events > capture->overwritten_before_capture
        ? capture->cursor.lost_events - capture->overwritten_before_capture : 0;
    uint64_t lost = capture->overwritten_before_capture + cursor_lost;
    bool complete = capture->write_ok && capture->generation_stable &&
        !capture->sequence_exhausted &&
        unexported == 0 && lost == 0 &&
        stats.recorded_events == capture->captured + lost && stats.active_threads == 0 &&
        stats.active_work_items == 0 && stats.active_waits == 0;
    /* Fixed, checked storage includes two bounded reader-loss witnesses. */
    char footer[8192], reader_json[4096] = "";
    bool reader_ok = capture_reader_json(reader_json, sizeof(reader_json), capture);
    int length = SDL_snprintf(footer, sizeof(footer),
        "],\"jceTrace\":{\"mode\":\"continuous\",\"consumer_mode\":\"%s\","
        "\"worker_available\":%s,\"capacity\":%u,\"recorded\":%llu,"
        "\"captured\":%llu,\"overwritten_before_capture\":%llu,\"cursor_lost\":%llu,"
        "\"lost\":%llu,\"unexported\":%llu,\"capture_complete\":%s,"
        "\"write_ok\":%s,\"capture_generation_stable\":%s,\"sequence_exhausted\":%s,"
        "\"active_threads\":%u,\"active_work_items\":%u,\"active_waits\":%u,"
        "\"writer_io_model\":\"persistent-exclusive\","
        "\"writer_encode_batches\":%llu,\"writer_append_calls\":%llu,"
        "\"writer_payload_bytes\":%llu,\"writer_buffer_capacity\":%llu,"
        "\"writer_buffer_limit\":%llu,\"writer_max_encode_ns\":%llu,"
        "\"writer_max_append_ns\":%llu,\"writer_max_drain_ns\":%llu,"
        "\"writer_diagnostics\":{\"version\":1,\"operation\":\"payload-write\","
        "\"clock_domain\":\"process-perf-ns\","
        "\"flush_delay_ns\":%llu,\"size_counts\":[%llu,%llu,%llu,%llu,%llu,%llu],"
        "\"wall_ns_counts\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
        "\"slowest_started_ns\":%llu,\"slowest_finished_ns\":%llu,"
        "\"slowest_requested_bytes\":%llu},\"reader_diagnostics\":%s,"
        "\"gpu_completion_observed\":false}}",
        capture->mode == JCE_TRACE_CAPTURE_THREADED ? "threaded" : "cooperative",
        worker_available ? "true" : "false",
        stats.capacity,
        (unsigned long long)stats.recorded_events,
        (unsigned long long)capture->captured,
        (unsigned long long)capture->overwritten_before_capture,
        (unsigned long long)cursor_lost,
        (unsigned long long)lost,
        (unsigned long long)unexported,
        complete ? "true" : "false",
        capture->write_ok ? "true" : "false",
        capture->generation_stable ? "true" : "false",
        capture->sequence_exhausted ? "true" : "false",
        stats.active_threads, stats.active_work_items, stats.active_waits,
        (unsigned long long)capture->encode_batches,
        (unsigned long long)capture->append_calls,
        (unsigned long long)capture->payload_bytes,
        (unsigned long long)capture->staging.capacity,
        (unsigned long long)capture->staging.limit,
        (unsigned long long)capture->max_encode_ns,
        (unsigned long long)capture->max_append_ns,
        (unsigned long long)capture->max_drain_ns,
        (unsigned long long)(capture->mode == JCE_TRACE_CAPTURE_THREADED ? CAPTURE_FLUSH_DELAY_NS : 0),
        (unsigned long long)capture->append_size_buckets[0],
        (unsigned long long)capture->append_size_buckets[1],
        (unsigned long long)capture->append_size_buckets[2],
        (unsigned long long)capture->append_size_buckets[3],
        (unsigned long long)capture->append_size_buckets[4],
        (unsigned long long)capture->append_size_buckets[5],
        (unsigned long long)capture->append_wall_buckets[0],
        (unsigned long long)capture->append_wall_buckets[1],
        (unsigned long long)capture->append_wall_buckets[2],
        (unsigned long long)capture->append_wall_buckets[3],
        (unsigned long long)capture->append_wall_buckets[4],
        (unsigned long long)capture->append_wall_buckets[5],
        (unsigned long long)capture->append_wall_buckets[6],
        (unsigned long long)capture->slowest_append_started_ns,
        (unsigned long long)capture->slowest_append_finished_ns,
        (unsigned long long)capture->slowest_append_requested_bytes, reader_json);
    if (!reader_ok) length = -1;
    bool suppressed = jce_trace_suppress_begin();
    bool ok = capture->write_ok && suppressed && length > 0 &&
        (size_t)length < sizeof(footer) &&
        jce__fs_host_stream_write(capture->stream, footer, (uint64_t)length);
    capture->write_ok = capture->write_ok && ok;
    /* Every acquired partial stream closes exactly once after worker join,
     * including suppressed/write-failure paths. Close flush failure must never
     * publish positive partial JSON over the formal PENDING reservation. */
    uint64_t close_started = jce_time_ticks_ns();
    bool closed = jce__fs_host_stream_close(&capture->stream);
    uint64_t close_finished = jce_time_ticks_ns();
    uint64_t close_ns = close_finished >= close_started ? close_finished - close_started : 0;
    ok = ok && closed;
    capture->write_ok = capture->write_ok && closed;
    jce_log_write(JCE_LOG_LEVEL_INFO, "trace_capture", __FILE__, __LINE__,
        "persistent stream closed: close_ok=%d close_ns=%llu max_write_ns=%llu max_drain_ns=%llu payload_bytes=%llu",
        closed, (unsigned long long)close_ns,
        (unsigned long long)capture->max_append_ns,
        (unsigned long long)capture->max_drain_ns,
        (unsigned long long)capture->payload_bytes);
    bool published = false;
    const char *failure_stage = capture->sequence_exhausted
        ? "sequence-exhausted" : "incomplete-history";
    if (!ok) failure_stage = closed ? "partial-footer-write" : "partial-final-close";
    if (complete && ok) {
        failure_stage = "final-reservation-mismatch";
        /* Replace only our own exclusive reservation, never an unrelated
         * destination. Caller-exclusive use is still required: this read and
         * rename are not protection against malicious external replacement. */
        uint64_t read = 0, total = 0;
        void *pending = jce_fs_host_read_capped(capture->final_path,
            sizeof(capture->pending), &read, &total);
        ok = pending && read == capture->pending_size && total == read &&
            memcmp(pending, capture->pending, (size_t)read) == 0;
        jce_fs_buffer_free(pending);
        if (ok) {
            failure_stage = "final-publication-rename";
            ok = jce_fs_host_rename(capture->path, capture->final_path);
        }
        capture->write_ok = capture->write_ok && ok;
        published = ok;
    }
    if (!published)
        LOG_ERROR("trace_capture", "capture not published: stage=%s final=%s partial=%s lost=%llu unexported=%llu",
            failure_stage, capture->final_path, capture->path,
            (unsigned long long)lost, (unsigned long long)unexported);
    if (result) {
        result->mode = capture->mode;
        result->worker_available = worker_available;
        result->capture_complete = complete && capture->write_ok && published;
        result->published = published;
        result->overwritten_before_capture = capture->overwritten_before_capture;
        result->cursor_lost = cursor_lost;
        result->captured_events = capture->captured;
        result->lost_events = lost;
        result->unexported_events = unexported;
        result->write_ok = capture->write_ok;
        result->generation_stable = capture->generation_stable;
        result->sequence_exhausted = capture->sequence_exhausted;
        result->final_stats = stats;
    }
    ok = complete && capture->write_ok && published;
    JCE_FREE(capture->staging.data);
    JCE_FREE(capture->final_path);
    JCE_FREE(capture->path);
    JCE_FREE(capture);
    if (suppressed && !jce_trace_suppress_end()) {
        if (result) { result->write_ok = false; result->capture_complete = false; }
        return false;
    }
    return ok;
}
