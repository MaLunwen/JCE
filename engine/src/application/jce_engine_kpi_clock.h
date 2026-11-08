/* Private completed-boundary arithmetic and explicit timed KPI capture ledger.
 * Both borrow the engine monotonic counter/frequency, never own a clock. */
#ifndef JCE_ENGINE_KPI_CLOCK_H
#define JCE_ENGINE_KPI_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Zero is a valid counter boundary. Subtract only after ordering admission;
 * floating conversion precedes millisecond scaling, so uint64 cannot wrap. */
static inline bool jce_engine_frame_clock_wall_ms(uint64_t frequency,
    uint64_t begin, uint64_t end, double *out_ms)
{
    if (!out_ms || !frequency || end < begin) return false;
    *out_ms = (double)(end - begin) * 1000.0 / (double)frequency;
    return true;
}

enum {
    JCE_KPI_CLOCK_FREQUENCY = 1u << 0,
    JCE_KPI_CLOCK_COUNTER = 1u << 1,
    JCE_KPI_CLOCK_SEQUENCE = 1u << 2,
    JCE_KPI_CLOCK_GAP = 1u << 3,
    JCE_KPI_CLOCK_OVERFLOW = 1u << 4,
    JCE_KPI_CLOCK_TERMINAL = 1u << 5
};
typedef struct JceEngineKpiClock {
    bool enabled, reported, checked;
    uint32_t errors;
    uint64_t frequency, timer_start, timer_checked;
    uint64_t render_start, render_end, render_ticks, render_intervals;
    uint64_t omitted_ticks;
} JceEngineKpiClock;

static inline void jce_engine_kpi_clock_init(JceEngineKpiClock *clock,
    bool enabled, uint64_t frequency, uint64_t timer_start)
{
    if (!clock) return;
    memset(clock, 0, sizeof(*clock));
    clock->enabled = enabled;
    if (!enabled) return;
    clock->frequency = frequency;
    clock->timer_start = timer_start;
    if (!frequency) clock->errors |= JCE_KPI_CLOCK_FREQUENCY;
}

/* Record only after the real capture owner accepted this complete row. */
static inline bool jce_engine_kpi_clock_record(JceEngineKpiClock *clock,
    uint64_t frequency, uint64_t begin, uint64_t end, uint64_t row)
{
    if (!clock || !clock->enabled || clock->errors) return false;
    if (!frequency || frequency != clock->frequency)
        clock->errors |= JCE_KPI_CLOCK_FREQUENCY;
    if (row != clock->render_intervals)
        clock->errors |= JCE_KPI_CLOCK_SEQUENCE;
    if (begin < clock->timer_start || end < begin ||
        (clock->render_intervals && begin < clock->render_end))
        clock->errors |= JCE_KPI_CLOCK_COUNTER;
    if (clock->render_intervals && begin > clock->render_end) {
        const uint64_t gap = begin - clock->render_end;
        if (gap > UINT64_MAX - clock->omitted_ticks)
            clock->errors |= JCE_KPI_CLOCK_OVERFLOW;
        else clock->omitted_ticks += gap;
        /* A skipped interval is explicit; it cannot certify continuous render. */
        clock->errors |= JCE_KPI_CLOCK_GAP;
    }
    if (end >= begin && end - begin > UINT64_MAX - clock->render_ticks)
        clock->errors |= JCE_KPI_CLOCK_OVERFLOW;
    if (clock->render_intervals == UINT64_MAX)
        clock->errors |= JCE_KPI_CLOCK_OVERFLOW;
    if (clock->errors) return false;
    if (!clock->render_intervals) clock->render_start = begin;
    clock->render_end = end;
    clock->render_ticks += end - begin;
    ++clock->render_intervals;
    return true;
}

/* The unchanged engine timer supplies the actual successful deadline check. */
static inline bool jce_engine_kpi_clock_check(JceEngineKpiClock *clock,
    uint64_t frequency, uint64_t checked)
{
    if (!clock || !clock->enabled) return false;
    if (!frequency || frequency != clock->frequency)
        clock->errors |= JCE_KPI_CLOCK_FREQUENCY;
    if (checked < clock->timer_start ||
        (clock->render_intervals && checked < clock->render_end) || clock->checked)
        clock->errors |= JCE_KPI_CLOCK_COUNTER;
    clock->timer_checked = checked;
    clock->checked = true;
    return clock->errors == 0;
}

/* Validate after the real writer's terminal finalization, before logging once. */
static inline bool jce_engine_kpi_clock_finish(JceEngineKpiClock *clock,
    uint64_t records)
{
    if (!clock || !clock->enabled) return false;
    if (!clock->checked || !clock->render_intervals ||
        clock->timer_checked < clock->render_start ||
        clock->timer_checked > clock->render_end)
        clock->errors |= JCE_KPI_CLOCK_TERMINAL;
    if (records != clock->render_intervals)
        clock->errors |= JCE_KPI_CLOCK_SEQUENCE;
    return clock->errors == 0;
}
#endif
