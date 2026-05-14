/*
 * api_core.h  Layer 1 — Core utilities.
 *
 * Math, logging, timing, profiling, memory, threading, filesystem,
 * events, handles, crash handling, internationalisation, system info.
 */

#ifndef JCE_API_CORE_H
#define JCE_API_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_coroutine.h>
#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_handle.h>
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_object_pool.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_CORE_H */
