/*
 * jce_trace.c - Bounded multi-producer runtime event timeline.
 */

#include <jce/os/core/jce_trace.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>

#include "jce_memory.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define TRACE_CAPACITY 16384u
#define TRACE_MASK     (TRACE_CAPACITY - 1u)

typedef struct TraceSlot {
    SDL_AtomicInt published;
    JceTraceEvent event;
} TraceSlot;

typedef struct TraceU64 {
    SDL_AtomicInt version;
    SDL_AtomicInt lo;
    SDL_AtomicInt hi;
} TraceU64;

typedef struct TraceJsonBuffer {
    char    *data;
    size_t   size;
    size_t   capacity;
} TraceJsonBuffer;

static TraceSlot s_slots[TRACE_CAPACITY];
static SDL_AtomicInt s_enabled;
static SDL_AtomicInt s_write_sequence;
static SDL_AtomicInt s_next_id;
static SDL_AtomicInt s_generation;
static SDL_AtomicInt s_active_threads;
static SDL_AtomicInt s_active_work_items;
static SDL_AtomicInt s_active_waits;
static SDL_AtomicInt s_tasks_submitted;
static SDL_AtomicInt s_work_items_started;
static SDL_AtomicInt s_work_items_completed;
static SDL_AtomicInt s_waits_completed;
static SDL_AtomicInt s_max_queue_ns;
static SDL_AtomicInt s_max_run_ns;
static SDL_AtomicInt s_max_wait_ns;
static SDL_AtomicInt s_max_queue_us;
static SDL_AtomicInt s_max_run_us;
static SDL_AtomicInt s_max_wait_us;
static SDL_AtomicInt s_max_queue_ms;
static SDL_AtomicInt s_max_run_ms;
static SDL_AtomicInt s_max_wait_ms;
static TraceU64 s_last_wall_ns;
static TraceU64 s_last_sim_ns;
static TraceU64 s_last_frame_index;
static SDL_TLSID s_thread_generation;
static SDL_TLSID s_thread_trace_active;
static SDL_TLSID s_current_task;

static uint64_t trace_u64_load(const TraceU64 *value)
{
    int before = 0;
    int after = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;

    do {
        before = SDL_GetAtomicInt((SDL_AtomicInt *)&value->version);
        if ((before & 1) != 0)
            continue;
        SDL_MemoryBarrierAcquire();
        lo = (uint32_t)SDL_GetAtomicInt((SDL_AtomicInt *)&value->lo);
        hi = (uint32_t)SDL_GetAtomicInt((SDL_AtomicInt *)&value->hi);
        SDL_MemoryBarrierAcquire();
        after = SDL_GetAtomicInt((SDL_AtomicInt *)&value->version);
    } while (before != after || (after & 1) != 0);

    return ((uint64_t)hi << 32u) | (uint64_t)lo;
}

static void trace_u64_store(TraceU64 *value, uint64_t input)
{
    SDL_AddAtomicInt(&value->version, 1);
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&value->lo, (int)(uint32_t)input);
    SDL_SetAtomicInt(&value->hi, (int)(uint32_t)(input >> 32u));
    SDL_MemoryBarrierRelease();
    SDL_AddAtomicInt(&value->version, 1);
}

static void trace_atomic_max(SDL_AtomicInt *target, uint64_t value)
{
    int desired = value > (uint64_t)INT_MAX ? INT_MAX : (int)value;
    int current = SDL_GetAtomicInt(target);

    while (desired > current &&
           !SDL_CompareAndSwapAtomicInt(target, current, desired))
        current = SDL_GetAtomicInt(target);
}

static void trace_duration_max(SDL_AtomicInt *nanoseconds,
                               SDL_AtomicInt *microseconds,
                               SDL_AtomicInt *milliseconds,
                               uint64_t value_ns)
{
    trace_atomic_max(nanoseconds, value_ns);
    trace_atomic_max(microseconds, value_ns / UINT64_C(1000));
    trace_atomic_max(milliseconds, value_ns / UINT64_C(1000000));
}

static uint64_t trace_duration_load(SDL_AtomicInt *nanoseconds,
                                    SDL_AtomicInt *microseconds,
                                    SDL_AtomicInt *milliseconds)
{
    uint64_t exact_ns =
        (uint32_t)SDL_GetAtomicInt(nanoseconds);
    uint64_t coarse_us =
        (uint64_t)(uint32_t)SDL_GetAtomicInt(microseconds) * UINT64_C(1000);
    uint64_t coarse_ms =
        (uint64_t)(uint32_t)SDL_GetAtomicInt(milliseconds) * UINT64_C(1000000);
    uint64_t result = exact_ns > coarse_us ? exact_ns : coarse_us;

    return result > coarse_ms ? result : coarse_ms;
}

static uint64_t trace_current_task_id(void)
{
    return (uint64_t)(uintptr_t)SDL_GetTLS(&s_current_task);
}

static void trace_set_current_task_id(uint64_t id)
{
    SDL_SetTLS(&s_current_task, (void *)(uintptr_t)id, NULL);
}

static void trace_copy_name(char dst[JCE_TRACE_NAME_CAP], const char *name)
{
    SDL_strlcpy(dst, name && name[0] ? name : "unnamed",
                JCE_TRACE_NAME_CAP);
}

static void trace_emit(JceTraceEventType type,
                       JceTraceCategory category,
                       uint64_t id,
                       uint64_t parent_id,
                       uint64_t value_u64,
                       uint32_t value0,
                       uint32_t value1,
                       const char *name)
{
    JceTraceEvent event;
    TraceSlot *slot;
    uint32_t sequence;

    if (SDL_GetAtomicInt(&s_enabled) == 0)
        return;

    SDL_zero(event);
    sequence = (uint32_t)(SDL_AddAtomicInt(&s_write_sequence, 1) + 1);
    if (sequence == 0u)
        sequence = (uint32_t)(SDL_AddAtomicInt(&s_write_sequence, 1) + 1);

    event.sequence = sequence;
    event.type = type;
    event.category = category;
    event.timestamp_ns = jce_time_ticks_ns();
    event.thread_id = jce_thread_current_id();
    event.id = id;
    event.parent_id = parent_id;
    event.value_u64 = value_u64;
    event.value_u32[0] = value0;
    event.value_u32[1] = value1;
    trace_copy_name(event.name, name);

    slot = &s_slots[(sequence - 1u) & TRACE_MASK];
    SDL_SetAtomicInt(&slot->published, 0);
    slot->event = event;
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&slot->published, (int)sequence);
}

void jce_trace_set_enabled(bool enabled)
{
    int was_enabled = SDL_GetAtomicInt(&s_enabled);

    if (enabled && was_enabled == 0)
        SDL_AddAtomicInt(&s_generation, 1);
    SDL_SetAtomicInt(&s_enabled, enabled ? 1 : 0);
}

bool jce_trace_enabled(void)
{
    return SDL_GetAtomicInt(&s_enabled) != 0;
}

void jce_trace_reset(void)
{
    uint32_t i;

    for (i = 0; i < TRACE_CAPACITY; ++i)
        SDL_SetAtomicInt(&s_slots[i].published, 0);
    SDL_SetAtomicInt(&s_write_sequence, 0);
    SDL_SetAtomicInt(&s_next_id, 0);
    SDL_SetAtomicInt(&s_active_threads, 0);
    SDL_SetAtomicInt(&s_active_work_items, 0);
    SDL_SetAtomicInt(&s_active_waits, 0);
    SDL_SetAtomicInt(&s_tasks_submitted, 0);
    SDL_SetAtomicInt(&s_work_items_started, 0);
    SDL_SetAtomicInt(&s_work_items_completed, 0);
    SDL_SetAtomicInt(&s_waits_completed, 0);
    SDL_SetAtomicInt(&s_max_queue_ns, 0);
    SDL_SetAtomicInt(&s_max_run_ns, 0);
    SDL_SetAtomicInt(&s_max_wait_ns, 0);
    SDL_SetAtomicInt(&s_max_queue_us, 0);
    SDL_SetAtomicInt(&s_max_run_us, 0);
    SDL_SetAtomicInt(&s_max_wait_us, 0);
    SDL_SetAtomicInt(&s_max_queue_ms, 0);
    SDL_SetAtomicInt(&s_max_run_ms, 0);
    SDL_SetAtomicInt(&s_max_wait_ms, 0);
    trace_u64_store(&s_last_wall_ns, 0);
    trace_u64_store(&s_last_sim_ns, 0);
    trace_u64_store(&s_last_frame_index, 0);
    SDL_AddAtomicInt(&s_generation, 1);
    trace_set_current_task_id(0);
}

uint64_t jce_trace_next_id(void)
{
    uint32_t id = (uint32_t)(SDL_AddAtomicInt(&s_next_id, 1) + 1);

    if (id == 0u)
        id = (uint32_t)(SDL_AddAtomicInt(&s_next_id, 1) + 1);
    return (uint64_t)id;
}

uint64_t jce_trace_task_current_id(void)
{
    return trace_current_task_id();
}

void jce_trace_task_restore_id(uint64_t task_id)
{
    trace_set_current_task_id(task_id);
}

void jce_trace_thread_register(const char *name)
{
    uintptr_t generation =
        (uintptr_t)(uint32_t)SDL_GetAtomicInt(&s_generation) + 1u;
    uintptr_t registered =
        (uintptr_t)SDL_GetTLS(&s_thread_generation);

    if (registered == generation)
        return;
    SDL_SetTLS(&s_thread_generation, (void *)generation, NULL);
    SDL_SetTLS(&s_thread_trace_active, NULL, NULL);
    jce_log_set_thread_name(name && name[0] ? name : "jce-thread");
    JCE_PROFILE_THREAD_NAME(name && name[0] ? name : "jce-thread");

    if (!jce_trace_enabled())
        return;
    SDL_SetTLS(&s_thread_trace_active, (void *)(uintptr_t)1u, NULL);
    SDL_AddAtomicInt(&s_active_threads, 1);
    trace_emit(JCE_TRACE_EVENT_THREAD_REGISTER,
               JCE_TRACE_CATEGORY_THREAD,
               jce_thread_current_id(), 0, 0, 0, 0, name);
}

void jce_trace_thread_unregister(void)
{
    uintptr_t generation =
        (uintptr_t)(uint32_t)SDL_GetAtomicInt(&s_generation) + 1u;
    uintptr_t registered =
        (uintptr_t)SDL_GetTLS(&s_thread_generation);
    uintptr_t trace_active =
        (uintptr_t)SDL_GetTLS(&s_thread_trace_active);

    if (registered == 0u)
        return;
    if (trace_active != 0u && registered == generation) {
        trace_emit(JCE_TRACE_EVENT_THREAD_UNREGISTER,
                   JCE_TRACE_CATEGORY_THREAD,
                   jce_thread_current_id(), 0, 0, 0, 0, "thread-exit");
        SDL_AddAtomicInt(&s_active_threads, -1);
    }
    SDL_SetTLS(&s_thread_generation, NULL, NULL);
    SDL_SetTLS(&s_thread_trace_active, NULL, NULL);
}

void jce_trace_task_submit(uint64_t task_id, uint64_t parent_id,
                           JceTraceTaskKind kind, const char *name)
{
    if (!jce_trace_enabled())
        return;
    if (task_id == 0)
        return;
    if (parent_id == 0)
        parent_id = trace_current_task_id();
    SDL_AddAtomicInt(&s_tasks_submitted, 1);
    trace_emit(JCE_TRACE_EVENT_TASK_SUBMIT, JCE_TRACE_CATEGORY_TASK,
               task_id, parent_id, 0, (uint32_t)kind, 0, name);
}

void jce_trace_task_begin(uint64_t span_id, uint64_t task_id,
                          const char *name, uint64_t queue_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (span_id == 0 || task_id == 0)
        return;
    SDL_AddAtomicInt(&s_active_work_items, 1);
    SDL_AddAtomicInt(&s_work_items_started, 1);
    trace_duration_max(&s_max_queue_ns, &s_max_queue_us, &s_max_queue_ms,
                       queue_time_ns);
    trace_set_current_task_id(task_id);
    trace_emit(JCE_TRACE_EVENT_TASK_BEGIN, JCE_TRACE_CATEGORY_TASK,
               span_id, task_id, queue_time_ns, 0, 0, name);
}

void jce_trace_task_end(uint64_t span_id, uint64_t task_id,
                        const char *name, JceTraceTaskState state,
                        uint64_t run_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (span_id == 0 || task_id == 0)
        return;
    trace_duration_max(&s_max_run_ns, &s_max_run_us, &s_max_run_ms,
                       run_time_ns);
    trace_emit(JCE_TRACE_EVENT_TASK_END, JCE_TRACE_CATEGORY_TASK,
               span_id, task_id, run_time_ns, (uint32_t)state, 0, name);
    trace_set_current_task_id(0);
    SDL_AddAtomicInt(&s_active_work_items, -1);
    SDL_AddAtomicInt(&s_work_items_completed, 1);
}

void jce_trace_wait_begin(uint64_t wait_id, uint64_t task_id,
                          const char *name)
{
    if (!jce_trace_enabled())
        return;
    if (wait_id == 0)
        return;
    if (task_id == 0)
        task_id = trace_current_task_id();
    SDL_AddAtomicInt(&s_active_waits, 1);
    trace_emit(JCE_TRACE_EVENT_WAIT_BEGIN, JCE_TRACE_CATEGORY_WAIT,
               wait_id, task_id, 0, 0, 0, name);
}

void jce_trace_wait_end(uint64_t wait_id, uint64_t task_id,
                        const char *name, uint64_t wait_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (wait_id == 0)
        return;
    trace_duration_max(&s_max_wait_ns, &s_max_wait_us, &s_max_wait_ms,
                       wait_time_ns);
    trace_emit(JCE_TRACE_EVENT_WAIT_END, JCE_TRACE_CATEGORY_WAIT,
               wait_id, task_id, wait_time_ns, 0, 0, name);
    SDL_AddAtomicInt(&s_active_waits, -1);
    SDL_AddAtomicInt(&s_waits_completed, 1);
}

void jce_trace_frame_mark(uint64_t frame_index, uint64_t wall_time_ns,
                          uint64_t simulation_time_ns)
{
    if (!jce_trace_enabled())
        return;
    trace_u64_store(&s_last_frame_index, frame_index);
    trace_u64_store(&s_last_wall_ns, wall_time_ns);
    trace_u64_store(&s_last_sim_ns, simulation_time_ns);
    JCE_PROFILE_PLOT("frame.wall_ms", (double)wall_time_ns / 1000000.0);
    JCE_PROFILE_PLOT("frame.sim_ms", (double)simulation_time_ns / 1000000.0);
    trace_emit(JCE_TRACE_EVENT_FRAME, JCE_TRACE_CATEGORY_FRAME,
               frame_index, 0, wall_time_ns,
               simulation_time_ns > UINT32_MAX
                   ? UINT32_MAX : (uint32_t)simulation_time_ns,
               0, "frame");
}

void jce_trace_counter(JceTraceCategory category, const char *name,
                       int64_t value)
{
    JCE_PROFILE_PLOT_I(name && name[0] ? name : "trace.counter", value);
    trace_emit(JCE_TRACE_EVENT_COUNTER, category, 0, 0,
               (uint64_t)value, 0, 0, name);
}

void jce_trace_cursor_init(JceTraceCursor *cursor)
{
    if (!cursor)
        return;
    cursor->next_sequence = 0;
    cursor->lost_events = 0;
}

uint32_t jce_trace_read(JceTraceCursor *cursor, JceTraceEvent *out_events,
                        uint32_t max_events)
{
    uint32_t end;
    uint32_t earliest;
    uint32_t sequence;
    uint32_t count = 0;

    if (!cursor || !out_events || max_events == 0)
        return 0;
    end = (uint32_t)SDL_GetAtomicInt(&s_write_sequence);
    if (end == 0)
        return 0;
    earliest = end >= TRACE_CAPACITY ? end - TRACE_CAPACITY + 1u : 1u;
    sequence = cursor->next_sequence;
    if (sequence == 0)
        sequence = earliest;
    else if (sequence < earliest) {
        cursor->lost_events += (uint64_t)(earliest - sequence);
        sequence = earliest;
    }

    while (sequence <= end && count < max_events) {
        TraceSlot *slot = &s_slots[(sequence - 1u) & TRACE_MASK];
        int published = SDL_GetAtomicInt(&slot->published);

        SDL_MemoryBarrierAcquire();
        if ((uint32_t)published == sequence) {
            JceTraceEvent event = slot->event;
            SDL_MemoryBarrierAcquire();
            if ((uint32_t)SDL_GetAtomicInt(&slot->published) == sequence &&
                event.sequence == sequence) {
                out_events[count++] = event;
            } else {
                cursor->lost_events++;
            }
        } else {
            cursor->lost_events++;
        }
        sequence++;
    }
    cursor->next_sequence = sequence;
    return count;
}

void jce_trace_get_stats(JceTraceStats *out_stats)
{
    uint32_t written;

    if (!out_stats)
        return;
    SDL_zero(*out_stats);
    written = (uint32_t)SDL_GetAtomicInt(&s_write_sequence);
    out_stats->enabled = jce_trace_enabled();
    out_stats->capacity = TRACE_CAPACITY;
    out_stats->resident_events =
        written < TRACE_CAPACITY ? written : TRACE_CAPACITY;
    out_stats->recorded_events = written;
    out_stats->active_threads =
        (uint32_t)SDL_GetAtomicInt(&s_active_threads);
    out_stats->active_work_items =
        (uint32_t)SDL_GetAtomicInt(&s_active_work_items);
    out_stats->active_waits =
        (uint32_t)SDL_GetAtomicInt(&s_active_waits);
    out_stats->tasks_submitted =
        (uint32_t)SDL_GetAtomicInt(&s_tasks_submitted);
    out_stats->work_items_started =
        (uint32_t)SDL_GetAtomicInt(&s_work_items_started);
    out_stats->work_items_completed =
        (uint32_t)SDL_GetAtomicInt(&s_work_items_completed);
    out_stats->waits_completed =
        (uint32_t)SDL_GetAtomicInt(&s_waits_completed);
    out_stats->max_queue_time_ns = trace_duration_load(
        &s_max_queue_ns, &s_max_queue_us, &s_max_queue_ms);
    out_stats->max_run_time_ns = trace_duration_load(
        &s_max_run_ns, &s_max_run_us, &s_max_run_ms);
    out_stats->max_wait_time_ns = trace_duration_load(
        &s_max_wait_ns, &s_max_wait_us, &s_max_wait_ms);
    out_stats->last_wall_frame_ns = trace_u64_load(&s_last_wall_ns);
    out_stats->last_sim_frame_ns = trace_u64_load(&s_last_sim_ns);
    out_stats->last_frame_index = trace_u64_load(&s_last_frame_index);
}

static bool trace_json_reserve(TraceJsonBuffer *buffer, size_t extra)
{
    size_t need;
    size_t capacity;
    char *grown;

    if (extra > SIZE_MAX - buffer->size)
        return false;
    need = buffer->size + extra;
    if (need <= buffer->capacity)
        return true;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < need) {
        if (capacity > SIZE_MAX / 2u)
            return false;
        capacity *= 2u;
    }
    grown = (char *)JCE_REALLOC(buffer->data, capacity);
    if (!grown)
        return false;
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool trace_json_append(TraceJsonBuffer *buffer, const char *text)
{
    size_t length = SDL_strlen(text);

    if (!trace_json_reserve(buffer, length + 1u))
        return false;
    SDL_memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return true;
}

static bool trace_json_append_format(TraceJsonBuffer *buffer,
                                     const char *format,
                                     unsigned long long a,
                                     unsigned long long b,
                                     unsigned long long c)
{
    char temp[384];
    int length = SDL_snprintf(temp, sizeof(temp), format, a, b, c);

    if (length < 0 || (size_t)length >= sizeof(temp))
        return false;
    return trace_json_append(buffer, temp);
}

static bool trace_json_append_escaped(TraceJsonBuffer *buffer,
                                      const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (!trace_json_append(buffer, "\""))
        return false;
    while (*cursor) {
        char temp[8];

        if (*cursor == '"' || *cursor == '\\') {
            temp[0] = '\\';
            temp[1] = (char)*cursor;
            temp[2] = '\0';
        } else if (*cursor < 0x20u) {
            SDL_snprintf(temp, sizeof(temp), "\\u%04x", (unsigned)*cursor);
        } else {
            temp[0] = (char)*cursor;
            temp[1] = '\0';
        }
        if (!trace_json_append(buffer, temp))
            return false;
        cursor++;
    }
    return trace_json_append(buffer, "\"");
}

static const char *trace_category_name(JceTraceCategory category)
{
    switch (category) {
    case JCE_TRACE_CATEGORY_FRAME:     return "frame";
    case JCE_TRACE_CATEGORY_THREAD:    return "thread";
    case JCE_TRACE_CATEGORY_TASK:      return "task";
    case JCE_TRACE_CATEGORY_WAIT:      return "wait";
    case JCE_TRACE_CATEGORY_RENDER:    return "render";
    case JCE_TRACE_CATEGORY_GPU_SCENE: return "gpu_scene";
    default:                           return "core";
    }
}

static bool trace_export_event(TraceJsonBuffer *json,
                               const JceTraceEvent *event,
                               bool *first)
{
    const char *phase;
    const char *event_name = event->name;
    double timestamp_us = (double)event->timestamp_ns / 1000.0;
    char prefix[256];
    char suffix[256];
    int length;

    if (!*first && !trace_json_append(json, ","))
        return false;
    *first = false;

    if (event->type == JCE_TRACE_EVENT_THREAD_REGISTER) {
        length = SDL_snprintf(
            prefix, sizeof(prefix),
            "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
            "\"tid\":%llu,\"args\":{\"name\":",
            (unsigned long long)event->thread_id);
        if (length < 0 || (size_t)length >= sizeof(prefix) ||
            !trace_json_append(json, prefix) ||
            !trace_json_append_escaped(json, event_name) ||
            !trace_json_append(json, "}}"))
            return false;
        return true;
    }

    switch (event->type) {
    case JCE_TRACE_EVENT_TASK_BEGIN:
    case JCE_TRACE_EVENT_WAIT_BEGIN:
        phase = "B";
        break;
    case JCE_TRACE_EVENT_TASK_END:
    case JCE_TRACE_EVENT_WAIT_END:
        phase = "E";
        break;
    case JCE_TRACE_EVENT_COUNTER:
        phase = "C";
        break;
    default:
        phase = "i";
        break;
    }

    length = SDL_snprintf(prefix, sizeof(prefix), "{\"name\":");
    if (length < 0 || !trace_json_append(json, prefix) ||
        !trace_json_append_escaped(json, event_name))
        return false;
    length = SDL_snprintf(
        suffix, sizeof(suffix),
        ",\"cat\":\"%s\",\"ph\":\"%s\",\"pid\":1,\"tid\":%llu,"
        "\"ts\":%.3f",
        trace_category_name(event->category), phase,
        (unsigned long long)event->thread_id, timestamp_us);
    if (length < 0 || (size_t)length >= sizeof(suffix) ||
        !trace_json_append(json, suffix))
        return false;

    if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
        if (!trace_json_append_format(
                json,
                ",\"s\":\"t\",\"args\":{\"task\":%llu,\"parent\":%llu,"
                "\"kind\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->parent_id,
                (unsigned long long)event->value_u32[0]))
            return false;
    } else if (event->type == JCE_TRACE_EVENT_COUNTER) {
        length = SDL_snprintf(
            suffix, sizeof(suffix),
            ",\"args\":{\"value\":%lld,\"id\":%llu,\"parent\":%llu}}",
            (long long)(int64_t)event->value_u64,
            (unsigned long long)event->id,
            (unsigned long long)event->parent_id);
        if (length < 0 || (size_t)length >= sizeof(suffix) ||
            !trace_json_append(json, suffix))
            return false;
    } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN ||
               event->type == JCE_TRACE_EVENT_TASK_END ||
               event->type == JCE_TRACE_EVENT_WAIT_BEGIN ||
               event->type == JCE_TRACE_EVENT_WAIT_END) {
        if (!trace_json_append_format(
                json, ",\"args\":{\"id\":%llu,\"parent\":%llu,\"ns\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->parent_id,
                (unsigned long long)event->value_u64))
            return false;
    } else {
        if (!trace_json_append_format(
                json, ",\"s\":\"t\",\"args\":{\"id\":%llu,\"value\":%llu,"
                "\"aux\":%llu}}",
                (unsigned long long)event->id,
                (unsigned long long)event->value_u64,
                (unsigned long long)event->value_u32[0]))
            return false;
    }
    return true;
}

bool jce_trace_export_chrome_json(const char *host_path)
{
    JceTraceEvent *events;
    JceTraceCursor cursor;
    TraceJsonBuffer json;
    uint32_t count;
    uint32_t i;
    bool first = true;
    bool ok;

    if (!host_path || !host_path[0])
        return false;
    events = JCE_NEW_ARRAY(JceTraceEvent, TRACE_CAPACITY);
    if (!events)
        return false;
    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events, TRACE_CAPACITY);
    SDL_zero(json);
    ok = trace_json_append(&json, "{\"displayTimeUnit\":\"ms\","
                                  "\"traceEvents\":[");
    for (i = 0; ok && i < count; ++i)
        ok = trace_export_event(&json, &events[i], &first);
    if (ok)
        ok = trace_json_append(&json, "]}");
    if (ok)
        ok = jce_fs_host_write_all(host_path, json.data, (uint64_t)json.size);
    JCE_FREE(json.data);
    JCE_FREE(events);
    return ok;
}
