/*
 * jce_profiler.h  Zero-cost profiling macros backed by Tracy.
 *
 * When TRACY_ENABLE is defined at build time, macros expand to real
 * Tracy C instrumentation.  Otherwise they compile to nothing.
 *
 * Usage:
 *   #include "jce_profiler.h"
 *
 *   void my_function(void) {
 *       JCE_PROFILE_ZONE;            // anonymous zone
 *       ...
 *   }
 *
 *   void my_named(void) {
 *       JCE_PROFILE_ZONE_N("load"); // named zone
 *       ...
 *   }
 *
 * Frame marks go at the end of the main loop:
 *   JCE_PROFILE_FRAME_MARK;
 *
 * Memory tracking (already hooked in jce_memory.h):
 *   JCE_PROFILE_ALLOC(ptr, size);
 *   JCE_PROFILE_FREE(ptr);
 */

#ifndef JCE_PROFILER_H
#define JCE_PROFILER_H

#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)
#include <tracy/TracyC.h>

/* -- Zone scoped --------------------------------------------------- */

/* Anonymous zone — uses source location of the macro. */
#define JCE_PROFILE_ZONE          TracyCZone(__jce_ctx, 1)
#define JCE_PROFILE_ZONE_END      TracyCZoneEnd(__jce_ctx)

/* Named zone. */
#define JCE_PROFILE_ZONE_N(name)  TracyCZoneN(__jce_ctx, name, 1)

/* Coloured zone (color is 0xRRGGBB). */
#define JCE_PROFILE_ZONE_C(color) TracyCZoneC(__jce_ctx, color, 1)

/* -- Frame marks --------------------------------------------------- */

#define JCE_PROFILE_FRAME_MARK          TracyCFrameMark
#define JCE_PROFILE_FRAME_MARK_N(name)  TracyCFrameMarkNamed(name)

/* -- Memory tracking ----------------------------------------------- */

#define JCE_PROFILE_ALLOC(ptr, size) TracyCAlloc(ptr, size)
#define JCE_PROFILE_FREE(ptr)        TracyCFree(ptr)

/* -- Plots --------------------------------------------------------- */

#define JCE_PROFILE_PLOT(name, val)  TracyCPlot(name, val)
#define JCE_PROFILE_PLOT_I(name, val) TracyCPlotI(name, val)

/* -- Messages ------------------------------------------------------ */

#define JCE_PROFILE_MSG(text, len)   TracyCMessage(text, len)

/* -- Thread naming ------------------------------------------------- */

#define JCE_PROFILE_THREAD_NAME(name) TracyCSetThreadName(name)

#else

/* Tracy disabled: compile everything to zero-cost no-ops. */
#define JCE_PROFILE_ZONE                do { } while (0)
#define JCE_PROFILE_ZONE_END            do { } while (0)
#define JCE_PROFILE_ZONE_N(name)        do { (void)(name); } while (0)
#define JCE_PROFILE_ZONE_C(color)       do { (void)(color); } while (0)

#define JCE_PROFILE_FRAME_MARK          do { } while (0)
#define JCE_PROFILE_FRAME_MARK_N(name)  do { (void)(name); } while (0)

#define JCE_PROFILE_ALLOC(ptr, size)    do { (void)(ptr); (void)(size); } while (0)
#define JCE_PROFILE_FREE(ptr)           do { (void)(ptr); } while (0)

#define JCE_PROFILE_PLOT(name, val)     do { (void)(name); (void)(val); } while (0)
#define JCE_PROFILE_PLOT_I(name, val)   do { (void)(name); (void)(val); } while (0)

#define JCE_PROFILE_MSG(text, len)      do { (void)(text); (void)(len); } while (0)

#define JCE_PROFILE_THREAD_NAME(name)   do { (void)(name); } while (0)

#endif

#endif /* JCE_PROFILER_H */
