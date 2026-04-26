/*
 * jce_profiler.c  Tracy bridge hidden behind the JCE public C ABI.
 */

#include <jce/os/core/jce_profiler.h>

#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)

#include <string.h>
#include <tracy/TracyC.h>

#ifndef TRACY_CALLSTACK
#define TRACY_CALLSTACK 0
#endif

static size_t jce_profile_strlen(const char *s)
{
    return s ? strlen(s) : 0u;
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
    return out;
}

void jce_profile_zone_end(JceProfileZone zone)
{
    TracyCZoneCtx ctx;
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
