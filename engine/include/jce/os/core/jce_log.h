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

/* -- Sink: a second consumer of the emitted stream ------------------
 *
 * One optional observer of everything the logger emits, so a tool that needs
 * the same stream in a second place (the editor Console panel) does not have
 * to keep a private log store with its own severity taxonomy.  It is strictly
 * an OBSERVER: it runs AFTER the record has gone to stderr / the log file and
 * cannot suppress or rewrite that output.  Records the ring buffer dropped
 * (producer burst) never reach the sink, the same way they never reach stderr.
 *
 * Threading: on threaded platforms the sink runs on the log backend thread,
 * never on the thread that called LOG_*; before jce_log_init() and after
 * jce_log_shutdown() it runs on the calling thread.  It must therefore be
 * thread-safe, must not block for long (it stalls log IO), must not call any
 * jce_log_* function (jce_log_write would recurse forever, jce_log_set_sink
 * would deadlock), and must copy anything it keeps — every pointer in the
 * record is owned by the logger and is valid only for the duration of the
 * call. */
typedef struct JceLogRecord {
    JceLogLevel level;
    const char *tag;          /* never NULL (may be "")                     */
    const char *message;      /* never NULL; already formatted              */
    const char *file;         /* source basename, never NULL                */
    int         line;
    const char *thread_name;  /* display name of the ORIGINATING thread     */
    uint64_t    timestamp_ms; /* monotonic ms (jce_time_ticks_ms) at LOG_*  */
    int64_t     wall_epoch_s; /* Unix epoch seconds, resolved at emit time  */
} JceLogRecord;

typedef void (*JceLogSinkFn)(const JceLogRecord *rec, void *user);

/* Install the single sink, or remove it by passing fn == NULL.  Single-owner:
   a second install replaces the first.  Removal waits for a sink call already
   in flight, so the sink's state may be torn down right after it returns —
   which also means the sink must never be removed from under a lock the sink
   itself takes.  Survives jce_log_shutdown(), like the level/colour knobs. */
JCE_API void JCE_CALL jce_log_set_sink(JceLogSinkFn fn, void *user);

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
