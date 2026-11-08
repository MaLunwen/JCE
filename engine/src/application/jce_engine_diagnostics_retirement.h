/* The capture worker can log; retire it before switching the last logger to
 * synchronous emission. Both owners retain storage until final publication. */
#ifndef JCE_ENGINE_DIAGNOSTICS_RETIREMENT_H
#define JCE_ENGINE_DIAGNOSTICS_RETIREMENT_H
#include "jce_engine_trace_scope.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_log_internal.h"

static inline void engine_quiesce_diagnostics(JceTraceCapture *capture)
{
    jce_trace_capture_quiesce(capture);
    JCE_ENGINE_TRACE_CALL("engine.log_quiesce", jce_log_quiesce_internal());
}

#endif
