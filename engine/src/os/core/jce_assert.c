/*
 * jce_assert.c  The reporting half of <jce/os/core/jce_assert.h>.
 *
 * Both entry points do the same three things in the same order -- format the
 * message, put one line through jce_log at ERROR, then diverge on what a
 * caller is allowed to do next.  JCE_ASSERT cannot continue and aborts;
 * JCE_ENSURE can, and returns false so the caller's own guard takes over.
 *
 * WHY THE LOG IS FLUSHED BEFORE ABORTING.  jce_log is asynchronous: records
 * go into an MPSC ring that a backend thread drains.  abort() does not give
 * that thread a chance to run, so without the explicit jce_log_flush the one
 * line that explains the crash is the line most likely to be lost.  The flush
 * is bounded and best-effort by jce_log's own contract, which is the right
 * trade on a path that is about to terminate the process anyway.
 */
#include <jce/os/core/jce_assert.h>
#include <jce/os/core/jce_log.h>

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define JCE_ASSERT_MSG_MAX 512


/* -- JCE_ENSURE's "once per site" ---------------------------------------- *
 *
 * A site is (file pointer, line).  __FILE__ is a string literal, so every
 * failure at one site presents the SAME pointer and the compare is a pointer
 * compare, not a strcmp -- which matters because this runs on the failure
 * path of code that may be failing every frame.
 *
 * Lock-free rather than mutexed because an assertion facility must not be the
 * thing that deadlocks: a JCE_ENSURE inside a mutex's own error path would
 * re-enter it.  Slots are claimed with one CAS and never released.
 *
 * THE TABLE IS BOUNDED AND SAYS SO.  Past JCE_ENSURE_SITES distinct sites the
 * probe gives up and reports the failure again -- noisier, never silent, and
 * never a wild write.  256 distinct BROKEN invariants in one process is not a
 * situation where log volume is the problem.
 */
#define JCE_ENSURE_SITES 256

#define SITE_EMPTY    0
#define SITE_CLAIMING 1
#define SITE_READY    2

/* ONE STRUCT, NOT SIX NAMES.  The dedup audit's global-state detector asks
 * for exactly this and has asked before: the text shape cache landed as six
 * loose statics, was caught, and became one (855->859 became 855->856).  It
 * is also the honest shape -- these six fields are one module's state and
 * are only ever touched together. */
static struct {
    JceAssertHandler handler;
    void            *handler_user;
    SDL_AtomicInt    failures;
    struct { const char *file; int line; } sites[JCE_ENSURE_SITES];
    SDL_AtomicInt    site_state[JCE_ENSURE_SITES];
    SDL_AtomicInt    site_count;
} g_a;

static size_t site_hash(const char *file, int line)
{
    uintptr_t h = (uintptr_t)file;
    h ^= (uintptr_t)(unsigned)line * 0x9E3779B9u;
    h ^= h >> 16;
    return (size_t)(h & (JCE_ENSURE_SITES - 1));
}

/* true if this site has already been reported; claims it otherwise. */
static bool site_already_seen(const char *file, int line)
{
    size_t i = site_hash(file, line);
    for (size_t probe = 0; probe < JCE_ENSURE_SITES; ++probe) {
        SDL_AtomicInt *st = &g_a.site_state[i];
        int cur = SDL_GetAtomicInt(st);

        if (cur == SITE_EMPTY) {
            if (SDL_CompareAndSwapAtomicInt(st, SITE_EMPTY, SITE_CLAIMING)) {
                g_a.sites[i].file = file;
                g_a.sites[i].line = line;
                SDL_SetAtomicInt(st, SITE_READY);
                SDL_AddAtomicInt(&g_a.site_count, 1);
                return false;          /* first time: report it */
            }
            cur = SDL_GetAtomicInt(st); /* lost the race; fall through to compare */
        }
        /* A slot mid-claim by another thread is not yet comparable.  Spinning
         * on it would block a failure path, so treat it as "not mine" and
         * probe on; the worst case is one extra line for one site. */
        if (cur == SITE_READY &&
            g_a.sites[i].file == file && g_a.sites[i].line == line)
            return true;

        i = (i + 1u) & (JCE_ENSURE_SITES - 1u);
    }
    return false;                      /* table full -- report rather than hide */
}

/* -- shared formatting --------------------------------------------------- */

static void format_message(char *out, size_t cap, const char *fmt, va_list ap)
{
    if (!fmt) { out[0] = '\0'; return; }
    vsnprintf(out, cap, fmt, ap);
}

/* -- public -------------------------------------------------------------- */

JceAssertHandler jce_assert_set_handler(JceAssertHandler fn, void *user)
{
    JceAssertHandler prev = g_a.handler;
    g_a.handler = fn;
    g_a.handler_user = user;
    return prev;
}

uint64_t jce_assert_failure_count(void)
{
    return (uint64_t)(unsigned)SDL_GetAtomicInt(&g_a.failures);
}

uint64_t jce_ensure_site_count(void)
{
    return (uint64_t)(unsigned)SDL_GetAtomicInt(&g_a.site_count);
}

void jce_ensure_reset(void)
{
    for (size_t i = 0; i < JCE_ENSURE_SITES; ++i) {
        SDL_SetAtomicInt(&g_a.site_state[i], SITE_EMPTY);
        g_a.sites[i].file = NULL;
        g_a.sites[i].line = 0;
    }
    SDL_SetAtomicInt(&g_a.site_count, 0);
}

void jce_assert_fail(const char *file, int line, const char *func,
                     const char *expr, const char *fmt, ...)
{
    char msg[JCE_ASSERT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    format_message(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SDL_AddAtomicInt(&g_a.failures, 1);

    if (msg[0])
        jce_log_write(JCE_LOG_LEVEL_ERROR, "assert",
                      "ASSERT FAILED: %s -- %s (%s at %s:%d)",
                      expr, msg, func ? func : "?", file ? file : "?", line);
    else
        jce_log_write(JCE_LOG_LEVEL_ERROR, "assert",
                      "ASSERT FAILED: %s (%s at %s:%d)",
                      expr, func ? func : "?", file ? file : "?", line);

    /* Before the handler, not after: a handler that longjmps out of a test
     * would otherwise take the explanation with it. */
    jce_log_flush();

    if (g_a.handler) {
        g_a.handler(file, line, func, expr, msg[0] ? msg : NULL,
                    g_a.handler_user);
        return;                        /* the hook owns what happens next */
    }
    abort();                           /* jce_crash_handler traps SIGABRT */
}

static void ensure_report(const char *file, int line, const char *func,
                          const char *expr, const char *msg, const char *note)
{
    if (msg && msg[0])
        jce_log_write(JCE_LOG_LEVEL_ERROR, "ensure",
                      "ENSURE FAILED: %s -- %s (%s at %s:%d)%s",
                      expr, msg, func ? func : "?", file ? file : "?", line,
                      note);
    else
        jce_log_write(JCE_LOG_LEVEL_ERROR, "ensure",
                      "ENSURE FAILED: %s (%s at %s:%d)%s",
                      expr, func ? func : "?", file ? file : "?", line, note);
}

bool jce_ensure_fail(const char *file, int line, const char *func,
                     const char *expr, const char *fmt, ...)
{
    if (site_already_seen(file, line))
        return false;

    char msg[JCE_ASSERT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    format_message(msg, sizeof msg, fmt, ap);
    va_end(ap);

    ensure_report(file, line, func, expr, msg, " [reported once]");
    return false;
}

bool jce_ensure_fail_always(const char *file, int line, const char *func,
                            const char *expr, const char *fmt, ...)
{
    /* Deliberately does NOT consult the site table and does NOT claim a slot.
     * Claiming one would let an ALWAYS site exhaust the 256 entries that the
     * once-per-site form depends on, so the loud flavour could silence the
     * quiet one. */
    char msg[JCE_ASSERT_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    format_message(msg, sizeof msg, fmt, ap);
    va_end(ap);

    ensure_report(file, line, func, expr, msg, "");
    return false;
}
