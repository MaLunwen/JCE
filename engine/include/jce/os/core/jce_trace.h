/*
 * jce_trace.h - Low-overhead runtime observability.
 *
 * Producers write fixed-size events into a bounded overwrite ring. Event
 * publication performs no heap allocation and never waits for a consumer.
 * Tracy, the editor profiler, structured logs, and Chrome Trace exports can
 * therefore consume one common task/thread timeline.
 */

#ifndef JCE_TRACE_H
#define JCE_TRACE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_TRACE_NAME_CAP 48u

typedef enum JceTraceCategory {
    JCE_TRACE_CATEGORY_CORE      = 0,
    JCE_TRACE_CATEGORY_FRAME     = 1,
    JCE_TRACE_CATEGORY_THREAD    = 2,
    JCE_TRACE_CATEGORY_TASK      = 3,
    JCE_TRACE_CATEGORY_WAIT      = 4,
    JCE_TRACE_CATEGORY_RENDER    = 5,
    JCE_TRACE_CATEGORY_GPU_SCENE = 6
} JceTraceCategory;

typedef enum JceTraceEventType {
    JCE_TRACE_EVENT_INVALID           = 0,
    JCE_TRACE_EVENT_THREAD_REGISTER   = 1,
    JCE_TRACE_EVENT_THREAD_UNREGISTER = 2,
    JCE_TRACE_EVENT_TASK_SUBMIT       = 3,
    JCE_TRACE_EVENT_TASK_BEGIN        = 4,
    JCE_TRACE_EVENT_TASK_END          = 5,
    JCE_TRACE_EVENT_WAIT_BEGIN        = 6,
    JCE_TRACE_EVENT_WAIT_END          = 7,
    JCE_TRACE_EVENT_FRAME             = 8,
    JCE_TRACE_EVENT_COUNTER           = 9
} JceTraceEventType;

typedef enum JceTraceTaskKind {
    JCE_TRACE_TASK_FRAME_JOB = 0,
    JCE_TRACE_TASK_ASYNC     = 1,
    JCE_TRACE_TASK_SERVICE   = 2
} JceTraceTaskKind;

typedef enum JceTraceTaskState {
    JCE_TRACE_TASK_SUCCEEDED = 0,
    JCE_TRACE_TASK_FAILED    = 1,
    JCE_TRACE_TASK_CANCELLED = 2
} JceTraceTaskState;

typedef struct JceTraceEvent {
    uint32_t          sequence;
    JceTraceEventType type;
    JceTraceCategory  category;
    uint32_t          flags;
    uint64_t          timestamp_ns;
    uint64_t          thread_id;
    uint64_t          id;
    uint64_t          parent_id;
    uint64_t          value_u64;
    uint32_t          value_u32[2];
    char              name[JCE_TRACE_NAME_CAP];
} JceTraceEvent;

/*
 * A cursor is consumer-owned. Keep one per editor/tool consumer. When a
 * consumer falls behind the bounded ring, read() advances it to the oldest
 * resident event and accumulates the skipped count in lost_events.
 */
typedef struct JceTraceCursor {
    uint32_t next_sequence;
    uint64_t lost_events;
} JceTraceCursor;

typedef struct JceTraceStats {
    bool     enabled;
    uint32_t capacity;
    uint32_t resident_events;
    uint64_t recorded_events;
    uint32_t active_threads;
    uint32_t active_work_items;
    uint32_t active_waits;
    uint64_t tasks_submitted;
    uint64_t work_items_started;
    uint64_t work_items_completed;
    uint64_t waits_completed;
    uint64_t max_queue_time_ns;
    uint64_t max_run_time_ns;
    uint64_t max_wait_time_ns;
    uint64_t last_wall_frame_ns;
    uint64_t last_sim_frame_ns;
    uint64_t last_frame_index;
} JceTraceStats;

JCE_API void JCE_CALL jce_trace_set_enabled(bool enabled);
JCE_API bool JCE_CALL jce_trace_enabled(void);

/* Reset is intended for tests or an explicit capture start. Producers must be
 * quiescent while it runs. The enabled state is preserved. */
JCE_API void JCE_CALL jce_trace_reset(void);

/* Process-unique for the current capture. Zero is never returned. */
JCE_API uint64_t JCE_CALL jce_trace_next_id(void);
JCE_API uint64_t JCE_CALL jce_trace_task_current_id(void);
JCE_API void JCE_CALL jce_trace_task_restore_id(uint64_t task_id);

JCE_API void JCE_CALL jce_trace_thread_register(const char *name);
JCE_API void JCE_CALL jce_trace_thread_unregister(void);

JCE_API void JCE_CALL
jce_trace_task_submit(uint64_t task_id, uint64_t parent_id,
                      JceTraceTaskKind kind, const char *name);
JCE_API void JCE_CALL
jce_trace_task_begin(uint64_t span_id, uint64_t task_id,
                     const char *name, uint64_t queue_time_ns);
JCE_API void JCE_CALL
jce_trace_task_end(uint64_t span_id, uint64_t task_id,
                   const char *name, JceTraceTaskState state,
                   uint64_t run_time_ns);
JCE_API void JCE_CALL
jce_trace_wait_begin(uint64_t wait_id, uint64_t task_id, const char *name);
JCE_API void JCE_CALL
jce_trace_wait_end(uint64_t wait_id, uint64_t task_id, const char *name,
                   uint64_t wait_time_ns);
JCE_API void JCE_CALL
jce_trace_frame_mark(uint64_t frame_index, uint64_t wall_time_ns,
                     uint64_t simulation_time_ns);
JCE_API void JCE_CALL
jce_trace_counter(JceTraceCategory category, const char *name, int64_t value);

JCE_API void JCE_CALL jce_trace_cursor_init(JceTraceCursor *cursor);
JCE_API uint32_t JCE_CALL
jce_trace_read(JceTraceCursor *cursor, JceTraceEvent *out_events,
               uint32_t max_events);
JCE_API void JCE_CALL jce_trace_get_stats(JceTraceStats *out_stats);

/* Export the currently resident ring to Chrome/Perfetto Trace Event JSON.
 * This allocates only on the calling thread and is intended for an explicit
 * capture/export action, never a frame hot path. */
JCE_API bool JCE_CALL jce_trace_export_chrome_json(const char *host_path);

JCE_EXTERN_C_END

#endif /* JCE_TRACE_H */
