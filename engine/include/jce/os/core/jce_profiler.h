/*
 * jce_profiler.h  Header-clean profiling API.
 *
 * Public code never includes Tracy headers directly.  When Tracy is
 * enabled, these macros route through JCE C ABI functions implemented
 * inside the engine; otherwise they compile to no-ops.
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

#include <jce/os/core/jce_defs.h>

#if defined(JCE_TRACY_ENABLED) && (JCE_TRACY_ENABLED + 0 == 1)

JCE_EXTERN_C_BEGIN

typedef struct JceProfileZone {
    uint32_t id;
    int32_t  active;
} JceProfileZone;

JCE_API JceProfileZone JCE_CALL
jce_profile_zone_begin(const char *name, uint32_t color,
                       const char *file, uint32_t line,
                       const char *function);
JCE_API void JCE_CALL jce_profile_zone_end(JceProfileZone zone);
JCE_API void JCE_CALL jce_profile_frame_mark(const char *name);
JCE_API void JCE_CALL jce_profile_alloc(const void *ptr, size_t size);
JCE_API void JCE_CALL jce_profile_free(const void *ptr);
JCE_API void JCE_CALL jce_profile_plot(const char *name, double val);
JCE_API void JCE_CALL jce_profile_plot_i(const char *name, int64_t val);
JCE_API void JCE_CALL jce_profile_msg(const char *text, size_t len);
JCE_API void JCE_CALL jce_profile_thread_name(const char *name);

JCE_EXTERN_C_END

/* -- Zone scoped --------------------------------------------------- */

/* Anonymous zone — uses source location of the macro. */
#define JCE_PROFILE_ZONE                                                \
    JceProfileZone __jce_ctx = jce_profile_zone_begin(                  \
        NULL, 0u, __FILE__, (uint32_t)__LINE__, __func__)
#define JCE_PROFILE_ZONE_END      jce_profile_zone_end(__jce_ctx)

/* Named zone. */
#define JCE_PROFILE_ZONE_N(name)                                        \
    JceProfileZone __jce_ctx = jce_profile_zone_begin(                  \
        (name), 0u, __FILE__, (uint32_t)__LINE__, __func__)

/* Coloured zone (color is 0xRRGGBB). */
#define JCE_PROFILE_ZONE_C(color)                                       \
    JceProfileZone __jce_ctx = jce_profile_zone_begin(                  \
        NULL, (uint32_t)(color), __FILE__, (uint32_t)__LINE__, __func__)

/* -- Frame marks --------------------------------------------------- */

#define JCE_PROFILE_FRAME_MARK          jce_profile_frame_mark(NULL)
#define JCE_PROFILE_FRAME_MARK_N(name)  jce_profile_frame_mark(name)

/* -- Memory tracking ----------------------------------------------- */

#define JCE_PROFILE_ALLOC(ptr, size) jce_profile_alloc((ptr), (size))
#define JCE_PROFILE_FREE(ptr)        jce_profile_free(ptr)

/* -- Plots --------------------------------------------------------- */

#define JCE_PROFILE_PLOT(name, val)   jce_profile_plot((name), (double)(val))
#define JCE_PROFILE_PLOT_I(name, val) jce_profile_plot_i((name), (int64_t)(val))

/* -- Messages ------------------------------------------------------ */

#define JCE_PROFILE_MSG(text, len)   jce_profile_msg((text), (len))

/* -- Thread naming ------------------------------------------------- */

#define JCE_PROFILE_THREAD_NAME(name) jce_profile_thread_name(name)

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
