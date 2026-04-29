/*
 * jce_log.h  High-performance async structured logging.
 *
 * Format: {timestamp} [{thread}] {level} - {tag}: {message} at {file}:{line}
 * Matches the Java JceLogger output format.
 *
 * On platforms with threading (desktop, mobile), log messages are enqueued
 * into an MPSC ring buffer and written to stderr / file by a dedicated
 * backend IO thread.  On WASM, falls back to synchronous fprintf.
 */

#ifndef JCE_LOG_H
#define JCE_LOG_H


#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdarg.h>

JCE_EXTERN_C_BEGIN

typedef enum JceLogLevel {
    JCE_LOG_LEVEL_TRACE = 0,
    JCE_LOG_LEVEL_DEBUG,
    JCE_LOG_LEVEL_INFO,
    JCE_LOG_LEVEL_SUCCESS,
    JCE_LOG_LEVEL_WARN,
    JCE_LOG_LEVEL_ERROR,
    JCE_LOG_LEVEL_OFF
} JceLogLevel;

/* Call once at startup.  Enables ANSI escape codes on Windows console
   and spawns the backend IO thread (on threaded platforms). */
JCE_API void JCE_CALL jce_log_init(void);

/* Flush remaining messages, join the backend thread, close log file.
   Call once at engine shutdown.  Safe to call if init was never called. */
JCE_API void JCE_CALL jce_log_shutdown(void);

/* Synchronously drain the ring buffer to stderr / log file.
   Use in crash handlers before re-raising the signal. */
JCE_API void JCE_CALL jce_log_flush(void);

/* Runtime configuration. */
JCE_API void JCE_CALL jce_log_set_level(JceLogLevel level);
JCE_API void JCE_CALL jce_log_set_colors(bool enabled);

/* Enable persistent file output (plain text, no ANSI).
   Pass NULL to close the current log file. */
JCE_API void JCE_CALL jce_log_set_file(const char *path);

/* Set the display name for the calling thread (e.g. "MAIN", "RENDER").
 * Must be called per-thread; defaults to the numeric thread ID. */
JCE_API void JCE_CALL jce_log_set_thread_name(const char *name);

/* Core logging function — use the macros below instead. */
JCE_API void JCE_CALL jce_log_write(JceLogLevel level, const char *tag,
                   const char *file, int line,
                   const char *fmt, ...);

/* va_list variant for FFI bindings that cannot call variadic functions. */
JCE_API void JCE_CALL jce_log_write_v(JceLogLevel level, const char *tag,
                     const char *file, int line,
                     const char *fmt, va_list ap);

JCE_EXTERN_C_END

/* -- Convenience macros (capture __FILE__ and __LINE__) ------------ */

#ifdef JCE_DIST
/* Dist builds: all logging compiled out. */
#define LOG_TRACE(tag, ...)   ((void)0)
#define LOG_DEBUG(tag, ...)   ((void)0)
#define LOG_INFO(tag, ...)    ((void)0)
#define LOG_SUCCESS(tag, ...) ((void)0)
#define LOG_WARN(tag, ...)    ((void)0)
#define LOG_ERROR(tag, ...)   ((void)0)
#else
#define LOG_TRACE(tag, ...)   jce_log_write(JCE_LOG_LEVEL_TRACE,   tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(tag, ...)   jce_log_write(JCE_LOG_LEVEL_DEBUG,   tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(tag, ...)    jce_log_write(JCE_LOG_LEVEL_INFO,    tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_SUCCESS(tag, ...) jce_log_write(JCE_LOG_LEVEL_SUCCESS, tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(tag, ...)    jce_log_write(JCE_LOG_LEVEL_WARN,    tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   jce_log_write(JCE_LOG_LEVEL_ERROR,   tag, __FILE__, __LINE__, __VA_ARGS__)
#endif

#endif /* JCE_LOG_H */
