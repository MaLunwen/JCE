/* Application-private completed-interval diagnostics. */
#ifndef JCE_ENGINE_PERF_HITCH_INTERNAL_H
#define JCE_ENGINE_PERF_HITCH_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>

#define JCE_ENGINE_PERF_RING 2048u
#define JCE_ENGINE_PERF_PHASE_TEXT 384u

typedef struct {
    int fps;
    double planned_ms, actual_ms;
    bool clock_valid;
} JceEnginePerfPacing;

typedef struct {
    double wall_ms, comparison_ms;
} JceEnginePerfSample;

/* Fixed per-Engine buckets. Drawable slots preserve all simultaneous gate
 * reasons; their counts partition intervals rather than counting one twice. */
enum {
    JCE_ENGINE_DISPOSITION_FULL, JCE_ENGINE_DISPOSITION_HEADLESS,
    JCE_ENGINE_DISPOSITION_DRAWABLE,
    JCE_ENGINE_DISPOSITION_GUARD = 10, JCE_ENGINE_DISPOSITION_FALLBACK,
    JCE_ENGINE_DISPOSITION_CLOSING, JCE_ENGINE_DISPOSITION_QUIT,
    JCE_ENGINE_DISPOSITION_CAPTURE_FAILED, JCE_ENGINE_DISPOSITION_DEVICE_FAILED,
    JCE_ENGINE_DISPOSITION_COUNT
};
enum {
    JCE_ENGINE_DRAWABLE_MINIMIZED = 1u, JCE_ENGINE_DRAWABLE_PAUSED = 2u,
    JCE_ENGINE_DRAWABLE_EXTENT = 4u
};
enum {
    JCE_ENGINE_DISPOSITION_FREQUENCY = 1u, JCE_ENGINE_DISPOSITION_COUNTER = 2u,
    JCE_ENGINE_DISPOSITION_SEQUENCE = 4u, JCE_ENGINE_DISPOSITION_OVERFLOW = 8u,
    JCE_ENGINE_DISPOSITION_UNFINISHED = 16u, JCE_ENGINE_DISPOSITION_EMPTY = 32u
};
typedef struct {
    uint64_t interval, row, begin, end;
} JceEnginePerfDispositionBoundary;
typedef struct {
    uint64_t count, ticks;
    JceEnginePerfDispositionBoundary first, last;
} JceEnginePerfDispositionBucket;
typedef struct {
    JceEnginePerfDispositionBucket buckets[JCE_ENGINE_DISPOSITION_COUNT];
    JceEnginePerfDispositionBoundary current;
    uint64_t frequency, begin, end, intervals, full, nonfull;
    uint32_t errors, kind;
    bool enabled, active, released, finished, reported;
} JceEnginePerfDisposition;

typedef struct {
    JceEnginePerfSample ring[JCE_ENGINE_PERF_RING];
    JceEnginePerfPacing current_pacing, completed_pacing;
    double factor, window_sum_ms, window_worst_ms;
    uint64_t interval;
    uint32_t head, count, warm, logged, window_count;
    bool enabled, started, iterating, previous_full, interval_eligible, observed;
    bool interval_phase_armed, previous_phase_armed;
    bool interval_completed, terminal_completed;
    JceEnginePerfDisposition disposition;
} JceEnginePerfHitch;

typedef enum {
    JCE_ENGINE_PERF_PHASE_UNAVAILABLE,
    JCE_ENGINE_PERF_PHASE_INELIGIBLE,
    JCE_ENGINE_PERF_PHASE_ZERO,
    JCE_ENGINE_PERF_PHASE_POSITIVE,
    JCE_ENGINE_PERF_PHASE_INVALID
} JceEnginePerfPhaseStatus;

typedef struct {
    uint64_t interval;
    double wall_ms, comparison_ms, mean_ms;
    JceEnginePerfPacing pacing;
    JceEnginePerfPhaseStatus phase_status;
    uint32_t top_count;
    bool ready, truncated;
    char phases[JCE_ENGINE_PERF_PHASE_TEXT];
} JceEnginePerfHitchReport;

typedef struct {
    uint32_t count, ring_count;
    double average_ms, worst_ms, p50_ms, p95_ms, p99_ms, p999_ms;
    bool ready;
} JceEnginePerfWindow;

/* Per-engine lifecycle, including the first disabled/enabled session. */
void jce_engine_perf_hitch_init(JceEnginePerfHitch *state, bool enabled, double factor);
/* Explicit FrameLog only. These borrow existing Engine raw counter samples;
 * they neither read a clock nor alter frame admission, snapshots or the CSV.
 * row is the writer ordinal observed at iterate entry, not a GPU/frame join. */
void jce_engine_perf_disposition_init(JceEnginePerfHitch *state, bool enabled,
    uint64_t frequency);
bool jce_engine_perf_disposition_boundary(JceEnginePerfHitch *state,
    uint64_t frequency, uint64_t boundary, uint64_t row);
void jce_engine_perf_disposition_reason(JceEnginePerfHitch *state, uint32_t kind);
void jce_engine_perf_disposition_release(JceEnginePerfHitch *state, bool full);
/* Finish closes the pending disposition at an existing terminal boundary.
 * Report is separate so writer finalization precedes once-only bounded output.
 * Neither function is evidence of reaching a requested timer deadline. */
bool jce_engine_perf_disposition_finish(JceEnginePerfHitch *state,
    uint64_t frequency, uint64_t boundary);
bool jce_engine_perf_disposition_report(JceEnginePerfHitch *state);
/* Main-thread-only entry gate before any clock/state/callback changes.
 * Reject nested entry without changing the outer interval. A skip leaves
 * previous_full false. Prior and current phase gates must both be armed;
 * an ever-available core snapshot is not proof of a fresh enabled interval.
 * Ordinals do not depend on auto-stop. This is separate
 * from the existing renderer/interactive-resize atomic guard. */
bool jce_engine_perf_hitch_begin_interval(JceEnginePerfHitch *state);
void jce_engine_perf_hitch_end_interval(JceEnginePerfHitch *state, bool full);
/* Complete the current fully rendered iteration without entering another one.
 * The caller owns the terminal wall-counter boundary and holds the entry gate.
 * Rotate its pending phases once; a second completion changes no state. */
bool jce_engine_perf_hitch_complete_terminal_interval(JceEnginePerfHitch *state);
/* Sole end-of-frame wait owner records the selected rate and measured wait.
 * This is interval evidence, never a scheduler or CPU-time measurement. */
void jce_engine_perf_hitch_record_pacing(JceEnginePerfHitch *state,
    const JceEnginePerfPacing *pacing);
/* Called at most once for a full rendering iteration, after the sole engine
 * snapshot tick. Invalid wall clock forbids a planned-wait deduction but
 * preserves a valid raw wall observation. Does not tick/reset the phase owner. */
bool jce_engine_perf_hitch_observe_completed(JceEnginePerfHitch *state, double wall_ms,
    bool wall_clock_valid,
    JceEnginePerfHitchReport *hitch, JceEnginePerfWindow *window);
/* Formats only captured values, never the next mutable phase snapshot.
 * Names are bounded ASCII escapes; no async GPU/CPU frame join is invented. */
bool jce_engine_perf_hitch_format(const JceEnginePerfHitchReport *report,
    char *out, uint32_t capacity);
#endif
