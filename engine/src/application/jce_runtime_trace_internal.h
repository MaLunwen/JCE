/* Runtime CPU call-wall observations over the existing bounded trace ring. */
#ifndef JCE_RUNTIME_TRACE_INTERNAL_H
#define JCE_RUNTIME_TRACE_INTERNAL_H
#include <jce/application/jce_runtime.h>
#include <stddef.h>
#include "os/core/jce_trace_scope_internal.h"

static inline JceTraceScope rt_trace_begin(const char *name)
{
    return jce_trace_scope_begin(name);
}

/* Existing constructor outcomes only; optional subsystem readiness is separate. */
static inline JceRuntime *rt_trace_create_failed(JceTraceScope scope, int reason)
{
    if (scope.id)
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,
            "runtime.create.failure_reason", reason, scope.id, scope.parent);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
    return NULL;
}
#endif
