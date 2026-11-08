/* Private bgfx CPU callback bridge. The upstream library must independently
 * enable and call its profiler hooks; this bridge cannot establish that fact.
 * Capture switches/reset require producer quiescence, as for JceTraceScope.
 * These spans describe calling-thread CPU execution, not GPU completion. */
#ifndef JCE_RENDERER_BGFX_PROFILER_INTERNAL_H
#define JCE_RENDERER_BGFX_PROFILER_INTERNAL_H

#include "os/core/jce_trace_scope_internal.h"
#include <jce/os/core/jce_str.h>
#include <stdint.h>

/* Match compiler TLS already used by the renderer encoder and profiler.
 * Never fall back to process-shared mutable storage on an unknown compiler. */
#if JCE_COMPILER_MSVC
#define JCE_BGFX_CPU_TLS __declspec(thread)
#define JCE_BGFX_CPU_TLS_AVAILABLE 1
#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG
#define JCE_BGFX_CPU_TLS __thread
#define JCE_BGFX_CPU_TLS_AVAILABLE 1
#else
#define JCE_BGFX_CPU_TLS_AVAILABLE 0
#endif

#if JCE_BGFX_CPU_TLS_AVAILABLE
#define JCE_BGFX_CPU_STACK_CAP 64u
typedef struct JceBgfxCpuEntry {
    JceTraceScope scope;
    char name[JCE_TRACE_NAME_CAP];
} JceBgfxCpuEntry;

static JCE_BGFX_CPU_TLS struct {
    JceBgfxCpuEntry entries[JCE_BGFX_CPU_STACK_CAP];
    uint32_t used;
    uint64_t skipped;
} s_bgfx_cpu_trace;

static inline void jce_bgfx_cpu_trace_begin(const char *name)
{
    if (s_bgfx_cpu_trace.used == JCE_BGFX_CPU_STACK_CAP) {
        if (s_bgfx_cpu_trace.skipped != UINT64_MAX)
            ++s_bgfx_cpu_trace.skipped;
        /* Omitted depth is visible; a closed admitted subset is not evidence
         * that all callback work was represented. */
        if (jce_trace_enabled())
            jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                                "bgfx.cpu.stack_overflow", 1);
        return;
    }

    JceBgfxCpuEntry *entry = &s_bgfx_cpu_trace.entries[s_bgfx_cpu_trace.used++];
    entry->scope = (JceTraceScope){0};
    /* A zero scope still occupies its begin/end position while disabled or
     * locally suppressed, so its end cannot close an admitted outer span. */
    if (!jce_trace_enabled())
        return;
    jce_strlcpy(entry->name, "bgfx.cpu.", sizeof(entry->name));
    jce_strlcpy(entry->name + 9u, name && name[0] ? name : "unnamed",
                 sizeof(entry->name) - 9u);
    /* Both bgfx name variants are copied: dynamic name storage may disappear
     * immediately after begin, and the ring has the same fixed name bound. */
    entry->scope = jce_trace_scope_begin(entry->name);
}

static inline void jce_bgfx_cpu_trace_end(void)
{
    if (s_bgfx_cpu_trace.skipped) {
        --s_bgfx_cpu_trace.skipped;
        return;
    }
    if (!s_bgfx_cpu_trace.used)
        return;
    JceBgfxCpuEntry *entry = &s_bgfx_cpu_trace.entries[--s_bgfx_cpu_trace.used];
    jce_trace_scope_end(entry->scope, JCE_TRACE_TASK_SUCCEEDED);
}
#else
static inline void jce_bgfx_cpu_trace_begin(const char *name)
{
    (void)name;
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bgfx.cpu.tls_unavailable", 1);
}
static inline void jce_bgfx_cpu_trace_end(void) { }
#endif

#undef JCE_BGFX_CPU_TLS
#endif /* JCE_RENDERER_BGFX_PROFILER_INTERNAL_H */
