#ifndef JCE_TRACE_CAPTURE_INTERNAL_H
#define JCE_TRACE_CAPTURE_INTERNAL_H
#include <jce/os/core/jce_trace.h>
#include <stddef.h>

typedef struct JceTraceCapture JceTraceCapture;
/* The existing Chrome encoder's reusable storage. limit==0 retains the
 * resident-export contract; continuous capture supplies a hard byte limit.
 * The consumer owns data and releases it once after joining its writer. */
typedef struct TraceJsonBuffer {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
} TraceJsonBuffer;

/* Actual exporter bounds: a 47-byte name escapes to at most 282 bytes;
 * the 8-byte prefix, 255-byte suffix and 383-byte args format plus comma
 * remain below 1024. Invalid unterminated names are rejected by the owner. */
#define JCE_TRACE_CHROME_EVENT_MAX_BYTES 1024u

typedef enum JceTraceCaptureMode {
    JCE_TRACE_CAPTURE_THREADED = 0,
    JCE_TRACE_CAPTURE_COOPERATIVE = 1
} JceTraceCaptureMode;
typedef struct JceTraceCaptureResult {
    JceTraceCaptureMode mode;
    bool worker_available;
    bool capture_complete;
    bool published;
    uint64_t overwritten_before_capture;
    uint64_t cursor_lost;
    uint64_t captured_events;
    uint64_t lost_events;
    uint64_t unexported_events;
    bool write_ok;
    bool generation_stable;
    bool sequence_exhausted;
    JceTraceStats final_stats;
} JceTraceCaptureResult;

/* Owner-thread calls only; caller exclusively owns the output destination.
 * Exclusive host creation refuses existing files/directories and reserves the
 * exact final path with invalid-JSON PENDING bytes. The same owner exclusively
 * creates its sibling .partial payload and holds one SDL stream through header,
 * payload batches and footer until one final close after worker join. Accepted
 * writes are not a durability certificate; publication requires footer write,
 * final close and unchanged reservation bytes, then replaces only that marker.
 * Failed/interrupted marker and partial files remain diagnostic evidence, even
 * when a partial contains positive JSON. Only a valid requested final path is
 * published proof. Caller-exclusive use is required; marker read/rename does
 * not protect against malicious external replacement or promise durability.
 * Start after enabling/resetting trace, before
 * producers. poll_ms is bounded 1..1000. Never reset during capture.
 * Threaded payload batches coalesce across polls within the bounded staging
 * buffer. At 1MiB or 16ms from its first encoded pending event, the worker
 * requests a flush. New events do not renew that deadline. Millisecond sleep
 * rounding, scheduling and synchronous IO may exceed it: this is not a hard
 * completion bound. Cooperative/Web poll behavior remains immediate.
 * Footer writer_diagnostics version 1 observes attempted payload stream writes
 * only, excluding reservation/header/footer/close. Size counts use inclusive
 * upper bounds 4096/16384/65536/262144/1048576 bytes then overflow; wall counts
 * use 100000/1000000/4000000/16000000/100000000/1000000000ns then overflow.
 * Slowest sample declares clock_domain=process-perf-ns and shares the Trace
 * event performance-clock axis (Chrome ts is in microseconds). Unavailable or
 * reversed reads reject publication; written staging is never replayed. Older
 * untagged samples have an unknown epoch. Flush deadlines and local encode,
 * drain/join/close durations retain their SDL-lifetime clock. Sample bytes
 * are requested, not a failed write's measured physical transfer. No GPU or
 * durable-IO proof is supplied, and writer diagnostics do not recursively emit.
 * Stop AFTER other producers join, while trace remains enabled; it joins its
 * worker and force-flushes once more on the owner to include unregister/join events.
 * true requires successful writing, stable generation, no lost/unexported
 * events and zero active threads/work/waits. false means incomplete capture;
 * result remains truthful even when
 * a write failure prevents metadata/footer publication. No GPU completion. */
JceTraceCapture *jce_trace_capture_start(const char *path, uint32_t poll_ms);
JceTraceCapture *jce_trace_capture_start_mode(const char *path, uint32_t poll_ms,
                                            JceTraceCaptureMode mode);
/* Owner-thread cooperative pump; NULL/threaded captures are no-ops. */
void jce_trace_capture_poll(JceTraceCapture *capture);
/* Owner-thread terminal barrier: join only the capture worker, retaining the
 * stream/cursor/staging for stop's final drain. NULL/idempotent. Allows the host
 * to retire the last diagnostic producer before unregister/final publication;
 * late ring events remain eligible. Stop still calls this barrier itself.
 * worker_available in the final result describes startup, not current liveness. */
void jce_trace_capture_quiesce(JceTraceCapture *capture);
bool jce_trace_capture_stop(JceTraceCapture *capture,
                            JceTraceCaptureResult *result);

/* Private per-read observation over the same ring/reader. These snapshots
 * describe reserved-sequence progress and why that read stopped, not producer
 * completion, an overwrite timestamp or causality. No producer work is added. */
typedef enum JceTraceReadStop {
    JCE_TRACE_READ_CAUGHT_UP = 0,
    JCE_TRACE_READ_BATCH_LIMIT = 1,
    JCE_TRACE_READ_SLOT_BUSY = 2,
    JCE_TRACE_READ_RESERVED_PENDING = 3,
    JCE_TRACE_READ_INVALID_ARGS = 4,
    JCE_TRACE_READ_STOP_COUNT = 5
} JceTraceReadStop;
typedef struct JceTraceReadObservation {
    uint32_t write_sequence, earliest_sequence;
    uint32_t next_before, next_after, count;
    JceTraceReadStop stop;
    uint32_t blocked_sequence, published_sequence;
    bool published_valid;
    uint64_t lost_delta;
} JceTraceReadObservation;
uint32_t jce_trace_read_observed_internal(JceTraceCursor *cursor,
    JceTraceEvent *events, uint32_t maximum, JceTraceReadObservation *observation);

/* Shared encoder/identity owned by jce_trace.c, not a second event registry. */
bool jce_trace_chrome_encode_batch_into(const JceTraceEvent *events,
                                       uint32_t count, bool *first,
                                       TraceJsonBuffer *json);
bool jce_trace_chrome_encode_batch(const JceTraceEvent *events, uint32_t count,
                                  bool *first, char **text, size_t *size);
uint32_t jce_trace_capture_generation(void);
/* Permanent sequence exhaustion rejects every consumer until quiescent reset. */
bool jce_trace_capture_valid_internal(void);
/* Nestable current-thread suppression; failure forbids unguarded capture IO. */
bool jce_trace_suppress_begin(void);
bool jce_trace_suppress_end(void);
#endif
