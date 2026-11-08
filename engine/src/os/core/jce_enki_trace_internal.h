/* Shared enki scheduler worker-exit bridge to the one trace authority. */
#ifndef JCE_ENKI_TRACE_INTERNAL_H
#define JCE_ENKI_TRACE_INTERNAL_H
#include <jce/os/core/jce_trace.h>
#include <SDL3/SDL_thread.h>

/* Called on the actual worker before scheduler shutdown joins it. Lazy
 * task registration supports trace enabled after scheduler creation. */
static inline void jce_enki_trace_worker_stop(uint32_t threadnum)
{
    if (threadnum > 0) {
        jce_trace_thread_unregister();
        /* enki creates non-SDL threads. Release SDL-owned per-thread storage
         * after the final trace event, even when trace was never enabled. */
        SDL_CleanupTLS();
    }
}
#endif
