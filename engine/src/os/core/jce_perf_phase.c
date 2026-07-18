/*
 * jce_perf_phase.c  Per-frame CPU-phase accumulator implementation.
 */

#include <jce/os/core/jce_perf_phase.h>

#include <stdio.h>
#include <string.h>

/* 24 filled up once the editor-UI phases (ed_layout/ed_panels/ed_menu/
 * ed_hier) joined the sr_* set — late registrants were silently dropped,
 * which reads as "phase missing" in the log.  48 gives headroom; slots are
 * tiny (name ptr + double). */
#define JCE_PERF_PHASE_MAX_SLOTS 64

typedef struct {
    const char *name;     /* string literal pointer — never freed */
    double      accum_ms;
} PhaseSlot;

static PhaseSlot s_slots[JCE_PERF_PHASE_MAX_SLOTS];
static int       s_slot_count = 0;
static int       s_enabled    = 0;
/* Per-frame snapshot: delta of each slot's window accumulator between the
 * last two frame_tick() calls.  Lets the editor profiler read true per-frame
 * phase costs without resetting the report window (single reset owner). */
static double    s_frame_ms[JCE_PERF_PHASE_MAX_SLOTS];
static double    s_prev_accum[JCE_PERF_PHASE_MAX_SLOTS];

void jce_perf_phase_set_enabled(int on)
{
    s_enabled = on;
}

int jce_perf_phase_enabled(void)
{
    return s_enabled;
}

void jce_perf_phase_frame_tick(void)
{
    for (int i = 0; i < s_slot_count; ++i) {
        double d = s_slots[i].accum_ms - s_prev_accum[i];
        s_frame_ms[i]   = (d >= 0.0) ? d : s_slots[i].accum_ms; /* report reset */
        s_prev_accum[i] = s_slots[i].accum_ms;
    }
}

int jce_perf_phase_count(void)
{
    return s_slot_count;
}

int jce_perf_phase_peek_frame(int idx, const char **out_name, double *out_ms)
{
    if (idx < 0 || idx >= s_slot_count) return 0;
    if (out_name) *out_name = s_slots[idx].name;
    if (out_ms)   *out_ms   = s_frame_ms[idx];
    return 1;
}

void jce_perf_phase_add(const char *name, double ms)
{
    if (!s_enabled || !name) return;
    for (int i = 0; i < s_slot_count; ++i) {
        /* Fast path: pointer equality (string literal reuse). */
        if (s_slots[i].name == name) {
            s_slots[i].accum_ms += ms;
            return;
        }
    }
    /* Slow path: strcmp for safety if two TUs use their own literal. */
    for (int i = 0; i < s_slot_count; ++i) {
        if (s_slots[i].name && strcmp(s_slots[i].name, name) == 0) {
            s_slots[i].accum_ms += ms;
            return;
        }
    }
    /* Register new slot. */
    if (s_slot_count < JCE_PERF_PHASE_MAX_SLOTS) {
        s_slots[s_slot_count].name     = name;
        s_slots[s_slot_count].accum_ms = ms;
        s_slot_count++;
    }
}

void jce_perf_phase_report(char *out, int out_sz)
{
    if (!out || out_sz <= 0) return;
    out[0] = '\0';

    int n = s_slot_count;
    if (n == 0) return;

    /* Insertion sort descending by accum_ms (n <= 24, no need for qsort). */
    PhaseSlot sorted[JCE_PERF_PHASE_MAX_SLOTS];
    for (int i = 0; i < n; ++i) sorted[i] = s_slots[i];
    for (int i = 1; i < n; ++i) {
        PhaseSlot key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].accum_ms < key.accum_ms) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }

    int pos = 0;
    for (int i = 0; i < n && pos < out_sz - 1; ++i) {
        if (!sorted[i].name) continue;
        int w = snprintf(out + pos, (size_t)(out_sz - pos),
                         "%s=%.2f ", sorted[i].name, sorted[i].accum_ms);
        if (w > 0) pos += w;
    }
    /* Trim trailing space. */
    if (pos > 0 && out[pos - 1] == ' ') {
        out[pos - 1] = '\0';
    } else {
        out[pos] = '\0';
    }

    /* Reset accumulators; keep name registrations for subsequent windows.
     * Keep the frame-tick baseline in sync so the next per-frame delta does
     * not go negative. */
    for (int i = 0; i < s_slot_count; ++i) {
        s_slots[i].accum_ms = 0.0;
        s_prev_accum[i]     = 0.0;
    }
}
