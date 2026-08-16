/*
 * jce_perf_phase.c  Per-frame CPU-phase accumulator implementation.
 */

#include <jce/os/core/jce_perf_phase.h>

#include <SDL3/SDL.h>

/* JCE_PERF_PHASE_MAX_SLOTS lives in the header now -- consumers size their own
 * row arrays from it, and the editor's profiler panel had drifted to a
 * hardcoded 64 against this table's 128. */

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

/* Idempotent within a frame.
 *
 * Two callers now drive this: the engine's player loop (every frame, every
 * app) and the editor's update. A second tick with no phase samples between it
 * and the first computes an all-zero delta and CLOBBERS the snapshot with
 * zeros -- which reads as "this frame cost nothing", not as "you ticked
 * twice". Guarding on the window total makes the second call a no-op and lets
 * both callers stay, which matters because the editor's call has to keep
 * working for a host that drives the engine some other way. */
static double s_last_tick_total = -1.0;

void jce_perf_phase_frame_tick(void)
{
    double total = 0.0;
    for (int i = 0; i < s_slot_count; ++i) total += s_slots[i].accum_ms;
    if (s_slot_count > 0 && total == s_last_tick_total) return;
    s_last_tick_total = total;

    for (int i = 0; i < s_slot_count; ++i) {
        double d = s_slots[i].accum_ms - s_prev_accum[i];
        s_frame_ms[i]   = (d >= 0.0) ? d : s_slots[i].accum_ms; /* report reset */
        s_prev_accum[i] = s_slots[i].accum_ms;
    }
}

int jce_perf_phase_frame_has_data(void)
{
    for (int i = 0; i < s_slot_count; ++i)
        if (s_frame_ms[i] > 0.0) return 1;
    return 0;
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
        if (s_slots[i].name && SDL_strcmp(s_slots[i].name, name) == 0) {
            s_slots[i].accum_ms += ms;
            return;
        }
    }
    /* Register new slot.  A FULL table used to drop late registrants in
     * silence, and a dropped probe reads exactly like a phase that costs
     * nothing - during the flush-phase investigation that made a 2.7 ms
     * qsort look like 0 ms and nearly sent the fix in the wrong direction.
     * Say so, once, instead of lying by omission. */
    if (s_slot_count < JCE_PERF_PHASE_MAX_SLOTS) {
        s_slots[s_slot_count].name     = name;
        s_slots[s_slot_count].accum_ms = ms;
        s_slot_count++;
    } else {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            SDL_Log("jce_perf_phase: table full (%d slots) - '%s' and any "
                    "later phase are NOT being measured. Raise "
                    "JCE_PERF_PHASE_MAX_SLOTS.",
                    JCE_PERF_PHASE_MAX_SLOTS, name);
        }
    }
}

static void perf_phase_report_scaled(char *out, int out_sz, double scale)
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
        /* A slot that rounds to 0.00 tells the reader nothing and costs ~14
         * characters of a budget that is genuinely tight: the caller's buffer
         * is char[512] (jce_engine.c:1983) and the log record it lands in is
         * also char[512] (jce_log_ring.h:42), so widening either is pointless
         * -- the only way to fit more USEFUL phases is to stop spending room
         * on empty ones.  The editor registers a slot per panel whether or not
         * that panel is open, and those idle zeros were crowding real
         * measurements off the tail of the sorted list. */
        const double v = sorted[i].accum_ms * scale;
        if (v < 0.005) continue;
        int w = SDL_snprintf(out + pos, (size_t)(out_sz - pos),
                             "%s=%.2f ", sorted[i].name, v);
        /* SDL_snprintf follows C99: on truncation it returns what it WOULD
         * have written, so w exceeds the space left.  The loop guard is tested
         * at the top, so an unclamped `pos += w` walked past out_sz and the
         * trim below then read out[pos - 1] and WROTE out[pos] outside the
         * buffer -- a stack smash in the caller's char[512], once per
         * reporting window, in every profiling run this engine has ever done.
         * Truncation is the normal case here (128 slots do not fit in 512
         * chars), not a rare edge. */
        if (w < 0) break;
        if (w >= out_sz - pos) {         /* truncated: the buffer is now full */
            pos = out_sz - 1;
            break;
        }
        pos += w;
    }
    if (pos > out_sz - 1) pos = out_sz - 1;
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

void jce_perf_phase_report(char *out, int out_sz)
{
    perf_phase_report_scaled(out, out_sz, 1.0);
}

void jce_perf_phase_report_average(char *out, int out_sz,
                                   uint32_t frame_count)
{
    double scale = frame_count > 0 ? 1.0 / (double)frame_count : 1.0;

    perf_phase_report_scaled(out, out_sz, scale);
}
