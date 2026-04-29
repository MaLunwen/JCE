/*
 * jce_crash_handler.c  Crash signal handler with platform-specific dialog.
 *
 * Installs signal handlers for SIGSEGV, SIGABRT, SIGFPE, SIGBUS.
 * On crash:
 *   - Captures basic crash info (signal, fault address, mini backtrace).
 *   - Logs via jce_log and __android_log_print (Android) or stderr.
 *   - Writes crash dump to crashes/crash-YYYYMMDD-HHMMSS.txt
 *   - Shows a visible error dialog so developers can read the crash info.
 *   - Re-raises the signal for normal OS crash reporting.
 */

#include <jce/os/core/jce_crash_handler.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
   Android (Bionic) or Emscripten.  Guard accordingly.
   We additionally use dladdr() to compute "<sym>+0xoff" offsets,
   which symbolises stripped release builds far more reliably than
   backtrace_symbols() alone (which falls back to "[address]"). */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
  #include <execinfo.h>
  #include <dlfcn.h>
  #include <unistd.h>
  #define HAS_BACKTRACE 1
#endif

/* Android (Bionic) lacks execinfo.h but ships libgcc-style _Unwind_*
   in <unwind.h> + dladdr() in <dlfcn.h>.  This is the canonical way
   recommended by NDK docs for in-process unwinding pre-libunwindstack. */
#if defined(__ANDROID__)
  #include <unwind.h>
  #include <dlfcn.h>
  #define HAS_ANDROID_UNWIND 1
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  #include <unistd.h>
#endif

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <SDL3/SDL.h>

  #include <windows.h>
  #include <dbghelp.h>
  #pragma comment(lib, "dbghelp.lib")
#endif

/* ------------------------------------------------------------------ */
/* Backtrace helper                                                    */
/* ------------------------------------------------------------------ */

#define MAX_BT_FRAMES 32

#ifdef HAS_ANDROID_UNWIND
/* Bionic _Unwind_Backtrace callback context. */
typedef struct {
    void   **frames;
    int      capacity;
    int      count;
} AndroidUnwindCtx;

static _Unwind_Reason_Code android_unwind_cb(struct _Unwind_Context *ctx, void *arg)
{
    AndroidUnwindCtx *u = (AndroidUnwindCtx *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && u->count < u->capacity)
        u->frames[u->count++] = (void *)pc;
    return (u->count >= u->capacity) ? _URC_END_OF_STACK : _URC_NO_REASON;
}
#endif

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
        Dl_info info;
        int written;
        if (dladdr(frames[i], &info) && info.dli_sname) {
            uintptr_t base = (uintptr_t)info.dli_saddr;
            uintptr_t pc   = (uintptr_t)frames[i];
            uintptr_t delta = pc >= base ? pc - base : 0;
            const char *mod = info.dli_fname ? info.dli_fname : "?";
            written = snprintf(buf + off, buf_size - off,
                               "  #%02d %s+0x%zx (%s) [%p]\n",
                               i, info.dli_sname, (size_t)delta,
                               mod, frames[i]);
        } else {
            written = snprintf(buf + off, buf_size - off,
                               "  #%02d %s\n", i,
                               syms ? syms[i] : "(unknown)");
        }
        if (written > 0) off += (size_t)written;
    }
    free(syms);
    return n;
#elif defined(HAS_ANDROID_UNWIND)
    void *frames[MAX_BT_FRAMES];
    AndroidUnwindCtx u = { frames, MAX_BT_FRAMES, 0 };
    _Unwind_Backtrace(android_unwind_cb, &u);
    if (u.count == 0) {
        snprintf(buf, buf_size, "  (no backtrace available)\n");
        return 0;
    }
    size_t off = 0;
    for (int i = 0; i < u.count && off < buf_size - 1; i++) {
        Dl_info info;
        int written;
        if (dladdr(frames[i], &info) && info.dli_sname) {
            uintptr_t base = (uintptr_t)info.dli_saddr;
            uintptr_t pc   = (uintptr_t)frames[i];
            uintptr_t delta = pc >= base ? pc - base : 0;
            const char *mod = info.dli_fname ? info.dli_fname : "?";
            written = snprintf(buf + off, buf_size - off,
                               "  #%02d %s+0x%zx (%s) [%p]\n",
                               i, info.dli_sname, (size_t)delta,
                               mod, frames[i]);
        } else if (dladdr(frames[i], &info) && info.dli_fname) {
            /* Stripped binary — we still have the module name + offset. */
            uintptr_t base = (uintptr_t)info.dli_fbase;
            uintptr_t pc   = (uintptr_t)frames[i];
            written = snprintf(buf + off, buf_size - off,
                               "  #%02d %s+0x%zx [%p]\n",
                               i, info.dli_fname, (size_t)(pc - base),
                               frames[i]);
        } else {
            written = snprintf(buf + off, buf_size - off,
                               "  #%02d [%p]\n", i, frames[i]);
        }
        if (written > 0) off += (size_t)written;
    }
    return u.count;
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
/* Dump helper                                                         */
/* ------------------------------------------------------------------ */

/*
 * Writes crash info to crashes/crash-YYYYMMDD-HHMMSS.txt and returns
 * the path (static buffer) so the dialog can display it.  Returns NULL
 * if write fails or path formation fails.
 */
static const char *write_crash_dump(const char *msg)
{
    time_t now_t = time(NULL);
    struct tm *now_tm = localtime(&now_t);
    if (!now_tm) return NULL;

    static char dump_path[512];
    snprintf(dump_path, sizeof(dump_path),
             "crashes/crash-%04d%02d%02d-%02d%02d%02d.txt",
             now_tm->tm_year + 1900, now_tm->tm_mon + 1, now_tm->tm_mday,
             now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec);

    if (jce_fs_host_write_all(dump_path, msg, strlen(msg))) {
        return dump_path;
    }
    return NULL;
}

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

    /* Write crash dump to disk. */
    const char *dump_path = write_crash_dump(msg);

#ifdef __ANDROID__
    /* Also log to Android logcat directly in case jce_log is broken. */
    __android_log_print(ANDROID_LOG_FATAL, "JCE",
                        "NATIVE CRASH: %s\n%s", signal_name(sig), bt_buf);

    /* Show dialog — this blocks until user dismisses. */
    char dialog_msg[4200];
    if (dump_path) {
        snprintf(dialog_msg, sizeof(dialog_msg),
                 "%s\n\nDump: %s", msg, dump_path);
    } else {
        snprintf(dialog_msg, sizeof(dialog_msg), "%s", msg);
    }
    android_show_crash_dialog("JCE Native Crash", dialog_msg);
#elif defined(_WIN32)
    char dialog_msg[4200];
    if (dump_path) {
        snprintf(dialog_msg, sizeof(dialog_msg),
                 "%s\n\nDump: %s", msg, dump_path);
    } else {
        snprintf(dialog_msg, sizeof(dialog_msg), "%s", msg);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "JCE Native Crash", dialog_msg, NULL);
#else
    /* On other Unix, just write to stderr as a last resort. */
    fprintf(stderr, "%s\n", msg);
    if (dump_path) fprintf(stderr, "\nDump: %s\n", dump_path);
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

    /* Write crash dump to disk. */
    const char *dump_path = write_crash_dump(msg);

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "JCE",
                        "NATIVE CRASH: sig=%d (%s) code=%d addr=%p\n%s",
                        sig, signal_name(sig),
                        info->si_code, info->si_addr, bt_buf);

    char dialog_msg[4200];
    if (dump_path) {
        snprintf(dialog_msg, sizeof(dialog_msg),
                 "%s\n\nDump: %s", msg, dump_path);
    } else {
        snprintf(dialog_msg, sizeof(dialog_msg), "%s", msg);
    }
    android_show_crash_dialog("JCE Native Crash", dialog_msg);
#else
    fprintf(stderr, "%s\n", msg);
    if (dump_path) fprintf(stderr, "\nDump: %s\n", dump_path);
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

    /* ── Capture symbolic backtrace via DbgHelp ───────────────────── */
    HANDLE process = GetCurrentProcess();
    static volatile LONG sym_initialized = 0;
    if (InterlockedCompareExchange(&sym_initialized, 1, 0) == 0) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS |
                      SYMOPT_UNDNAME);
        SymInitialize(process, NULL, TRUE);
    }

    void   *frames[64];
    USHORT  frame_count = CaptureStackBackTrace(0, 64, frames, NULL);

    SYMBOL_INFO *sym = (SYMBOL_INFO *)calloc(
        sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    if (sym) {
        sym->MaxNameLen   = 255;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    }

    /* Identify thread name (Windows 10+). */
    char thread_label[64] = "Thread";
    {
        DWORD tid = GetCurrentThreadId();
        PWSTR desc_w = NULL;
        HRESULT hr = GetThreadDescription(GetCurrentThread(), &desc_w);
        if (SUCCEEDED(hr) && desc_w && desc_w[0]) {
            char desc_a[64] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc_w, -1,
                                desc_a, (int)sizeof(desc_a), NULL, NULL);
            snprintf(thread_label, sizeof(thread_label),
                     "Thread '%s' (tid=%lu)", desc_a, (unsigned long)tid);
        } else {
            snprintf(thread_label, sizeof(thread_label),
                     "Thread (tid=%lu)", (unsigned long)tid);
        }
        if (desc_w) LocalFree(desc_w);
    }

    /* Build dump string with full backtrace. */
    char dump_msg[8192];
    int dump_len = snprintf(dump_msg, sizeof(dump_msg),
                            "=== JCE CRASH ===\n"
                            "Exception: 0x%08lX (%s)\n"
                            "Address:   0x%p\n"
                            "%s\n\n"
                            "Backtrace:\n",
                            ep->ExceptionRecord->ExceptionCode, exc_name,
                            ep->ExceptionRecord->ExceptionAddress, thread_label);

    /* Header — short, fits in one log line. */
    LOG_ERROR(LOG_TAG,
              "=== JCE CRASH === 0x%08lX (%s) at 0x%p  %s",
              ep->ExceptionRecord->ExceptionCode, exc_name,
              ep->ExceptionRecord->ExceptionAddress, thread_label);

    /* One log line per frame so the 512-byte ring message limit
     * doesn't truncate the backtrace.  Also append to dump_msg. */
    if (sym) {
        for (USHORT i = 0; i < frame_count && dump_len < (int)sizeof(dump_msg) - 100; i++) {
            DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
            DWORD64 displacement = 0;
            const char *name = "(unknown)";
            if (SymFromAddr(process, addr, &displacement, sym)) {
                name = sym->Name;
            }
            IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64), 0, 0, 0 };
            DWORD line_disp = 0;
            if (SymGetLineFromAddr64(process, addr, &line_disp, &line)) {
                LOG_ERROR(LOG_TAG, "  bt#%02u 0x%p %s  (%s:%lu)",
                          i, (void *)(uintptr_t)addr, name,
                          line.FileName, (unsigned long)line.LineNumber);
                dump_len += snprintf(dump_msg + dump_len, sizeof(dump_msg) - dump_len,
                                    "  bt#%02u 0x%p %s  (%s:%lu)\n",
                                    i, (void *)(uintptr_t)addr, name,
                                    line.FileName, (unsigned long)line.LineNumber);
            } else {
                LOG_ERROR(LOG_TAG, "  bt#%02u 0x%p %s",
                          i, (void *)(uintptr_t)addr, name);
                dump_len += snprintf(dump_msg + dump_len, sizeof(dump_msg) - dump_len,
                                    "  bt#%02u 0x%p %s\n",
                                    i, (void *)(uintptr_t)addr, name);
            }
        }
        free(sym);
    } else {
        LOG_ERROR(LOG_TAG, "  (failed to allocate symbol buffer)");
        dump_len += snprintf(dump_msg + dump_len, sizeof(dump_msg) - dump_len,
                            "  (failed to allocate symbol buffer)\n");
    }

    jce_log_flush();

    /* Write crash dump to disk. */
    const char *dump_path = write_crash_dump(dump_msg);

    char box_msg[1200];
    if (dump_path) {
        snprintf(box_msg, sizeof(box_msg),
                 "=== JCE CRASH ===\n"
                 "Exception: 0x%08lX (%s)\n"
                 "Address:   0x%p\n"
                 "%s\n\n"
                 "Dump: %s\n"
                 "Please report this crash.",
                 ep->ExceptionRecord->ExceptionCode, exc_name,
                 ep->ExceptionRecord->ExceptionAddress, thread_label,
                 dump_path);
    } else {
        snprintf(box_msg, sizeof(box_msg),
                 "=== JCE CRASH ===\n"
                 "Exception: 0x%08lX (%s)\n"
                 "Address:   0x%p\n"
                 "%s\n\n"
                 "See log file for full backtrace.\n"
                 "Please report this crash.",
                 ep->ExceptionRecord->ExceptionCode, exc_name,
                 ep->ExceptionRecord->ExceptionAddress, thread_label);
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "JCE Native Crash", box_msg, NULL);

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

void jce_crash_handler_shutdown(void)
{
#if defined(_WIN32)
    /* Restore default unhandled exception filter. */
    SetUnhandledExceptionFilter(NULL);

#elif defined(__EMSCRIPTEN__)
    /* Restore default signal handler for SIGABRT. */
    signal(SIGABRT, SIG_DFL);

#else
    /* Unix / Android / macOS: restore default signal handlers. */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    signal(SIGFPE,  SIG_DFL);
#ifdef SIGBUS
    signal(SIGBUS,  SIG_DFL);
#endif

#endif /* platform */

    LOG_DEBUG(LOG_TAG, "crash handler uninstalled");
}
