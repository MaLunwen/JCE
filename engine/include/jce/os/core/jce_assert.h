/*
 * jce_assert.h  Engine assertions that survive the configuration that ships.
 *
 * WHY THIS EXISTS, measured rather than asserted.  Before this header the
 * tree's only invariant check was bare <assert.h>, and CMake's default
 * Release flags carry /DNDEBUG, so every one of them compiled to nothing in
 * the build that reaches a player.  That is not merely "less checking in
 * release": it changed what could be TESTED.  tests/middleware/net's three
 * self-test wrappers had to be registered inside
 *
 *     if(CMAKE_BUILD_TYPE STREQUAL "Debug")
 *
 * with a comment saying out loud that a Release registration "would be a test
 * that passes without checking anything".  The exclusion was honest and it
 * was still a hole: the configuration nobody ships was the only one those
 * invariants ran in.
 *
 * THE THREE FLAVOURS, and they are not interchangeable:
 *
 *   JCE_ASSERT   A broken invariant the program cannot sensibly continue past.
 *                Logs at ERROR through jce_log and then aborts, so
 *                jce_crash_handler's SIGABRT trap produces the backtrace and
 *                the message box.  COMPILED IN BY DEFAULT IN EVERY
 *                CONFIGURATION, Release included -- that is the entire point.
 *                A project that wants them gone builds with JCE_ASSERTS_OFF.
 *
 *   JCE_ENSURE   A broken invariant the program CAN continue past, reported
 *                once per site and never again, and NEVER stripped.  Evaluates
 *                to the condition, so it reads as the branch it guards:
 *                    if (!JCE_ENSURE(slot < cap)) return;
 *                One predictable branch plus an already-taken flag is a price
 *                worth paying in a shipped build for knowing the invariant
 *                broke.  This is the one to reach for by default, and
 *                JCE_ENSURE_ALWAYS drops the once-per-site memory when the
 *                SECOND occurrence is the evidence.
 *
 *   JCE_VERIFY   JCE_ASSERT whose EXPRESSION still runs when assertions are
 *                compiled out.  For the classic footgun, `assert(init())`,
 *                where stripping the check also strips the work.
 *
 * TESTABILITY.  jce_assert_set_handler installs a hook that runs INSTEAD of
 * the abort, which is how the facility's own unit test exercises a failing
 * assertion without killing the test process.  Passing NULL restores the
 * default.  The hook does not suppress the log line.
 */

#ifndef JCE_ASSERT_H
#define JCE_ASSERT_H


#include <jce/os/core/jce_defs.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Reported once per failing site.  `fmt` is NULL for the message-less forms. */
typedef void (JCE_CALL *JceAssertHandler)(const char *file, int line,
                                          const char *func, const char *expr,
                                          const char *message, void *user);

/* Install a handler that runs INSTEAD of abort() for JCE_ASSERT / JCE_VERIFY.
 * NULL restores the default (log, flush, abort).  Returns the previous one so
 * a test can nest.  Not thread-safe against concurrent assertion failures --
 * install it before the code under test runs. */
JCE_API JceAssertHandler JCE_CALL jce_assert_set_handler(JceAssertHandler fn,
                                                         void *user);

/* How many JCE_ASSERT / JCE_VERIFY failures have been reported this process.
 * Zero in a healthy run; a test that installs a handler reads this to prove a
 * failure actually happened rather than trusting that it did. */
JCE_API uint64_t JCE_CALL jce_assert_failure_count(void);

/* How many DISTINCT JCE_ENSURE sites have fired.  A repeat from the same site
 * does not increment it -- that is what "once per site" means. */
JCE_API uint64_t JCE_CALL jce_ensure_site_count(void);

/* Forget every remembered JCE_ENSURE site, so a site that already fired will
 * report again.  For tests; a shipped build never calls it. */
JCE_API void JCE_CALL jce_ensure_reset(void);

/* The reporting entry points.  Call the macros, not these. */
JCE_API void JCE_CALL jce_assert_fail(const char *file, int line,
                                      const char *func, const char *expr,
                                      const char *fmt, ...);

/* Always returns false, so `(cond) || jce_ensure_fail(...)` is the condition's
 * own truth value and the macro can be used as an expression. */
JCE_API bool JCE_CALL jce_ensure_fail(const char *file, int line,
                                      const char *func, const char *expr,
                                      const char *fmt, ...);

/* The same, without the once-per-site memory: every failure reports. */
JCE_API bool JCE_CALL jce_ensure_fail_always(const char *file, int line,
                                             const char *func, const char *expr,
                                             const char *fmt, ...);

/* -- The macros ---------------------------------------------------------- */

#if defined(JCE_ASSERTS_OFF) && JCE_ASSERTS_OFF
#  define JCE_ASSERTS_ENABLED 0
#else
#  define JCE_ASSERTS_ENABLED 1
#endif

#if JCE_ASSERTS_ENABLED

/* The ternary keeps this an expression and evaluates `cond` exactly once. */
#define JCE_ASSERT(cond) \
    ((cond) ? (void)0 \
            : jce_assert_fail(__FILE__, __LINE__, __func__, #cond, (const char *)0))

/* `...` is the whole printf argument list starting with the format, so
 * __VA_ARGS__ is never empty and no ##-extension is needed. */
#define JCE_ASSERTF(cond, ...) \
    ((cond) ? (void)0 \
            : jce_assert_fail(__FILE__, __LINE__, __func__, #cond, __VA_ARGS__))

#define JCE_VERIFY(expr)          JCE_ASSERT(expr)
#define JCE_VERIFYF(expr, ...)    JCE_ASSERTF(expr, __VA_ARGS__)

#else

#define JCE_ASSERT(cond)          ((void)0)
#define JCE_ASSERTF(cond, ...)    ((void)0)
/* THE EXPRESSION STILL RUNS.  That is the whole difference from JCE_ASSERT. */
#define JCE_VERIFY(expr)          ((void)(expr))
#define JCE_VERIFYF(expr, ...)    ((void)(expr))

#endif /* JCE_ASSERTS_ENABLED */

/* NEVER stripped: a shipped build is exactly where you want to learn that an
 * invariant broke without taking the process down with it. */
#define JCE_ENSURE(cond) \
    ((cond) ? true \
            : jce_ensure_fail(__FILE__, __LINE__, __func__, #cond, (const char *)0))

#define JCE_ENSUREF(cond, ...) \
    ((cond) ? true \
            : jce_ensure_fail(__FILE__, __LINE__, __func__, #cond, __VA_ARGS__))

/* EVERY failure reports, not just the first from this site.
 *
 * Which of the two is the right default depends on the call site, so both
 * exist and neither is a wrapper for the other.  Once-per-site is for an
 * invariant that could break every frame, where the log volume would become
 * the failure.  ALWAYS is for one that should never break twice -- a count
 * that drifts, a handle freed twice -- where the SECOND occurrence is the
 * evidence and hiding it is the bug.
 *
 * It is also the more common shape among the engines this one is measured
 * against: Unity's Debug.Assert and Godot's ERR_FAIL_COND report every time,
 * while Unreal's ensure is once and ensureAlways is the opt-out.  Shipping
 * only the once-per-site form would have silently made the rarer choice the
 * only one available. */
#define JCE_ENSURE_ALWAYS(cond) \
    ((cond) ? true \
            : jce_ensure_fail_always(__FILE__, __LINE__, __func__, #cond, \
                                     (const char *)0))

#define JCE_ENSUREF_ALWAYS(cond, ...) \
    ((cond) ? true \
            : jce_ensure_fail_always(__FILE__, __LINE__, __func__, #cond, \
                                     __VA_ARGS__))

JCE_EXTERN_C_END

#endif /* JCE_ASSERT_H */
