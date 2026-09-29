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

/* Fence the backend so records queued before this call reach stderr / file.
   Safe with concurrent producers.  Crash-path waits are bounded, so the call
   becomes best-effort if the backend itself is unavailable. */
JCE_API void JCE_CALL jce_log_flush(void);

/* Runtime configuration. */
JCE_API void JCE_CALL jce_log_set_level(JceLogLevel level);
JCE_API void JCE_CALL jce_log_set_colors(bool enabled);

/* -- File sink: persistence with a bounded, compressed history ------
 *
 * The live log stays plain text — no ANSI, no framing, no compression — so it
 * needs no tool to read.  NOT, on Windows, while the sink is open: SDL opens a
 * writable file with a share mode of 0 (SDL_iostream.c, CreateFileW), so the
 * live file is exclusive to this process until jce_log_set_file(NULL) or
 * jce_log_shutdown() releases it.  That is pre-existing behaviour of the sink,
 * recorded here because "plain text" otherwise reads as "tailable" and on
 * Windows it is not.  Rotated generations are closed files and always readable.
 *
 * The live log is not allowed to grow without limit: once it passes
 * `max_bytes` the backend closes it, moves it aside as generation 1, shifts
 * the older generations down and reopens a fresh live file.  Generation
 * `max_files` is deleted, so the directory holds at most `max_files` rotated
 * files plus the live one.
 *
 * Rotated generations are compressed with zstd and named "<path>.N.zst".
 * If compression fails for any reason the generation is still kept, renamed
 * to "<path>.N" with no ".zst" suffix — losing a log is worse than storing it
 * uncompressed.  Both spellings participate in the shift and in the deletion
 * of the oldest, so a mixed directory is still bounded.
 *
 * Costs, stated so nothing here is a surprise:
 *   - Rotation runs on the log backend thread while the file lock is held.
 *     Producers do not block on it: they only enqueue into the ring buffer.
 *     The exception is the synchronous fallback path (before jce_log_init(),
 *     after jce_log_shutdown()), where the calling thread IS the emitter and
 *     therefore pays for the rotation itself.  Either way log IO pauses for
 *     the duration of one compress, and a long enough pause fills the ring,
 *     which drops records — that is why the level here is 3, not 9.
 *   - Compression buffers the whole rotated file in memory, twice: once raw,
 *     once compressed.  `max_bytes` therefore also sizes that allocation, and
 *     it has NO upper clamp — do not set it larger than you will allocate.
 *   - There is no per-line stat(): the sink counts the bytes it writes.
 *
 * On Emscripten there is no file sink at all and all of this is compiled out;
 * jce_log_set_file/_ex are no-ops and jce_log_get_file_config returns false.
 */

/* Defaults applied when a field is 0 / the config pointer is NULL.  These are
   the values the implementation uses — jce_log_get_file_config() reports the
   effective config back, so the numbers below are checkable, not just claimed
   (tests/os/core/test_jce_log_rotation.c asserts them). */
#define JCE_LOG_FILE_DEFAULT_MAX_BYTES  (8u * 1024u * 1024u)  /* 8 MiB     */
#define JCE_LOG_FILE_DEFAULT_MAX_FILES  4u                    /* + live    */

/* Floor on max_bytes.  A smaller request is RAISED to this, because a
   threshold below one line would rotate on every record.  Enforced in
   jce_log_set_file_ex() and observable through jce_log_get_file_config(). */
#define JCE_LOG_FILE_MIN_MAX_BYTES      1024u

/* Ceiling on max_files.  A larger request is LOWERED to this. */
#define JCE_LOG_FILE_MAX_MAX_FILES      64u

typedef struct JceLogFileConfig {
    uint64_t max_bytes;   /* rotate once the live file reaches this size  */
    uint32_t max_files;   /* rotated generations to keep (live excluded)  */
    bool     compress;    /* zstd-compress rotated generations            */
} JceLogFileConfig;

/* Enable persistent file output (plain text, no ANSI).
   Pass NULL to close the current log file.
   Equivalent to jce_log_set_file_ex(path, NULL): rotation is ON with the
   defaults above.  An unbounded log directory is the failure this sink is
   meant to avoid, so there is deliberately no "never rotate" setting. */
JCE_API void JCE_CALL jce_log_set_file(const char *path);

/* As jce_log_set_file(), with explicit rotation settings.  `cfg` is copied;
   NULL or a zero field means "use the default".  The config applies to the
   file opened by THIS call and is replaced by the next one. */
JCE_API void JCE_CALL jce_log_set_file_ex(const char *path,
                                          const JceLogFileConfig *cfg);

/* Report the config the sink is actually using, after defaults and clamping.
   Returns true iff a log file is currently open.  *out is filled either way:
   with the pending/last config when no file is open, and zeroed on platforms
   that have no file sink.  Returns false and touches nothing if out is NULL. */
JCE_API bool JCE_CALL jce_log_get_file_config(JceLogFileConfig *out);

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
    int64_t     wall_epoch_s; /* Unix epoch seconds at producer enqueue     */
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

#ifdef JCE_LOG_NONE
/* No logging at all: for host tools that link the engine's codecs but not its
 * async log backend (jce_cook is the one such target).  This used to be spelled
 * JCE_DIST, which conflated "a lean host tool" with "a shipping game" -- so the
 * moment the dist variant kept a level, the cook tool stopped linking.  They are
 * different questions and now have different switches. */
#define LOG_TRACE(tag, ...)   ((void)0)
#define LOG_DEBUG(tag, ...)   ((void)0)
#define LOG_INFO(tag, ...)    ((void)0)
#define LOG_SUCCESS(tag, ...) ((void)0)
#define LOG_WARN(tag, ...)    ((void)0)
#define LOG_ERROR(tag, ...)   ((void)0)
#elif defined(JCE_DIST)
/* Dist builds keep WARN and ERROR; everything chattier is compiled out.
 *
 * Until 2026-08-31 this block silenced all six, and CMakeLists.txt puts
 * JCE_DIST=1 PUBLIC on jce_core for the dist variant (and JCESDKHelpers.cmake
 * puts it on every SDK consumer exe), so a shipped game emitted nothing: 1477
 * LOG_* statements in engine/src and 93 in caged_kingdom, all `((void)0)`.
 * Paired with a dist link that produced no .pdb, a player-reported crash came
 * back as an unsymbolizable minidump with no log beside it.
 *
 * TRACE/DEBUG/INFO/SUCCESS stay compiled out: they are the per-frame and
 * per-asset chatter the level was introduced to remove, and jce_log's ring is
 * 4096 slots deep, so leaving them in would cost throughput and drown the two
 * levels that matter.  WARN and ERROR are, by their own contract, the ones a
 * shipped build has to be able to say out loud. */
#define LOG_TRACE(tag, ...)   ((void)0)
#define LOG_DEBUG(tag, ...)   ((void)0)
#define LOG_INFO(tag, ...)    ((void)0)
#define LOG_SUCCESS(tag, ...) ((void)0)
#define LOG_WARN(tag, ...)    jce_log_write(JCE_LOG_LEVEL_WARN,  tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   jce_log_write(JCE_LOG_LEVEL_ERROR, tag, __FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_TRACE(tag, ...)   jce_log_write(JCE_LOG_LEVEL_TRACE,   tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(tag, ...)   jce_log_write(JCE_LOG_LEVEL_DEBUG,   tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(tag, ...)    jce_log_write(JCE_LOG_LEVEL_INFO,    tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_SUCCESS(tag, ...) jce_log_write(JCE_LOG_LEVEL_SUCCESS, tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(tag, ...)    jce_log_write(JCE_LOG_LEVEL_WARN,    tag, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(tag, ...)   jce_log_write(JCE_LOG_LEVEL_ERROR,   tag, __FILE__, __LINE__, __VA_ARGS__)
#endif

#endif /* JCE_LOG_H */
