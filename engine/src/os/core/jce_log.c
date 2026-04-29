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
#endif

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

/* Format and emit a single log message to stderr (and optionally file).
   Called only by the backend thread (async) or synchronously (WASM). */
static void emit_message(const JceLogMessage *m)
{
    /* Timestamp string.  wall_time is resolved here (backend / sync
       fallback) so the producer path never calls SDL_GetCurrentTime(). */
    char ts[32];
    {
        SDL_Time wt = m->wall_time;
        if (wt == 0) SDL_GetCurrentTime(&wt);

        SDL_DateTime dt;
        if (SDL_TimeToDateTime(wt, &dt, true)) {
            int ms = (int)(m->timestamp_ms % 1000);
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
    if (g_log_file) {
        char file_buf[2048];
        int file_len = snprintf(file_buf, sizeof(file_buf),
                "%s [%s] %s - %s: %s at %s:%d\n",
                ts, m->thread_name, lvl, m->tag, m->message,
                fname, m->line);
        SDL_LockMutex(g_file_mtx);
        if (file_len > 0)
            SDL_WriteIO(g_log_file, file_buf, (size_t)file_len);
        SDL_UnlockMutex(g_file_mtx);
    }
#endif

#ifdef __ANDROID__
    __android_log_print(to_android_prio(m->level), m->tag,
                        "%s at %s:%d", m->message, fname, m->line);
#endif
}

/* -- Async backend thread ------------------------------------------ */

#ifdef JCE_LOG_ASYNC

#define BATCH_SIZE 64

static int SDLCALL log_backend_func(void *data)
{
    (void)data;
    JceLogMessage batch[BATCH_SIZE];

    while (SDL_GetAtomicInt(&g_running)) {
        /* Sleep until signalled or 100 ms timeout (periodic flush). */
        SDL_LockMutex(g_ring->wake_mtx);
        SDL_WaitConditionTimeout(g_ring->wake_cond, g_ring->wake_mtx, 100);
        SDL_UnlockMutex(g_ring->wake_mtx);

        /* Drain all pending messages. */
        int n;
        while ((n = jce_log_ring_pop_batch(g_ring, batch, BATCH_SIZE)) > 0) {
            for (int i = 0; i < n; i++)
                emit_message(&batch[i]);
        }
        fflush(stderr);
        if (g_log_file) {
            SDL_LockMutex(g_file_mtx);
            SDL_FlushIO(g_log_file);
            SDL_UnlockMutex(g_file_mtx);
        }

        /* Report dropped messages. */
        int dropped = SDL_GetAtomicInt(&g_ring->dropped);
        if (dropped > 0) {
            SDL_SetAtomicInt(&g_ring->dropped, 0);
            fprintf(stderr,
                    ANSI_YELLOW "[LOG] Dropped %d messages (ring buffer full)"
                    ANSI_RESET "\n", dropped);
        }
    }

    /* Final drain after shutdown signal. */
    int n;
    while ((n = jce_log_ring_pop_batch(g_ring, batch, BATCH_SIZE)) > 0) {
        for (int i = 0; i < n; i++)
            emit_message(&batch[i]);
    }
    fflush(stderr);
    if (g_log_file) {
        SDL_LockMutex(g_file_mtx);
        SDL_FlushIO(g_log_file);
        SDL_UnlockMutex(g_file_mtx);
    }
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
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hErr, &mode)) {
            SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif

#ifdef JCE_LOG_ASYNC
    if (!g_ring) {
        g_ring     = jce_log_ring_create();
        g_file_mtx = SDL_CreateMutex();
        SDL_SetAtomicInt(&g_running, 1);
        /* Intentional: dedicated SDL thread instead of enkiTS — the log backend
         * must outlive the task system so that shutdown messages are still captured. */
        g_backend_thread = SDL_CreateThread(log_backend_func, "JCE-Log", NULL);
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
    if (g_file_mtx) {
        SDL_DestroyMutex(g_file_mtx);
        g_file_mtx = NULL;
    }
#endif
}

void jce_log_flush(void)
{
#ifdef JCE_LOG_ASYNC
    /* Best-effort synchronous drain — used by crash handlers.
       Reads directly from the ring without the backend thread.
       NOT safe to call concurrently with the backend, but in a
       crash context the backend may be dead anyway. */
    if (!g_ring) return;

    JceLogMessage tmp;
    while (jce_log_ring_pop_batch(g_ring, &tmp, 1) > 0)
        emit_message(&tmp);

    fflush(stderr);
    if (g_log_file)
        SDL_FlushIO(g_log_file);
#else
    fflush(stderr);
#endif
}

void jce_log_set_file(const char *path)
{
#ifdef JCE_LOG_ASYNC
    SDL_LockMutex(g_file_mtx);
    if (g_log_file) {
        SDL_CloseIO(g_log_file);
        g_log_file = NULL;
    }
    if (path) {
        g_log_file = SDL_IOFromFile(path, "a");
    }
    SDL_UnlockMutex(g_file_mtx);
#else
    (void)path;
#endif
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

void jce_log_write(JceLogLevel level, const char *tag,
                   const char *file, int line,
                   const char *fmt, ...)
{
    if (level < g_min_level) return;

    /* Build the log message on the stack. */
    JceLogMessage m;
    m.level        = level;
    m.line         = line;
    m.timestamp_ms = jce_time_ticks_ms();
    /* wall_time is derived by the backend thread (emit_message) to
       keep the producer hot path free of SDL_GetCurrentTime() syscalls.
       Set to 0 as a sentinel; emit_message fills it with current time. */
    m.wall_time    = 0;

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
    emit_message(&m);
}

void jce_log_write_v(JceLogLevel level, const char *tag,
                     const char *file, int line,
                     const char *fmt, va_list ap)
{
    if (level < g_min_level) return;

    /* Build the log message on the stack. */
    JceLogMessage m;
    m.level        = level;
    m.line         = line;
    m.timestamp_ms = jce_time_ticks_ms();
    m.wall_time    = 0;

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
    emit_message(&m);
}
