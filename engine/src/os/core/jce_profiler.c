/*
 * jce_profiler.c  Tracy bridge hidden behind the JCE public C ABI.
 */

#include <jce/os/core/jce_profiler.h>

#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)

#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>

#include <string.h>
#include <tracy/TracyC.h>

#ifndef TRACY_CALLSTACK
#define TRACY_CALLSTACK 0
#endif

static size_t jce_profile_strlen(const char *s)
{
    return s ? strlen(s) : 0u;
}

/* ── Trace bridge ───────────────────────────────────────────────────
 *
 * The engine carries 89 named JCE_PROFILE_ZONE_N zones and they went to
 * Tracy alone, which needs a live profiler connection. The built-in
 * recorder — the one that works headless and in CI, and that
 * JCE_TRACE_EXPORT writes out as Chrome/Perfetto JSON — had six
 * hand-placed call sites. So an exported trace of a frame that took
 * 639 ms contained no span longer than 1.9 ms, and the profiler could
 * not see the thing it was opened to explain.
 *
 * Standard engines route one instrumentation macro to every consumer
 * (Unreal's TRACE_CPUPROFILER_EVENT_SCOPE -> Insights and external
 * profilers; Unity's ProfilerMarker likewise). Do the same: every zone
 * that reaches Tracy also reaches the trace ring.
 *
 * Zones are strictly scoped, so their spans nest — a thread-local stack
 * is the whole bookkeeping needed, and it keeps JceProfileZone at its
 * current 8 bytes. Widening that struct would have been an ABI break in
 * a public header for a development feature.
 *
 * Cost when JCE_TRACE is unset: one predictable branch per zone edge. */
#if defined(_MSC_VER)
#  define JCE_PROF_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define JCE_PROF_TLS __thread
#else
#  define JCE_PROF_TLS
#endif

#define JCE_PROF_SPAN_DEPTH 64

typedef struct {
    uint64_t    span_id;
    uint64_t    t0_ns;
    const char *name;
} JceProfSpan;

static JCE_PROF_TLS JceProfSpan s_prof_spans[JCE_PROF_SPAN_DEPTH];
static JCE_PROF_TLS int         s_prof_depth;

static uint64_t jce_prof_now_ns(void)
{
    /* jce_time_perf_to_ms is the engine's own conversion; go through it so
     * the trace shares the renderer/engine timebase rather than inventing a
     * second one that would not line up in the exported JSON. */
    return (uint64_t)(jce_time_perf_to_ms(0u, jce_time_perf_counter())
                      * 1000000.0);
}

JceProfileZone jce_profile_zone_begin(const char *name, uint32_t color,
                                      const char *file, uint32_t line,
                                      const char *function)
{
    JceProfileZone out = { 0u, 0 };
    uint64_t srcloc;
    TracyCZoneCtx ctx;

    srcloc = ___tracy_alloc_srcloc_name(
        line,
        file ? file : "",
        jce_profile_strlen(file),
        function ? function : "",
        jce_profile_strlen(function),
        name ? name : "",
        jce_profile_strlen(name),
        color);

    ctx = ___tracy_emit_zone_begin_alloc_callstack(srcloc, TRACY_CALLSTACK, 1);
    out.id = ctx.id;
    out.active = ctx.active;

    if (jce_trace_enabled()) {
        /* Anonymous zones carry no name; fall back to the function so the
         * exported span is still identifiable. Overflow past the stack depth
         * stops recording rather than corrupting the pairing. */
        const char *label = (name && name[0]) ? name
                          : ((function && function[0]) ? function : "zone");
        if (s_prof_depth < JCE_PROF_SPAN_DEPTH) {
            JceProfSpan *sp = &s_prof_spans[s_prof_depth];
            sp->span_id = jce_trace_next_id();
            sp->t0_ns   = jce_prof_now_ns();
            sp->name    = label;
            jce_trace_task_begin(sp->span_id, sp->span_id, label, 0u);
        }
        ++s_prof_depth;
    }
    return out;
}

void jce_profile_zone_end(JceProfileZone zone)
{
    TracyCZoneCtx ctx;

    if (jce_trace_enabled() && s_prof_depth > 0) {
        --s_prof_depth;
        if (s_prof_depth < JCE_PROF_SPAN_DEPTH) {
            const JceProfSpan *sp = &s_prof_spans[s_prof_depth];
            const uint64_t now = jce_prof_now_ns();
            jce_trace_task_end(sp->span_id, sp->span_id, sp->name,
                               JCE_TRACE_TASK_SUCCEEDED,
                               (now > sp->t0_ns) ? (now - sp->t0_ns) : 0u);
        }
    }

    ctx.id = zone.id;
    ctx.active = zone.active;
    ___tracy_emit_zone_end(ctx);
}

void jce_profile_frame_mark(const char *name)
{
    ___tracy_emit_frame_mark(name);
}

void jce_profile_alloc(const void *ptr, size_t size)
{
    ___tracy_emit_memory_alloc_callstack(ptr, size, TRACY_CALLSTACK, 0);
}

void jce_profile_free(const void *ptr)
{
    ___tracy_emit_memory_free_callstack(ptr, TRACY_CALLSTACK, 0);
}

void jce_profile_plot(const char *name, double val)
{
    ___tracy_emit_plot(name, val);
}

void jce_profile_plot_i(const char *name, int64_t val)
{
    ___tracy_emit_plot_int(name, val);
}

void jce_profile_msg(const char *text, size_t len)
{
    TracyCMessage(text, len);
}

void jce_profile_thread_name(const char *name)
{
    TracyCSetThreadName(name);
}

#endif /* JCE_TRACY_ENABLED */

#if !defined(JCE_TRACY_ENABLED) || (JCE_TRACY_ENABLED + 0 != 1)
typedef int jce_profiler_noop_translation_unit;
#endif
