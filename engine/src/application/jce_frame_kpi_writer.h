/* Private complete-record writer. No renderer, phase table or public ABI. */
#ifndef JCE_FRAME_KPI_WRITER_INTERNAL_H
#define JCE_FRAME_KPI_WRITER_INTERNAL_H
#include <SDL3/SDL_iostream.h>
#include <jce/os/core/jce_async.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct JceFrameKpiWriter JceFrameKpiWriter;
typedef enum {
    JCE_KPI_WRITER_OPEN, JCE_KPI_WRITER_FINISHING,
    JCE_KPI_WRITER_COMPLETE, JCE_KPI_WRITER_FAILED
} JceFrameKpiWriterState;
typedef enum {
    JCE_KPI_WRITER_ERROR_NONE, JCE_KPI_WRITER_ERROR_HEADER_WRITE,
    JCE_KPI_WRITER_ERROR_HEADER_FLUSH, JCE_KPI_WRITER_ERROR_INVALID_RECORD,
    JCE_KPI_WRITER_ERROR_FULL, JCE_KPI_WRITER_ERROR_SUBMIT,
    JCE_KPI_WRITER_ERROR_CANCELLED, JCE_KPI_WRITER_ERROR_TASK,
    JCE_KPI_WRITER_ERROR_WRITE, JCE_KPI_WRITER_ERROR_FLUSH,
    JCE_KPI_WRITER_ERROR_CLOSE
} JceFrameKpiWriterError;

typedef struct {
    bool enabled;
    bool context_json; /* Fixed opt-in fourth column; legacy bytes by default. */
    SDL_IOStream *stream;
    JceAsyncExecutor *executor; /* Borrowed; owner must finish before shutdown. */
    size_t byte_capacity, drain_bytes;
    uint32_t record_capacity, drain_records, durable_interval;
    /* Either zero retains immediate dispatch. Enable both for byte/oldest-age
     * coalescing; an owner poll observes elapsed age, not a timer deadline. */
    size_t min_batch_bytes;
    uint32_t max_delay_ms;
} JceFrameKpiWriterConfig;
/* Latest physical WriteIO bracket, including the initial header. Times use
 * jce_time_ticks_ns (process monotonic wall clock), not CPU execution. A pending
 * write has no published returned time/bytes/status; zero placeholders are
 * unavailable. active is this publication frontier, not an OS-thread probe. */
typedef struct {
    uint64_t serial, started_ns, returned_ns;
    size_t requested_bytes, actual_bytes;
    SDL_IOStatus io_status;
    bool active, data, return_available;
} JceFrameKpiWriterIoObservation;
typedef struct {
    bool captured, task_state_available;
    JceAsyncState task_state;
    uint64_t observed_ns, submitted_tasks, started_tasks, completion_callbacks;
    uint64_t write_calls;
    size_t queued_bytes;
    uint32_t queued_records;
    JceFrameKpiWriterIoObservation write;
} JceFrameKpiWriterFailure;
typedef struct {
    JceFrameKpiWriterState state;
    JceFrameKpiWriterError error;
    JceAsyncExecutionMode mode;
    bool off_thread, closed, task_outstanding, owner_operation_active;
    size_t queued_bytes, peak_bytes, byte_capacity;
    uint32_t queued_records, peak_records, record_capacity;
    /* Byte totals exclude the CSV header; IO call totals include it.
     * Written bytes can contain a failed partial prefix; only COMPLETE proves
     * exact accepted/written equality plus final durability and close. */
    uint64_t accepted_records, written_records, accepted_bytes, written_bytes;
    uint64_t write_calls, flush_calls, completion_callbacks;
    /* Non-NULL submit returns/published handles vs actual worker entries. A
     * fast worker may enter before its submit return has been published. */
    uint64_t submitted_tasks, started_tasks;
    JceFrameKpiWriterIoObservation write;
    /* Captured once under the real queue lock at the first error. Later IO or
     * shutdown cannot overwrite it. Occupancy is not accepted-minus-written. */
    JceFrameKpiWriterFailure first_failure;
    /* Matching serial's return published after the snapshot, kept separately.
     * The API may already have returned while publication waited for mutex;
     * compare returned_ns with first_failure.observed_ns for actual order.
     * Equal clock readings do not establish strict order. */
    JceFrameKpiWriterIoObservation failure_write_return;
    size_t failed_requested_bytes, failed_actual_bytes;
    SDL_IOStatus failed_io_status;
    char message[160]; /* Owned first error; survives subsequent close errors. */
} JceFrameKpiWriterStatus;

/* Defaults are candidate capture budgets, not measured throughput guarantees.
 * Disabled create returns NULL without allocation/IO/ownership transfer.
 * NULL from enabled create means invalid config/allocation failure; the caller
 * still owns stream. Any non-NULL writer owns stream, including header-failed
 * writers; poll/status reveal failure and finish/destroy close it exactly once.
 * Header write + initial durability run at create, outside the measured loop. */
void jce_frame_kpi_writer_config_init(JceFrameKpiWriterConfig *config, bool low_memory);
JceFrameKpiWriter *jce_frame_kpi_writer_create(const JceFrameKpiWriterConfig *config);
/* Public mutations are owner-thread only and reject reentry without changing
 * capture state. A rejected nested operation leaves ownership intact; retry
 * after the active outer operation returns. Check owner_operation_active before
 * interpreting a false operation result as a capture failure or clearing an
 * engine/capture owner. get_status remains available during callbacks. */
/* Owner-thread only. Frame indexes begin at0 and advance contiguously. The
 * complete opaque bytes are copied before return; caller may reuse its buffer.
 * No physical write/pump/wait occurs here. A record error fails capture permanently:
 * no drops, producer waits for space, synchronous fallback or partial commit. */
bool jce_frame_kpi_writer_submit(JceFrameKpiWriter *writer, uint64_t frame,
                                const void *bytes, size_t length);
/* Owner-thread pump + schedule. Native threaded mode runs only callbacks on
 * this thread; cooperative mode can run physical IO here and is NOT off-thread.
 * Enabled coalescing dispatches on bytes, oldest age, descriptor or durability
 * boundary. Age is observed on owner poll/submit, not a storage deadline.
 * One finite LOW task exists at a time. Finite drain target may be exceeded by
 * one complete record, but never exceeds the admitted byte capacity. */
bool jce_frame_kpi_writer_poll(JceFrameKpiWriter *writer);
/* Owner-thread only. Seal, force any coalesced tail, drain own queue and actual completion callbacks,
 * flush and close. Idempotent. False means failure or rejected/deferred reentry;
 * status distinguishes these. Reentry closes/frees nothing. Must run
 * before default CANCEL_ALL and before converting a controlled quit to success.
 * Safe joining blocking SDL IO can wait indefinitely on broken storage; this
 * is not a universal bounded shutdown/deadline guarantee. */
bool jce_frame_kpi_writer_finish(JceFrameKpiWriter *writer);
/* True means safely joined/closed and memory released, even when capture
 * FAILED. NULL is already cleaned. False means non-owner/reentry/deferred:
 * memory is NOT freed; retain the pointer and retry after the outer operation.
 * Copy status/error before destroying; destroy success is not capture success. */
bool jce_frame_kpi_writer_destroy(JceFrameKpiWriter *writer);
void jce_frame_kpi_writer_get_status(JceFrameKpiWriter *writer,
                                    JceFrameKpiWriterStatus *out_status);
#endif
