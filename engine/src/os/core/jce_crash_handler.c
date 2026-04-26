/*
 * jce_crash_handler.c  Crash signal handler with platform-specific dialog.
 *
 * Installs signal handlers for SIGSEGV, SIGABRT, SIGFPE, SIGBUS.
 * On crash:
 *   - Captures basic crash info (signal, fault address, mini backtrace).
 *   - Logs via jce_log and __android_log_print (Android) or stderr.
 *   - Shows a visible error dialog so developers can read the crash info.
 *   - Re-raises the signal for normal OS crash reporting.
 */

#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_crash"

/* ------------------------------------------------------------------ */
/* Platform-specific includes                                          */
/* ------------------------------------------------------------------ */

#ifdef __ANDROID__
  #include <android/log.h>
  #include <jni.h>
  #include <SDL3/SDL.h>
  #include <unistd.h>
#endif

/* backtrace() is available on glibc Linux and macOS, but NOT on
   Android (Bionic) or Emscripten.  Guard accordingly. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
  #include <execinfo.h>
  #include <unistd.h>
  #define HAS_BACKTRACE 1
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  #include <unistd.h>
#endif

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <SDL3/SDL.h>
#endif

/* ------------------------------------------------------------------ */
/* Backtrace helper                                                    */
/* ------------------------------------------------------------------ */

#define MAX_BT_FRAMES 32

static int capture_backtrace(char *buf, size_t buf_size)
{
    buf[0] = '\0';

#ifdef HAS_BACKTRACE
    void *frames[MAX_BT_FRAMES];
    int n = backtrace(frames, MAX_BT_FRAMES);
    if (n <= 0) {
        snprintf(buf, buf_size, "  (no backtrace available)\n");
        return 0;
    }

    char **syms = backtrace_symbols(frames, n);
    size_t off = 0;
    for (int i = 0; i < n && off < buf_size - 1; i++) {
        int written = snprintf(buf + off, buf_size - off,
                               "  #%02d %s\n", i,
                               syms ? syms[i] : "(unknown)");
        if (written > 0) off += (size_t)written;
    }
    free(syms);
    return n;
#else
    snprintf(buf, buf_size, "  (backtrace not available on this platform)\n");
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Signal name helper                                                  */
/* ------------------------------------------------------------------ */

static const char *signal_name(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGFPE:  return "SIGFPE (Floating-point exception)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS (Bus error)";
#endif
        default:      return "Unknown signal";
    }
}

/* ------------------------------------------------------------------ */
/* Android: show crash dialog via JNI                                  */
/* ------------------------------------------------------------------ */

#ifdef __ANDROID__

static void android_show_crash_dialog(const char *title, const char *message)
{
    /* Use SDL's message box — it works from the native thread and
       blocks until the user dismisses it.  SDL internally uses the
       Android UI thread via JNI. */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
}

#endif /* __ANDROID__ */

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

static void crash_signal_handler(int sig)
{
    /* Build crash message. */
    char bt_buf[2048];
    capture_backtrace(bt_buf, sizeof(bt_buf));

    char msg[4096];
    snprintf(msg, sizeof(msg),
             "=== JCE CRASH ===\n"
             "Signal: %d (%s)\n\n"
             "Backtrace:\n%s\n"
             "Please report this crash with the above information.",
             sig, signal_name(sig), bt_buf);

    /* Log + flush the async ring buffer so the message is visible. */
    LOG_ERROR(LOG_TAG, "%s", msg);
    jce_log_flush();

#ifdef __ANDROID__
    /* Also log to Android logcat directly in case jce_log is broken. */
    __android_log_print(ANDROID_LOG_FATAL, "JCE",
                        "NATIVE CRASH: %s\n%s", signal_name(sig), bt_buf);

    /* Show dialog — this blocks until user dismisses. */
    android_show_crash_dialog("JCE Native Crash", msg);
#elif defined(_WIN32)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "JCE Native Crash", msg, NULL);
#else
    /* On other Unix, just write to stderr as a last resort. */
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
#endif

    /* Re-raise with default handler so the OS produces a core dump / crash report. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ------------------------------------------------------------------ */
/* sigaction-based handler for platforms that support si_addr           */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

static void crash_sigaction_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;

    char bt_buf[2048];
    capture_backtrace(bt_buf, sizeof(bt_buf));

    const char *code_str = "";
    if (sig == SIGSEGV) {
        switch (info->si_code) {
            case 1: code_str = "SEGV_MAPERR (address not mapped)"; break;
            case 2: code_str = "SEGV_ACCERR (invalid permissions)"; break;
            default: code_str = "(unknown code)"; break;
        }
    }

    char msg[4096];
    snprintf(msg, sizeof(msg),
             "=== JCE CRASH ===\n"
             "Signal: %d (%s)\n"
             "Code: %d %s\n"
             "Fault address: %p\n\n"
             "Backtrace:\n%s\n"
             "Please report this crash with the above information.",
             sig, signal_name(sig),
             info->si_code, code_str,
             info->si_addr,
             bt_buf);

    LOG_ERROR(LOG_TAG, "%s", msg);
    jce_log_flush();

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "JCE",
                        "NATIVE CRASH: sig=%d (%s) code=%d addr=%p\n%s",
                        sig, signal_name(sig),
                        info->si_code, info->si_addr, bt_buf);

    android_show_crash_dialog("JCE Native Crash", msg);
#else
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
#endif

    /* Re-raise with default handler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

#endif /* !_WIN32 && !__EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* Windows: Structured Exception Handler                               */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS *ep)
{
    const char *exc_name = "Unknown exception";
    switch (ep->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:    exc_name = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:      exc_name = "STACK_OVERFLOW"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:  exc_name = "FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  exc_name = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: exc_name = "ILLEGAL_INSTRUCTION"; break;
    }

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "=== JCE CRASH ===\n"
             "Exception: 0x%08lX (%s)\n"
             "Address: 0x%p\n\n"
             "Please report this crash.",
             ep->ExceptionRecord->ExceptionCode, exc_name,
             ep->ExceptionRecord->ExceptionAddress);

    LOG_ERROR(LOG_TAG, "%s", msg);
    jce_log_flush();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "JCE Native Crash", msg, NULL);

    return EXCEPTION_CONTINUE_SEARCH;
}

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void jce_crash_handler_init(void)
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(windows_exception_handler);

#elif defined(__EMSCRIPTEN__)
    /* Emscripten: just use basic signal() for SIGABRT. */
    signal(SIGABRT, crash_signal_handler);

#else
    /* Unix / Android / macOS: use sigaction for fault address info. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = crash_sigaction_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS,  &sa, NULL);
#endif

#endif /* platform */

    LOG_DEBUG(LOG_TAG, "crash handler installed");
}
