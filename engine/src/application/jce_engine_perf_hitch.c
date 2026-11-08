/* One paired wall/comparison ring and one completed-snapshot observer. */
#include "jce_engine_perf_hitch.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_wall(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
/* Preserve the previous reporter's nearest-rank rule, not interpolation. */
static double percentile(const double *ascending, uint32_t count, double q)
{
    if (!count) return 0.0;
    long rank = (long)(q * (double)count + 0.5) - 1;
    if (rank < 0) rank = 0;
    if (rank >= (long)count) rank = (long)count - 1;
    return ascending[rank];
}
void jce_engine_perf_hitch_init(JceEnginePerfHitch *state, bool enabled, double factor)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->enabled = enabled;
    state->factor = isfinite(factor) ? factor : 4.0;
    if (state->factor < 1.5) state->factor = 1.5;
}

void jce_engine_perf_disposition_init(JceEnginePerfHitch *state, bool enabled,
    uint64_t frequency)
{
    if (!state) return;
    memset(&state->disposition, 0, sizeof state->disposition);
    state->disposition.enabled = enabled;
    if (!enabled) return;
    state->disposition.frequency = frequency;
    if (!frequency) state->disposition.errors = JCE_ENGINE_DISPOSITION_FREQUENCY;
}

static bool disposition_close(JceEnginePerfDisposition *d,
    uint64_t frequency, uint64_t boundary)
{
    if (!frequency || frequency != d->frequency)
        d->errors |= JCE_ENGINE_DISPOSITION_FREQUENCY;
    if (!d->active) return d->errors == 0;
    if (!d->released) d->errors |= JCE_ENGINE_DISPOSITION_UNFINISHED;
    if (boundary < d->current.begin) d->errors |= JCE_ENGINE_DISPOSITION_COUNTER;
    if (d->kind >= JCE_ENGINE_DISPOSITION_COUNT)
        d->errors |= JCE_ENGINE_DISPOSITION_SEQUENCE;
    if (d->errors) { d->active = false; return false; }
    JceEnginePerfDispositionBucket *bucket = &d->buckets[d->kind];
    const uint64_t ticks = boundary - d->current.begin;
    uint64_t *completed = d->kind == JCE_ENGINE_DISPOSITION_FULL ? &d->full : &d->nonfull;
    if (bucket->count == UINT64_MAX || d->intervals == UINT64_MAX ||
        *completed == UINT64_MAX || ticks > UINT64_MAX - bucket->ticks) {
        d->errors |= JCE_ENGINE_DISPOSITION_OVERFLOW;
        d->active = false;
        return false;
    }
    d->current.end = boundary;
    if (!bucket->count) bucket->first = d->current;
    bucket->last = d->current;
    ++bucket->count;
    bucket->ticks += ticks;
    ++d->intervals;
    ++*completed;
    d->end = boundary;
    d->active = false;
    return true;
}

bool jce_engine_perf_disposition_boundary(JceEnginePerfHitch *state,
    uint64_t frequency, uint64_t boundary, uint64_t row)
{
    if (!state || !state->disposition.enabled || state->disposition.finished ||
        !state->iterating) return false;
    JceEnginePerfDisposition *d = &state->disposition;
    if (d->active && (state->interval <= d->current.interval || row < d->current.row))
        d->errors |= JCE_ENGINE_DISPOSITION_SEQUENCE;
    (void)disposition_close(d, frequency, boundary);
    if (d->errors) return false;
    if (!d->intervals) d->begin = boundary;
    d->current = (JceEnginePerfDispositionBoundary){state->interval, row, boundary, boundary};
    d->kind = JCE_ENGINE_DISPOSITION_CLOSING;
    d->active = true;
    d->released = false;
    return true;
}

void jce_engine_perf_disposition_reason(JceEnginePerfHitch *state, uint32_t kind)
{
    if (!state || !state->disposition.enabled || state->disposition.finished) return;
    JceEnginePerfDisposition *d = &state->disposition;
    /* Admission rejection before the raw boundary must not overwrite the
     * previous interval. Nested and already released calls change nothing. */
    if (!state->iterating || !d->active || d->released ||
        d->current.interval != state->interval) return;
    d->kind = kind;
}

void jce_engine_perf_disposition_release(JceEnginePerfHitch *state, bool full)
{
    if (!state || !state->disposition.enabled || state->disposition.finished) return;
    JceEnginePerfDisposition *d = &state->disposition;
    if (!state->iterating || !d->active || d->current.interval != state->interval) return;
    if (d->released) { d->errors |= JCE_ENGINE_DISPOSITION_SEQUENCE; return; }
    if (full) d->kind = JCE_ENGINE_DISPOSITION_FULL;
    d->released = true;
}

bool jce_engine_perf_disposition_finish(JceEnginePerfHitch *state,
    uint64_t frequency, uint64_t boundary)
{
    if (!state || !state->disposition.enabled) return false;
    JceEnginePerfDisposition *d = &state->disposition;
    if (d->finished) return d->errors == 0;
    (void)disposition_close(d, frequency, boundary);
    if (!d->intervals) d->errors |= JCE_ENGINE_DISPOSITION_EMPTY;
    d->finished = true;
    return d->errors == 0;
}

bool jce_engine_perf_disposition_report(JceEnginePerfHitch *state)
{
    static const char *const names[JCE_ENGINE_DISPOSITION_COUNT] = {
        "full", "headless", "drawable_unknown", "drawable_minimized", "drawable_paused",
        "drawable_minimized_paused", "drawable_extent", "drawable_minimized_extent",
        "drawable_paused_extent", "drawable_minimized_paused_extent", "guard", "fallback",
        "closing", "quit", "capture_failed", "device_failed"
    };
    if (!state || !state->disposition.enabled || !state->disposition.finished ||
        state->disposition.reported) return false;
    JceEnginePerfDisposition *d = &state->disposition;
    d->reported = true;
    jce_log_write(JCE_LOG_LEVEL_INFO, "engine", __FILE__, __LINE__,
        "frame disposition v1: frequency=%llu begin=%llu end=%llu intervals=%llu "
        "full=%llu nonfull=%llu valid=%u errors=%u",
        (unsigned long long)d->frequency, (unsigned long long)d->begin,
        (unsigned long long)d->end, (unsigned long long)d->intervals,
        (unsigned long long)d->full, (unsigned long long)d->nonfull,
        (unsigned)(d->errors == 0), (unsigned)d->errors);
    for (uint32_t i = 0; i < JCE_ENGINE_DISPOSITION_COUNT; ++i) {
        const JceEnginePerfDispositionBucket *b = &d->buckets[i];
        if (!b->count) continue;
        jce_log_write(JCE_LOG_LEVEL_INFO, "engine", __FILE__, __LINE__,
            "frame disposition reason v1: reason=%s count=%llu ticks=%llu "
            "first_interval=%llu first_row=%llu first_begin=%llu first_end=%llu "
            "last_interval=%llu last_row=%llu last_begin=%llu last_end=%llu",
            names[i], (unsigned long long)b->count, (unsigned long long)b->ticks,
            (unsigned long long)b->first.interval, (unsigned long long)b->first.row,
            (unsigned long long)b->first.begin, (unsigned long long)b->first.end,
            (unsigned long long)b->last.interval, (unsigned long long)b->last.row,
            (unsigned long long)b->last.begin, (unsigned long long)b->last.end);
    }
    return true;
}

bool jce_engine_perf_hitch_begin_interval(JceEnginePerfHitch *state)
{
    if (!state || state->iterating) return false;
    state->iterating = true;
    if (state->started) ++state->interval;
    state->started = true;
    state->terminal_completed = false;
    state->interval_completed = state->previous_full;
    state->completed_pacing = state->previous_full ? state->current_pacing :
                                                    (JceEnginePerfPacing){0};
    memset(&state->current_pacing, 0, sizeof state->current_pacing);
    state->interval_phase_armed = jce_perf_phase_enabled() != 0;
    state->interval_eligible = state->previous_full && state->previous_phase_armed &&
                               state->interval_phase_armed;
    state->previous_full = false;
    state->observed = false;
    return true;
}
void jce_engine_perf_hitch_end_interval(JceEnginePerfHitch *state, bool full)
{
    if (!state || !state->iterating) return;
    state->previous_full = full && !state->terminal_completed;
    state->previous_phase_armed = state->previous_full && state->interval_phase_armed &&
                                  jce_perf_phase_enabled() != 0;
    if (!state->previous_full)
        memset(&state->current_pacing, 0, sizeof state->current_pacing);
    state->iterating = false;
}
bool jce_engine_perf_hitch_complete_terminal_interval(JceEnginePerfHitch *state)
{
    if (!state || !state->iterating || state->terminal_completed) return false;
    state->terminal_completed = true;
    state->interval_completed = true;
    state->completed_pacing = state->current_pacing;
    state->interval_eligible = state->interval_phase_armed &&
                               jce_perf_phase_enabled() != 0;
    state->observed = false;
    ++state->interval;
    if (jce_perf_phase_enabled()) jce_perf_phase_frame_tick();
    return true;
}
void jce_engine_perf_hitch_record_pacing(JceEnginePerfHitch *state,
    const JceEnginePerfPacing *pacing)
{
    if (state && state->iterating && !state->terminal_completed && pacing)
        state->current_pacing = *pacing;
}
static bool append_name(char *out, size_t capacity, size_t *used, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    if (*used >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        const bool plain = *p >= 33u && *p <= 126u && *p != '\\' && *p != '=' && *p != '|';
        const size_t bytes = plain ? 1u : 4u;
        if (bytes >= capacity - *used) return false;
        if (plain) out[(*used)++] = (char)*p;
        else {
            out[(*used)++] = '\\'; out[(*used)++] = 'x';
            out[(*used)++] = hex[*p >> 4]; out[(*used)++] = hex[*p & 15u];
        }
    }
    out[*used] = 0;
    return true;
}
static void capture_phases(JceEnginePerfHitchReport *out, bool eligible)
{
    if (!eligible) { out->phase_status = JCE_ENGINE_PERF_PHASE_INELIGIBLE; return; }
    if (!jce_perf_phase_enabled() || !jce_perf_phase_frame_has_data()) return;
    int count = jce_perf_phase_count();
    if (count < 0 || count > JCE_PERF_PHASE_MAX_SLOTS) {
        out->phase_status = JCE_ENGINE_PERF_PHASE_INVALID; return;
    }
    bool positive = false;
    for (int i = 0; i < count; ++i) {
        const char *name = NULL; double ms = 0.0;
        if (!jce_perf_phase_peek_frame(i, &name, &ms) || !name || !isfinite(ms) || ms < 0.0) {
            out->phase_status = JCE_ENGINE_PERF_PHASE_INVALID; return;
        }
        if (ms > 0.0) positive = true;
    }
    out->phase_status = positive ? JCE_ENGINE_PERF_PHASE_POSITIVE : JCE_ENGINE_PERF_PHASE_ZERO;
    if (!positive) return;
    int selected[8]; size_t used = 0;
    for (unsigned rank = 0; rank < 8u; ++rank) {
        int best = -1; double best_ms = 0.0;
        for (int i = 0; i < count; ++i) {
            bool already = false;
            for (unsigned k = 0; k < rank; ++k) if (selected[k] == i) already = true;
            if (already) continue;
            double ms = 0.0;
            jce_perf_phase_peek_frame(i, NULL, &ms);
            if (ms > best_ms) { best_ms = ms; best = i; }
        }
        if (best < 0) break;
        selected[rank] = best;
        const char *name = NULL;
        jce_perf_phase_peek_frame(best, &name, NULL);
        const size_t entry_start = used;
        if (used) {
            /* Keep room for both the separator and the terminator. */
            if (used >= sizeof out->phases - 1u) { out->truncated = true; break; }
            out->phases[used++] = ' ';
        }
        bool named = append_name(out->phases, sizeof out->phases, &used, name);
        int length = named ? snprintf(out->phases + used, sizeof out->phases - used,
                                     "=%.2f", best_ms) : -1;
        if (length < 0 || (size_t)length >= sizeof out->phases - used) {
            used = entry_start; out->phases[used] = 0; out->truncated = true; break;
        }
        used += (size_t)length;
        ++out->top_count;
    }
}
static void capture_window(JceEnginePerfHitch *state, JceEnginePerfWindow *out)
{
    if (state->window_count < 120u) return;
    out->ready = true; out->count = state->window_count; out->ring_count = state->count;
    out->average_ms = state->window_sum_ms / (double)state->window_count;
    out->worst_ms = state->window_worst_ms;
    if (state->count) {
        /* Temporary sorting workspace, not a second permanent history ring. */
        double sorted[JCE_ENGINE_PERF_RING];
        for (uint32_t i = 0; i < state->count; ++i)
            sorted[i] = state->ring[i].wall_ms;
        qsort(sorted, state->count, sizeof(double), compare_wall);
        out->p50_ms = percentile(sorted, state->count, 0.50);
        out->p95_ms = percentile(sorted, state->count, 0.95);
        out->p99_ms = percentile(sorted, state->count, 0.99);
        out->p999_ms = percentile(sorted, state->count, 0.999);
    }
    state->window_count = 0; state->window_sum_ms = state->window_worst_ms = 0.0;
}
bool jce_engine_perf_hitch_observe_completed(JceEnginePerfHitch *state, double wall_ms,
    bool wall_clock_valid,
    JceEnginePerfHitchReport *hitch, JceEnginePerfWindow *window)
{
    if (!state || !hitch || !window || !state->enabled || !state->iterating || state->observed ||
        !isfinite(wall_ms) || wall_ms < 0.0 || !isfinite(state->factor) || state->factor < 1.5 ||
        !isfinite(state->window_sum_ms + wall_ms)) return false;
    memset(hitch, 0, sizeof(*hitch));
    memset(window, 0, sizeof(*window));
    JceEnginePerfPacing pacing = state->completed_pacing;
    const bool pacing_valid = state->interval_completed && wall_clock_valid &&
        pacing.clock_valid && pacing.fps >= 0 && isfinite(pacing.planned_ms) &&
        isfinite(pacing.actual_ms) && pacing.planned_ms >= 0.0 &&
        (pacing.planned_ms == 0.0 || pacing.fps > 0) &&
        pacing.actual_ms >= pacing.planned_ms && pacing.actual_ms <= wall_ms;
    if (!pacing_valid) {
        pacing.planned_ms = pacing.actual_ms = 0.0;
        pacing.clock_valid = false;
        if (pacing.fps < 0) pacing.fps = 0;
    }
    const double comparison_ms = wall_ms - pacing.planned_ms;
    state->observed = true;
    state->window_sum_ms += wall_ms;
    if (wall_ms > state->window_worst_ms) state->window_worst_ms = wall_ms;
    ++state->window_count;
    if (state->warm < 120u) ++state->warm;
    else {
        state->ring[state->head] = (JceEnginePerfSample){wall_ms, comparison_ms};
        state->head = (state->head + 1u) & (JCE_ENGINE_PERF_RING - 1u);
        if (state->count < JCE_ENGINE_PERF_RING) ++state->count;
        if (state->count >= 32u && state->logged < 64u && comparison_ms >= 8.0) {
            double sum = 0.0;
            for (uint32_t i = 0; i < state->count; ++i) sum += state->ring[i].comparison_ms;
            const double mean = sum / (double)state->count;
            if (isfinite(mean) && comparison_ms >= mean * state->factor) {
                hitch->ready = true; hitch->interval = state->interval;
                hitch->wall_ms = wall_ms; hitch->comparison_ms = comparison_ms;
                hitch->mean_ms = mean; hitch->pacing = pacing;
                capture_phases(hitch, state->interval_eligible);
                ++state->logged;
            }
        }
    }
    capture_window(state, window);
    return true;
}
bool jce_engine_perf_hitch_format(const JceEnginePerfHitchReport *report,
    char *out, uint32_t capacity)
{
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!report || !report->ready || !isfinite(report->wall_ms) || report->wall_ms <= 0.0 ||
        !isfinite(report->comparison_ms) || report->comparison_ms < 0.0 ||
        !isfinite(report->mean_ms) || report->mean_ms < 0.0 ||
        !isfinite(report->pacing.planned_ms) || report->pacing.planned_ms < 0.0 ||
        !isfinite(report->pacing.actual_ms) || report->pacing.actual_ms < 0.0 ||
        report->pacing.fps < 0) return false;
    const char *phases = report->phases;
    switch (report->phase_status) {
    case JCE_ENGINE_PERF_PHASE_UNAVAILABLE: phases = "(NO PHASE SNAPSHOT)"; break;
    case JCE_ENGINE_PERF_PHASE_INELIGIBLE: phases = "(discontinuous interval; snapshot not attributed)"; break;
    case JCE_ENGINE_PERF_PHASE_ZERO: phases = "(sampled snapshot; no positive duration)"; break;
    case JCE_ENGINE_PERF_PHASE_INVALID: phases = "(invalid phase samples; attribution unavailable)"; break;
    case JCE_ENGINE_PERF_PHASE_POSITIVE:
        if (!memchr(report->phases, 0, sizeof report->phases)) return false;
        break;
    default: return false;
    }
    int length = snprintf(out, capacity,
        "perf-hitch: raw wall %.2f ms | comparison %.2f ms (mean %.2f ms) | "
        "pacing planned %.2f ms actual %.2f ms fps %d clock %s | interval %llu | top phases: %s%s",
        report->wall_ms, report->comparison_ms, report->mean_ms,
        report->pacing.planned_ms, report->pacing.actual_ms, report->pacing.fps,
        report->pacing.clock_valid ? "valid" : "unavailable",
        (unsigned long long)report->interval, phases,
        report->truncated ? " (phase names truncated)" : "");
    if (length < 0 || (uint32_t)length >= capacity) { out[0] = 0; return false; }
    return true;
}
