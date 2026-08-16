/*
 * jce_log.c  High-performance async logging with MPSC ring buffer.
 *
 * Architecture:
 *   Producer threads (Main, Workers) enqueue pre-formatted messages into
 *   a lock-free-ish ring buffer.  A dedicated backend IO thread drains
 *   the ring in batches and writes to stderr (with optional ANSI colors)
 *   and an optional log file.
 *
 * Hot path (jce_log_write):
 *   level check → vsnprintf → ring push → signal condvar → return.
 *   NO fprintf, NO file IO in the calling thread.
 *
 * On Emscripten (WASM) where threading is unavailable, falls back to
 * synchronous fprintf (same as the old implementation).
 *
 * Output format (matching Java JceLogger):
 *   2026-03-09 14:23:45.123 [MAIN] INFO - jce_renderer: initialized at jce_renderer.c:98
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SDL_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>   /* isatty: "should stderr be coloured?" -- see jce_log_init */
#endif

#ifdef __ANDROID__
#include <android/log.h>

static int to_android_prio(JceLogLevel level)
{
    switch (level) {
    case JCE_LOG_LEVEL_TRACE:   return ANDROID_LOG_VERBOSE;
    case JCE_LOG_LEVEL_DEBUG:   return ANDROID_LOG_DEBUG;
    case JCE_LOG_LEVEL_INFO:
    case JCE_LOG_LEVEL_SUCCESS: return ANDROID_LOG_INFO;
    case JCE_LOG_LEVEL_WARN:    return ANDROID_LOG_WARN;
    case JCE_LOG_LEVEL_ERROR:   return ANDROID_LOG_ERROR;
    default:                    return ANDROID_LOG_DEFAULT;
    }
}
#endif

/* Enable async ring buffer on platforms with threading support. */
#ifndef __EMSCRIPTEN__
#define JCE_LOG_ASYNC 1
#endif

#include "jce_log_ring.h"

#ifdef JCE_LOG_ASYNC
#include <zstd.h>   /* rotated log files are compressed; see "File sink" below */
#endif

/* -- ANSI color codes (matching JceLogger.java) -------------------- */

#define ANSI_RESET   "\033[0m"
#define ANSI_GRAY    "\033[90m"
#define ANSI_RED     "\033[91m"
#define ANSI_GREEN   "\033[92m"
#define ANSI_YELLOW  "\033[93m"
#define ANSI_BLUE    "\033[94m"
#define ANSI_PURPLE  "\033[95m"
#define ANSI_CYAN    "\033[96m"
#define ANSI_WHITE   "\033[97m"

/* -- Global state -------------------------------------------------- */

static JceLogLevel g_min_level = JCE_LOG_LEVEL_INFO;
static bool        g_colors    = true;

/* Per-thread display name (e.g. "MAIN", "RENDER"). */
#ifdef _MSC_VER
static __declspec(thread) char tl_thread_name[32] = {0};
#else
static __thread char tl_thread_name[32] = {0};
#endif

#ifdef JCE_LOG_ASYNC
static JceLogRing   *g_ring           = NULL;
static SDL_Thread   *g_backend_thread = NULL;
static SDL_AtomicInt g_running;               /* 1 = running, 0 = stop */
static SDL_IOStream *g_log_file       = NULL; /* optional file sink    */
static SDL_Mutex    *g_file_mtx       = NULL; /* protects g_log_file   */
static SDL_Mutex    *g_flush_mtx      = NULL; /* flush completion fence */
static SDL_Condition *g_flush_cond    = NULL;
static SDL_AtomicInt g_flush_requested;
static SDL_AtomicInt g_flush_completed;

/* Rotation state.  All of it is owned by g_file_mtx, exactly like g_log_file:
   it is read and written on the backend thread (per line) and replaced by
   jce_log_set_file_ex() on whatever thread calls that. */
static char     g_log_path[1024] = {0};   /* live path; "" when no sink     */
static uint64_t g_file_bytes     = 0;     /* bytes in the LIVE file         */
static bool     g_rotate_stuck   = false; /* rotation could not shrink it   */
static JceLogFileConfig g_file_cfg = {
    JCE_LOG_FILE_DEFAULT_MAX_BYTES, JCE_LOG_FILE_DEFAULT_MAX_FILES, true
};
#endif

/* Optional observer of the emitted stream (see jce_log_set_sink).
   g_sink_mtx stays NULL until jce_log_init() and on non-threaded platforms;
   SDL mutex calls are no-ops on a NULL handle, so the lock/unlock pairs below
   are unconditional (same pattern as g_file_mtx in jce_log_set_file). */
static JceLogSinkFn  g_sink      = NULL;
static void         *g_sink_user = NULL;
static SDL_Mutex    *g_sink_mtx  = NULL;

/* -- Level metadata ------------------------------------------------ */

static const char *level_str(JceLogLevel level)
{
    switch (level) {
    case JCE_LOG_LEVEL_TRACE:   return "TRACE";
    case JCE_LOG_LEVEL_DEBUG:   return "DEBUG";
    case JCE_LOG_LEVEL_INFO:    return "INFO";
    case JCE_LOG_LEVEL_SUCCESS: return "SUCCESS";
    case JCE_LOG_LEVEL_WARN:    return "WARN";
    case JCE_LOG_LEVEL_ERROR:   return "ERROR";
    default:                    return "?";
    }
}

static const char *level_color(JceLogLevel level)
{
    switch (level) {
    case JCE_LOG_LEVEL_TRACE:   return ANSI_GRAY;
    case JCE_LOG_LEVEL_DEBUG:   return ANSI_BLUE;
    case JCE_LOG_LEVEL_INFO:    return ANSI_WHITE;
    case JCE_LOG_LEVEL_SUCCESS: return ANSI_GREEN;
    case JCE_LOG_LEVEL_WARN:    return ANSI_YELLOW;
    case JCE_LOG_LEVEL_ERROR:   return ANSI_RED;
    default:                    return ANSI_WHITE;
    }
}

/* -- Helpers ------------------------------------------------------- */

/* Strip directory path from __FILE__ (MSVC gives full paths). */
static const char *strip_path(const char *path)
{
    const char *slash  = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last   = (slash > bslash) ? slash : bslash;
    return last ? last + 1 : path;
}

typedef struct JceLogClockAnchor {
    SDL_Time wall_time;
    uint64_t monotonic_ms;
} JceLogClockAnchor;

static JceLogClockAnchor log_clock_anchor(void)
{
    JceLogClockAnchor anchor;
    uint64_t before;
    uint64_t after;

    before = jce_time_ticks_ms();
    anchor.wall_time = 0;
    SDL_GetCurrentTime(&anchor.wall_time);
    after = jce_time_ticks_ms();
    anchor.monotonic_ms =
        after >= before ? before + (after - before) / 2u : after;
    return anchor;
}

static SDL_Time log_message_wall_time(const JceLogMessage *message,
                                      const JceLogClockAnchor *anchor)
{
    uint64_t elapsed_ms;
    uint64_t elapsed_ns;

    if (anchor->wall_time <= 0 ||
        anchor->monotonic_ms < message->timestamp_ms)
        return anchor->wall_time;
    elapsed_ms = anchor->monotonic_ms - message->timestamp_ms;
    if (elapsed_ms > UINT64_MAX / UINT64_C(1000000))
        return anchor->wall_time;
    elapsed_ns = elapsed_ms * UINT64_C(1000000);
    if (elapsed_ns >= (uint64_t)anchor->wall_time)
        return anchor->wall_time;
    return anchor->wall_time - (SDL_Time)elapsed_ns;
}

/* -- File sink: size-based rotation with a zstd'd history ----------- *
 *
 * Deliberately inside JCE_LOG_ASYNC, where the file sink itself already
 * lives.  On Emscripten there is no sink, so none of this would ever run
 * there; compiling it in would only add code that exists to be dead.
 *
 * The contract this implements is written out in jce_log.h.  Everything here
 * runs with g_file_mtx held, on the backend thread for rotation and on the
 * caller's thread for (re)configuration.
 * ------------------------------------------------------------------ */

#ifdef JCE_LOG_ASYNC

#define JCE_LOG_TAG "jce_log"

/* Not 9, the level jce_archive_writer.c / jce_asset_cooker.c use.  Those
 * compress assets once, offline, where a slow best-effort pass is free.
 * Rotation runs on the log backend thread: every millisecond spent in zstd is
 * a millisecond the ring buffer is not being drained, and a full ring DROPS
 * records.  Level 3 (zstd's own default) compresses log text nearly as well
 * for a fraction of the time. */
#define JCE_LOG_ZSTD_LEVEL 3

/* "<path>.N.zst" or "<path>.N".  A generation exists in exactly one of the
 * two spellings — ".zst" when zstd succeeded for it, plain when it did not —
 * which is why every generation operation below is done twice. */
static void log_gen_path(char *out, size_t cap, unsigned gen, bool zst)
{
    if (zst) snprintf(out, cap, "%s.%u.zst", g_log_path, gen);
    else     snprintf(out, cap, "%s.%u",     g_log_path, gen);
}

/* Open (or reopen) the live file and seed the byte counter.  Caller holds
   g_file_mtx.  Leaves g_log_file NULL if the file cannot be opened. */
static void log_file_open_locked(void)
{
    Sint64 end;

    g_file_bytes = 0;
    if (!g_log_path[0]) return;
    g_log_file = SDL_IOFromFile(g_log_path, "a");
    if (!g_log_file) return;

    /* How I know the count is right after an "a" open onto an EXISTING file:
     * I do not assume it is empty.  "a" appends to whatever is already there
     * — a previous run's log, or a file that rotation could not move aside —
     * and those bytes are part of the very file this counter describes.
     * Starting the counter at 0 would under-count by exactly the pre-existing
     * size and postpone the first rotation by that much; over enough restarts
     * the live file grows past max_bytes without bound, which is the single
     * failure this sink exists to prevent.  So the starting size is read once
     * here, with a seek to the end, and never again: from this point the
     * count advances by the RETURN VALUE of SDL_WriteIO, i.e. the bytes that
     * actually landed.  No per-line stat(). */
    end = SDL_SeekIO(g_log_file, 0, SDL_IO_SEEK_END);
    if (end > 0) g_file_bytes = (uint64_t)end;
}

/* Compress the live file into "<path>.1.zst" and delete the original.
 * Returns true ONLY when the compressed generation is completely written and
 * the live file is gone, i.e. the caller has nothing left to do.  Every
 * failure path returns false with the live file still on disk, so the caller
 * can fall back to keeping it uncompressed — see log_rotate_locked(). */
static bool log_compress_live_locked(void)
{
    char          dst[1200];
    SDL_IOStream *in    = NULL;
    SDL_IOStream *out   = NULL;
    ZSTD_CCtx    *cctx  = NULL;
    uint8_t      *raw   = NULL;
    uint8_t      *comp  = NULL;
    size_t        bound;
    size_t        csize;
    Sint64        size;
    bool          ok    = false;

    in = SDL_IOFromFile(g_log_path, "rb");
    if (!in) return false;
    size = SDL_GetIOSize(in);
    if (size <= 0) { SDL_CloseIO(in); return false; }

    /* One CCtx per rotation rather than a resident one: with the default
       8 MiB threshold this is a single allocation per 8 MiB of log text. */
    raw = (uint8_t *)JCE_MALLOC((size_t)size);
    if (!raw) goto done;
    if (SDL_ReadIO(in, raw, (size_t)size) != (size_t)size) goto done;
    SDL_CloseIO(in);
    in = NULL;

    cctx = ZSTD_createCCtx();
    if (!cctx) goto done;
    bound = ZSTD_compressBound((size_t)size);
    comp  = (uint8_t *)JCE_MALLOC(bound);
    if (!comp) goto done;
    csize = ZSTD_compressCCtx(cctx, comp, bound, raw, (size_t)size,
                              JCE_LOG_ZSTD_LEVEL);
    if (ZSTD_isError(csize)) goto done;

    log_gen_path(dst, sizeof(dst), 1u, true);
    SDL_RemovePath(dst);
    out = SDL_IOFromFile(dst, "wb");
    if (!out) goto done;
    if (SDL_WriteIO(out, comp, csize) != csize) {
        SDL_CloseIO(out);
        out = NULL;
        SDL_RemovePath(dst);   /* never leave a truncated generation behind */
        goto done;
    }
    SDL_CloseIO(out);
    out = NULL;

    /* Only now is the plain original redundant. */
    SDL_RemovePath(g_log_path);
    ok = true;

done:
    if (in)   SDL_CloseIO(in);
    if (out)  SDL_CloseIO(out);
    if (cctx) ZSTD_freeCCtx(cctx);
    JCE_FREE(comp);
    JCE_FREE(raw);
    return ok;
}

/* Close the live file, shift the history down, drop the oldest generation and
 * open a fresh live file.  Caller holds g_file_mtx; in practice the caller is
 * the backend thread, from emit_message(). */
static void log_rotate_locked(void)
{
    char     from[1200];
    char     to[1200];
    unsigned i;
    bool     compressed = false;
    uint64_t before     = g_file_bytes;

    if (g_log_file) {
        SDL_FlushIO(g_log_file);
        SDL_CloseIO(g_log_file);
        g_log_file = NULL;
    }

    /* Delete the oldest generation FIRST, in BOTH spellings — the bounded
       directory is the whole point, and trading an unbounded log file for an
       unbounded log directory would be no fix at all.
       The shift below empties every other slot before refilling it, so this
       is the only slot that can accumulate.  The case that needs it is a
       compression setting that CHANGED between rotations: slot max_files then
       holds a ".zst" the shift no longer has a ".zst" source to overwrite,
       and the incoming plain generation lands beside it instead of on it.
       tests/os/core/test_jce_log_rotation.c reproduces exactly that; without
       these four lines the directory holds max_files + 1 files. */
    log_gen_path(to, sizeof(to), g_file_cfg.max_files, true);
    SDL_RemovePath(to);
    log_gen_path(to, sizeof(to), g_file_cfg.max_files, false);
    SDL_RemovePath(to);

    /* Shift the survivors down, oldest first so nothing is overwritten. */
    for (i = g_file_cfg.max_files; i > 1u; --i) {
        log_gen_path(from, sizeof(from), i - 1u, true);
        log_gen_path(to,   sizeof(to),   i,      true);
        SDL_RenamePath(from, to);    /* absent source simply fails: harmless */
        log_gen_path(from, sizeof(from), i - 1u, false);
        log_gen_path(to,   sizeof(to),   i,      false);
        SDL_RenamePath(from, to);
    }

    if (g_file_cfg.compress)
        compressed = log_compress_live_locked();
    if (!compressed) {
        /* An uncompressed log is a worse log; NO log is a worse outcome.  A
           zstd error therefore costs the ".zst", never the file. */
        log_gen_path(to, sizeof(to), 1u, false);
        SDL_RemovePath(to);
        SDL_RenamePath(g_log_path, to);
    }

    log_file_open_locked();

    if (g_file_bytes >= before) {
        /* The live file did not shrink, so neither the compress nor the
           rename moved it: something outside this process holds it (a reader
           with a write lock, a scanner, a full disk).  Retrying on the next
           line would re-compress the same bytes forever, so stop. */
        g_rotate_stuck = true;
        LOG_ERROR(JCE_LOG_TAG,
                  "could not rotate log '%s' (still %" SDL_PRIu64 " bytes); "
                  "rotation is now off for this file and it will keep growing",
                  g_log_path, g_file_bytes);
    } else if (g_file_cfg.compress && !compressed) {
        LOG_WARN(JCE_LOG_TAG,
                 "zstd failed on the rotated log; kept it uncompressed as "
                 "'%s.1'", g_log_path);
    }
}

/* Turn a caller's request into the effective config: the ONLY place the
 * defaults, the floor and the ceiling documented in jce_log.h become
 * behaviour.  jce_log_get_file_config() hands this result straight back, so
 * those documented numbers are observable instead of merely asserted. */
static void log_file_effective_config(const JceLogFileConfig *in,
                                      JceLogFileConfig *out)
{
    out->max_bytes = (in && in->max_bytes) ? in->max_bytes
                                           : JCE_LOG_FILE_DEFAULT_MAX_BYTES;
    if (out->max_bytes < JCE_LOG_FILE_MIN_MAX_BYTES)
        out->max_bytes = JCE_LOG_FILE_MIN_MAX_BYTES;

    out->max_files = (in && in->max_files) ? in->max_files
                                           : JCE_LOG_FILE_DEFAULT_MAX_FILES;
    if (out->max_files > JCE_LOG_FILE_MAX_MAX_FILES)
        out->max_files = JCE_LOG_FILE_MAX_MAX_FILES;

    out->compress = in ? in->compress : true;
}

#endif /* JCE_LOG_ASYNC */

/* Format and emit a single log message to stderr (and optionally file).
   Called only by the backend thread (async) or synchronously (WASM). */
static void emit_message(const JceLogMessage *m,
                         const JceLogClockAnchor *anchor)
{
    char ts[32];
    SDL_Time wt = log_message_wall_time(m, anchor);
    {
        SDL_DateTime dt;
        if (SDL_TimeToDateTime(wt, &dt, true)) {
            int ms = (int)((wt / INT64_C(1000000)) % 1000);
            snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second, ms);
        } else {
            snprintf(ts, sizeof(ts), "%012" SDL_PRIu64, m->timestamp_ms);
        }
    }

    const char *fname = strip_path(m->file);
    const char *lvl   = level_str(m->level);
    const char *lclr  = level_color(m->level);

    if (g_colors) {
        fprintf(stderr,
                ANSI_GRAY  "%s" ANSI_RESET " "
                ANSI_PURPLE "[%s]" ANSI_RESET " "
                "%s%s - "
                "%s: %s" ANSI_RESET " "
                ANSI_CYAN "at %s:%d" ANSI_RESET "\n",
                ts,
                m->thread_name,
                lclr, lvl,
                m->tag, m->message,
                fname, m->line);
    } else {
        fprintf(stderr, "%s [%s] %s - %s: %s at %s:%d\n",
                ts, m->thread_name, lvl, m->tag, m->message,
                fname, m->line);
    }

#ifdef JCE_LOG_ASYNC
    /* Write to log file (plain text, no ANSI). */
    SDL_LockMutex(g_file_mtx);
    if (g_log_file) {
        char file_buf[2048];
        int file_len = snprintf(file_buf, sizeof(file_buf),
                "%s [%s] %s - %s: %s at %s:%d\n",
                ts, m->thread_name, lvl, m->tag, m->message,
                fname, m->line);
        /* snprintf reports what it WOULD have written.  The fixed record
           fields cannot reach 2048 today, so this clamp never fires; it is
           here because the byte count below must equal the bytes written. */
        if (file_len > (int)sizeof(file_buf) - 1)
            file_len = (int)sizeof(file_buf) - 1;
        if (file_len > 0) {
            /* Count on the thread that wrote it, from what actually landed.
               Stat-ing the file per line would put a syscall per record on
               the one thread that has to keep up with every producer. */
            g_file_bytes += (uint64_t)SDL_WriteIO(g_log_file, file_buf,
                                                  (size_t)file_len);
            if (!g_rotate_stuck && g_file_bytes >= g_file_cfg.max_bytes)
                log_rotate_locked();
        }
    }
    SDL_UnlockMutex(g_file_mtx);
#endif

#ifdef __ANDROID__
    __android_log_print(to_android_prio(m->level), m->tag,
                        "%s at %s:%d", m->message, fname, m->line);
#endif

    /* Secondary observer, last so it can never affect the output above.
       The lock is held across the call so that jce_log_set_sink(NULL, ...)
       returns only once no sink invocation is still running. */
    SDL_LockMutex(g_sink_mtx);
    if (g_sink) {
        JceLogRecord rec;
        rec.level        = m->level;
        rec.tag          = m->tag;
        rec.message      = m->message;
        rec.file         = fname;
        rec.line         = m->line;
        rec.thread_name  = m->thread_name;
        rec.timestamp_ms = m->timestamp_ms;
        /* SDL_Time counts nanoseconds since the Unix epoch. */
        rec.wall_epoch_s = (int64_t)(wt / 1000000000);
        g_sink(&rec, g_sink_user);
    }
    SDL_UnlockMutex(g_sink_mtx);
}

/* -- Async backend thread ------------------------------------------ */

#ifdef JCE_LOG_ASYNC

#define BATCH_SIZE 64

static void log_backend_flush_outputs(void)
{
    fflush(stderr);
    SDL_LockMutex(g_file_mtx);
    if (g_log_file)
        SDL_FlushIO(g_log_file);
    SDL_UnlockMutex(g_file_mtx);
}

static void log_backend_complete_flushes(int completed)
{
    if (completed == SDL_GetAtomicInt(&g_flush_completed))
        return;

    SDL_LockMutex(g_flush_mtx);
    SDL_SetAtomicInt(&g_flush_completed, completed);
    SDL_BroadcastCondition(g_flush_cond);
    SDL_UnlockMutex(g_flush_mtx);
}

static int SDLCALL log_backend_func(void *data)
{
    (void)data;
    JceLogMessage batch[BATCH_SIZE];

    while (SDL_GetAtomicInt(&g_running)) {
        /* Sleep until signalled or 100 ms timeout (periodic flush). */
        SDL_LockMutex(g_ring->wake_mtx);
        SDL_WaitConditionTimeout(g_ring->wake_cond, g_ring->wake_mtx, 100);
        SDL_UnlockMutex(g_ring->wake_mtx);

        /*
         * Snapshot before draining.  Requests published while this batch is
         * being emitted belong to the next acknowledgement; otherwise a
         * request arriving between drain and completion could be fenced
         * before its preceding record reaches the sink.
         */
        int flush_target = SDL_GetAtomicInt(&g_flush_requested);

        /* Drain all pending messages. */
        int n;
        while ((n = jce_log_ring_pop_batch(g_ring, batch, BATCH_SIZE)) > 0) {
            JceLogClockAnchor anchor = log_clock_anchor();
            for (int i = 0; i < n; i++)
                emit_message(&batch[i], &anchor);
        }
        /* Report dropped messages. */
        int dropped = SDL_GetAtomicInt(&g_ring->dropped);
        if (dropped > 0) {
            SDL_SetAtomicInt(&g_ring->dropped, 0);
            fprintf(stderr,
                    ANSI_YELLOW "[LOG] Dropped %d messages (ring buffer full)"
                    ANSI_RESET "\n", dropped);
        }
        log_backend_flush_outputs();
        log_backend_complete_flushes(flush_target);
    }

    /* Final drain after shutdown signal. */
    int flush_target = SDL_GetAtomicInt(&g_flush_requested);
    int n;
    while ((n = jce_log_ring_pop_batch(g_ring, batch, BATCH_SIZE)) > 0) {
        JceLogClockAnchor anchor = log_clock_anchor();
        for (int i = 0; i < n; i++)
            emit_message(&batch[i], &anchor);
    }
    log_backend_flush_outputs();
    log_backend_complete_flushes(flush_target);
    return 0;
}

#endif /* JCE_LOG_ASYNC */

/* -- Public API ---------------------------------------------------- */

void jce_log_init(void)
{
#ifdef SDL_PLATFORM_WINDOWS
    /* Enable ANSI escape codes on Windows 10+ console. */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    /* The colour decision is the SAME question this call already answers.
     * GetConsoleMode fails when stderr is a file or a pipe, so its result is
     * exactly "is stderr a console?" -- and until now it was used only to turn
     * VT processing on and then discarded, while g_colors stayed
     * unconditionally true.  So `jce_editor.exe > log.txt 2>&1` wrote raw
     * ANSI escapes into the file: a persisted log full of \033[96m.
     *
     * The engine's own file sink was never the problem -- it formats a
     * separate, escape-free line (see the JCE_LOG_ASYNC block in emit()).  It
     * is stderr redirection that produces a coloured file, which is how most
     * people capture a log.
     *
     * jce_log_set_colors() still overrides this in either direction; this only
     * changes the DEFAULT from "always colour" to "colour a terminal". */
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr == INVALID_HANDLE_VALUE) {
        g_colors = false;
    } else {
        DWORD mode = 0;
        if (GetConsoleMode(hErr, &mode)) {
            SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        } else {
            g_colors = false;   /* redirected to a file or a pipe */
        }
    }
#else
    /* Same rule everywhere else, asked the POSIX way. */
    g_colors = isatty(STDERR_FILENO) ? true : false;
#endif

#ifdef JCE_LOG_ASYNC
    if (!g_ring) {
        JceLogRing *ring = jce_log_ring_create();
        SDL_Mutex *file_mtx = SDL_CreateMutex();
        SDL_Mutex *sink_mtx = SDL_CreateMutex();
        SDL_Mutex *flush_mtx = SDL_CreateMutex();
        SDL_Condition *flush_cond = SDL_CreateCondition();

        if (!ring || !file_mtx || !sink_mtx || !flush_mtx || !flush_cond) {
            if (flush_cond) SDL_DestroyCondition(flush_cond);
            if (flush_mtx) SDL_DestroyMutex(flush_mtx);
            if (sink_mtx) SDL_DestroyMutex(sink_mtx);
            if (file_mtx) SDL_DestroyMutex(file_mtx);
            if (ring) jce_log_ring_destroy(ring);
            return;
        }

        g_ring       = ring;
        g_file_mtx   = file_mtx;
        g_sink_mtx   = sink_mtx;
        g_flush_mtx  = flush_mtx;
        g_flush_cond = flush_cond;
        SDL_SetAtomicInt(&g_flush_requested, 0);
        SDL_SetAtomicInt(&g_flush_completed, 0);
        SDL_SetAtomicInt(&g_running, 1);
        /* Intentional: dedicated SDL thread instead of enkiTS — the log backend
         * must outlive the task system so that shutdown messages are still captured. */
        g_backend_thread = SDL_CreateThread(log_backend_func, "JCE-Log", NULL);
        if (!g_backend_thread) {
            SDL_SetAtomicInt(&g_running, 0);
            SDL_DestroyCondition(g_flush_cond);
            SDL_DestroyMutex(g_flush_mtx);
            SDL_DestroyMutex(g_sink_mtx);
            SDL_DestroyMutex(g_file_mtx);
            jce_log_ring_destroy(g_ring);
            g_flush_cond = NULL;
            g_flush_mtx = NULL;
            g_sink_mtx = NULL;
            g_file_mtx = NULL;
            g_ring = NULL;
        }
    }
#endif
}

void jce_log_shutdown(void)
{
#ifdef JCE_LOG_ASYNC
    if (!g_ring) return;

    /* Signal backend to stop and wait for it to drain. */
    SDL_SetAtomicInt(&g_running, 0);
    SDL_LockMutex(g_ring->wake_mtx);
    SDL_SignalCondition(g_ring->wake_cond);
    SDL_UnlockMutex(g_ring->wake_mtx);

    SDL_WaitThread(g_backend_thread, NULL);
    g_backend_thread = NULL;

    jce_log_ring_destroy(g_ring);
    g_ring = NULL;

    if (g_log_file) {
        SDL_CloseIO(g_log_file);
        g_log_file = NULL;
    }
    g_log_path[0] = '\0';
    g_file_bytes  = 0;
    /* g_file_cfg is deliberately kept, like g_min_level / g_colors / g_sink. */
    if (g_file_mtx) {
        SDL_DestroyMutex(g_file_mtx);
        g_file_mtx = NULL;
    }
    if (g_flush_cond) {
        SDL_DestroyCondition(g_flush_cond);
        g_flush_cond = NULL;
    }
    if (g_flush_mtx) {
        SDL_DestroyMutex(g_flush_mtx);
        g_flush_mtx = NULL;
    }
    /* Safe now: the backend thread is joined, so no sink call is in flight.
       g_sink itself is deliberately kept, like g_min_level / g_colors. */
    if (g_sink_mtx) {
        SDL_DestroyMutex(g_sink_mtx);
        g_sink_mtx = NULL;
    }
#endif
}

void jce_log_flush(void)
{
#ifdef JCE_LOG_ASYNC
    enum {
        FLUSH_WAIT_SLICE_MS = 20,
        FLUSH_WAIT_LIMIT_MS = 2000
    };
    if (!g_ring || !g_backend_thread ||
        !g_flush_mtx || !g_flush_cond ||
        !SDL_GetAtomicInt(&g_running)) {
        fflush(stderr);
        return;
    }

    /*
     * The ring is MPSC, not MPMC: only the backend may advance read_pos.
     * Publish a fence ticket and wait until the backend has drained all
     * records that preceded it.  The timeout keeps crash-path flushing
     * best-effort if the backend itself is the failing thread.
     */
    int ticket = SDL_AddAtomicInt(&g_flush_requested, 1) + 1;
    uint64_t deadline = SDL_GetTicks() + FLUSH_WAIT_LIMIT_MS;

    SDL_LockMutex(g_ring->wake_mtx);
    SDL_SignalCondition(g_ring->wake_cond);
    SDL_UnlockMutex(g_ring->wake_mtx);

    SDL_LockMutex(g_flush_mtx);
    while (SDL_GetAtomicInt(&g_running)) {
        uint32_t completed =
            (uint32_t)SDL_GetAtomicInt(&g_flush_completed);
        uint32_t distance = completed - (uint32_t)ticket;
        if (distance < UINT32_C(0x80000000))
            break;

        uint64_t now = SDL_GetTicks();
        if (now >= deadline)
            break;
        uint64_t remaining = deadline - now;
        int wait_ms = remaining < FLUSH_WAIT_SLICE_MS
                    ? (int)remaining : FLUSH_WAIT_SLICE_MS;
        SDL_WaitConditionTimeout(g_flush_cond, g_flush_mtx, wait_ms);
    }
    SDL_UnlockMutex(g_flush_mtx);
#else
    fflush(stderr);
#endif
}

void jce_log_set_file(const char *path)
{
    /* Rotation on, with the documented defaults.  Keeping the one existing
       caller (editor/src/core/jce_editor.cpp) source-compatible is why this
       stayed a one-argument function and the settings arrived as a sibling
       rather than as extra parameters here. */
    jce_log_set_file_ex(path, NULL);
}

void jce_log_set_file_ex(const char *path, const JceLogFileConfig *cfg)
{
#ifdef JCE_LOG_ASYNC
    SDL_LockMutex(g_file_mtx);
    if (g_log_file) {
        SDL_CloseIO(g_log_file);
        g_log_file = NULL;
    }
    g_log_path[0]  = '\0';
    g_file_bytes   = 0;
    g_rotate_stuck = false;
    log_file_effective_config(cfg, &g_file_cfg);
    if (path && path[0]) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", path);
        log_file_open_locked();
    }
    SDL_UnlockMutex(g_file_mtx);
#else
    (void)path;
    (void)cfg;
#endif
}

bool jce_log_get_file_config(JceLogFileConfig *out)
{
    bool open_now = false;

    if (!out) return false;
#ifdef JCE_LOG_ASYNC
    SDL_LockMutex(g_file_mtx);
    *out     = g_file_cfg;
    open_now = (g_log_file != NULL);
    SDL_UnlockMutex(g_file_mtx);
#else
    out->max_bytes = 0;
    out->max_files = 0;
    out->compress  = false;
#endif
    return open_now;
}

void jce_log_set_level(JceLogLevel level)
{
    g_min_level = level;
}

void jce_log_set_colors(bool enabled)
{
    g_colors = enabled;
}

void jce_log_set_thread_name(const char *name)
{
    if (name) {
        snprintf(tl_thread_name, sizeof(tl_thread_name), "%s", name);
    } else {
        tl_thread_name[0] = '\0';
    }
}

void jce_log_set_sink(JceLogSinkFn fn, void *user)
{
    SDL_LockMutex(g_sink_mtx);
    g_sink      = fn;
    g_sink_user = user;
    SDL_UnlockMutex(g_sink_mtx);
}

void jce_log_write(JceLogLevel level, const char *tag,
                   const char *file, int line,
                   const char *fmt, ...)
{
    if (level < g_min_level) return;

    /* Build the log message on the stack. */
    JceLogMessage m;
    JceLogClockAnchor anchor;
    m.level        = level;
    m.line         = line;
    m.timestamp_ms = jce_time_ticks_ms();

    snprintf(m.tag,  sizeof(m.tag),  "%s", tag  ? tag  : "");
    snprintf(m.file, sizeof(m.file), "%s", file ? file : "");

    /* Capture the calling thread's display name. */
    if (tl_thread_name[0] != '\0') {
        snprintf(m.thread_name, sizeof(m.thread_name), "%s", tl_thread_name);
    } else {
        snprintf(m.thread_name, sizeof(m.thread_name), "T-%lu",
                 (unsigned long)SDL_GetCurrentThreadID());
    }

    /* Format the user message. */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.message, sizeof(m.message), fmt, ap);
    va_end(ap);

#ifdef JCE_LOG_ASYNC
    if (g_ring) {
        jce_log_ring_push(g_ring, &m);
        return;
    }
#endif

    /* Fallback: synchronous emit (WASM, or before init / after shutdown). */
    anchor = log_clock_anchor();
    emit_message(&m, &anchor);
}

void jce_log_write_v(JceLogLevel level, const char *tag,
                     const char *file, int line,
                     const char *fmt, va_list ap)
{
    if (level < g_min_level) return;

    /* Build the log message on the stack. */
    JceLogMessage m;
    JceLogClockAnchor anchor;
    m.level        = level;
    m.line         = line;
    m.timestamp_ms = jce_time_ticks_ms();

    snprintf(m.tag,  sizeof(m.tag),  "%s", tag  ? tag  : "");
    snprintf(m.file, sizeof(m.file), "%s", file ? file : "");

    /* Capture the calling thread's display name. */
    if (tl_thread_name[0] != '\0') {
        snprintf(m.thread_name, sizeof(m.thread_name), "%s", tl_thread_name);
    } else {
        snprintf(m.thread_name, sizeof(m.thread_name), "T-%lu",
                 (unsigned long)SDL_GetCurrentThreadID());
    }

    /* Format the user message using provided va_list. */
    vsnprintf(m.message, sizeof(m.message), fmt, ap);

#ifdef JCE_LOG_ASYNC
    if (g_ring) {
        jce_log_ring_push(g_ring, &m);
        return;
    }
#endif

    /* Fallback: synchronous emit (WASM, or before init / after shutdown). */
    anchor = log_clock_anchor();
    emit_message(&m, &anchor);
}
