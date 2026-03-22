/*
 * jce_log.c  Structured logging with ANSI colors.
 *
 * Output format (matching Java JceLogger):
 *   2026-03-09 14:23:45.123 [MAIN] INFO - jce_renderer: initialized at jce_renderer.c:98
 */

#include "jce_log.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
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

/* -- Public API ---------------------------------------------------- */

void jce_log_init(void)
{
#ifdef _WIN32
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

    /* Format the user message. */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Timestamp: date/time from C library + milliseconds from SDL ticks. */
    char ts[32];
    {
        time_t now = time(NULL);
        const struct tm *lt = localtime(&now);
        int ms = (int)(SDL_GetTicks() % 1000);
        if (lt) {
            snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                     lt->tm_hour, lt->tm_min, lt->tm_sec, ms);
        } else {
            snprintf(ts, sizeof(ts), "%012" SDL_PRIu64, SDL_GetTicks());
        }
    }

    const char *fname = strip_path(file);
    const char *lvl   = level_str(level);
    const char *lclr  = level_color(level);

    /* Resolve thread display name: explicit name or numeric ID. */
    char tname[32];
    if (tl_thread_name[0] != '\0') {
        snprintf(tname, sizeof(tname), "%s", tl_thread_name);
    } else {
        snprintf(tname, sizeof(tname), "T-%lu", (unsigned long)SDL_GetCurrentThreadID());
    }

    if (g_colors) {
        fprintf(stderr,
                ANSI_GRAY  "%s" ANSI_RESET " "
                ANSI_PURPLE "[%s]" ANSI_RESET " "
                "%s%s - "
                "%s: %s" ANSI_RESET " "
                ANSI_CYAN "at %s:%d" ANSI_RESET "\n",
                ts,
                tname,
                lclr, lvl,
                tag, msg,
                fname, line);
    } else {
        fprintf(stderr, "%s [%s] %s - %s: %s at %s:%d\n",
                ts, tname, lvl, tag, msg, fname, line);
    }

#ifdef __ANDROID__
    __android_log_print(to_android_prio(level), tag,
                        "%s at %s:%d", msg, fname, line);
#endif
}
