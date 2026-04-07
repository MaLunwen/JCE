/*
 * jce_log.h  Structured logging with ANSI colors.
 *
 * Format: {timestamp} [{thread}] {level} - {tag}: {message} at {file}:{line}
 * Matches the Java JceLogger output format.
 */

#ifndef JCE_LOG_H
#define JCE_LOG_H

#include <stdbool.h>

typedef enum JceLogLevel {
    JCE_LOG_LEVEL_TRACE = 0,
    JCE_LOG_LEVEL_DEBUG,
    JCE_LOG_LEVEL_INFO,
    JCE_LOG_LEVEL_SUCCESS,
    JCE_LOG_LEVEL_WARN,
    JCE_LOG_LEVEL_ERROR,
    JCE_LOG_LEVEL_OFF
} JceLogLevel;

/* Call once at startup (enables ANSI escape codes on Windows console). */
void jce_log_init(void);

/* Runtime configuration. */
void jce_log_set_level(JceLogLevel level);
void jce_log_set_colors(bool enabled);

/* Set the display name for the calling thread (e.g. "MAIN", "RENDER").
 * Must be called per-thread; defaults to the numeric thread ID. */
void jce_log_set_thread_name(const char *name);

/* Core logging function  use the macros below instead. */
void jce_log_write(JceLogLevel level, const char *tag,
                   const char *file, int line,
                   const char *fmt, ...);

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
