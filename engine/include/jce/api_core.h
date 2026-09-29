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

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_assert.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_read_source.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_handle.h>
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_toolchain.h>
#include <jce/os/core/jce_trace.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/os/core/jce_camera_shake.h>
#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_console_session.h>
#include <jce/os/core/jce_coro.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_hashmap.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_platform_services.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_str.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_CORE_H */
