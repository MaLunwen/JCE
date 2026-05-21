/*
 * jce_coroutine.h  Cooperative coroutine yield primitives (P3-B.4).
 *
 * Unity-parity surface area:
 *   WaitForSeconds       → JCE_COROUTINE_WAIT_SECONDS
 *   WaitForFixedUpdate   → JCE_COROUTINE_WAIT_FIXED_UPDATE
 *   WaitForEndOfFrame    → JCE_COROUTINE_WAIT_END_OF_FRAME
 *   WaitUntil            → JCE_COROUTINE_WAIT_UNTIL
 *   yield return null    → JCE_COROUTINE_WAIT_NEXT_FRAME
 *
 * Implementation model: this is NOT stackful — no fibers, asm, longjmp,
 * or platform context switching (those break the cross-platform C99 +
 * 512 MB / single-core baseline contract).  Instead the scheduler is a
 * deferred-callback dispatcher hooked into the PlayerLoop phases the
 * caller asked to wake on.  Each coroutine is a plain `JceCoroutineFn`
 * that the user writes as a small state machine; on each resume it
 * fills `next_wait` (or returns false to exit).  This is exactly how
 * Unity's IEnumerator coroutines execute under the hood — IEnumerators
 * advance on the main thread between PlayerLoop phases, they are not
 * real coroutines either.
 *
 * Threading: main thread only.  Start / cancel and the scheduler tick
 * all run on the thread that owns the PlayerLoop.
 *
 * Layer: L5 (runtime).  Consumed via <jce/api_runtime.h>.
 */

#ifndef JCE_COROUTINE_H
#define JCE_COROUTINE_H

#include <jce/os/core/jce_defs.h>
#include <jce/runtime/jce_player_loop.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque handle.  Encodes (generation << 32) | slot_index so a stale
 * handle whose slot has been recycled compares unequal to the new
 * occupant.  Zero is reserved as the invalid sentinel. */
typedef uint64_t JceCoroutineHandle;
#define JCE_COROUTINE_INVALID ((JceCoroutineHandle)0)

/* Maximum simultaneously-alive coroutines.  Static pool, no heap. */
#ifndef JCE_COROUTINE_MAX_ACTIVE
#define JCE_COROUTINE_MAX_ACTIVE 256u
#endif

typedef struct JceCoroutineWait JceCoroutineWait;

/* User callback executed when the coroutine resumes.
 *   - return false: coroutine is finished, scheduler frees the slot.
 *   - return true : coroutine yielded again; the function MUST have
 *                   populated *next_wait describing the new wait spec.
 * `next_wait` is zero-initialised on entry. */
typedef bool (*JceCoroutineFn)(void *user, JceCoroutineWait *next_wait);

/* Predicate polled every Update for WAIT_UNTIL coroutines.  Return true
 * to resume the coroutine on this tick. */
typedef bool (*JceCoroutineUntilFn)(void *user);

typedef enum JceCoroutineWaitKind {
    JCE_COROUTINE_WAIT_NEXT_FRAME = 0, /* resume on next Update phase   */
    JCE_COROUTINE_WAIT_SECONDS,        /* wall-time elapsed via Update  */
    JCE_COROUTINE_WAIT_FIXED_UPDATE,   /* resume on next FixedUpdate    */
    JCE_COROUTINE_WAIT_END_OF_FRAME,   /* resume in EndOfFrame this frame
                                        * (or next, if already past)   */
    JCE_COROUTINE_WAIT_UNTIL           /* poll predicate every Update   */
} JceCoroutineWaitKind;

struct JceCoroutineWait {
    JceCoroutineWaitKind kind;
    double               seconds;     /* WAIT_SECONDS only             */
    JceCoroutineUntilFn  until;       /* WAIT_UNTIL only — else NULL   */
    void                *until_user;
};

/* Start a coroutine.  Implicit initial wait is "next frame": `fn` will
 * not fire from inside this call — it runs on the next Update phase.
 * Returns JCE_COROUTINE_INVALID if the pool is exhausted or fn is NULL.
 * Auto-initialises the scheduler on first call. */
JCE_API JceCoroutineHandle JCE_CALL
jce_coroutine_start(JceCoroutineFn fn, void *user);

/* Cancel a running coroutine.  Safe to call from inside the coroutine's
 * own callback or on a stale / already-finished handle (no-op). */
JCE_API void JCE_CALL
jce_coroutine_cancel(JceCoroutineHandle h);

/* True iff the handle still refers to a live slot (same generation). */
JCE_API bool JCE_CALL
jce_coroutine_is_alive(JceCoroutineHandle h);

/* Number of currently-alive coroutines (for editor diagnostics). */
JCE_API uint32_t JCE_CALL
jce_coroutine_active_count(void);

/* PlayerLoop integration.  `init` is idempotent and is invoked lazily by
 * the first jce_coroutine_start; expose it for tests that want a
 * deterministic setup.  `shutdown` cancels every alive coroutine and
 * unregisters the phase hooks. */
JCE_API void JCE_CALL jce_coroutine_system_init(void);
JCE_API void JCE_CALL jce_coroutine_system_shutdown(void);

#ifndef NDEBUG
/* Self-test: spins up a handful of coroutines, drives the PlayerLoop
 * phases manually, and verifies cancellation / timing / predicate /
 * counter semantics.  Returns true on success.  Logs failures via
 * jce_log.  Does not run automatically — call from a debug entry. */
JCE_API bool JCE_CALL jce_coroutine_self_test(void);
#endif

/* ── Yield builders (zero-cost wrappers for readability) ─────────── */

static inline void jce_coroutine_yield_next_frame(JceCoroutineWait *w)
{
    w->kind       = JCE_COROUTINE_WAIT_NEXT_FRAME;
    w->seconds    = 0.0;
    w->until      = (JceCoroutineUntilFn)0;
    w->until_user = (void *)0;
}

static inline void jce_coroutine_yield_seconds(JceCoroutineWait *w, double s)
{
    w->kind       = JCE_COROUTINE_WAIT_SECONDS;
    w->seconds    = s;
    w->until      = (JceCoroutineUntilFn)0;
    w->until_user = (void *)0;
}

static inline void jce_coroutine_yield_fixed_update(JceCoroutineWait *w)
{
    w->kind       = JCE_COROUTINE_WAIT_FIXED_UPDATE;
    w->seconds    = 0.0;
    w->until      = (JceCoroutineUntilFn)0;
    w->until_user = (void *)0;
}

static inline void jce_coroutine_yield_end_of_frame(JceCoroutineWait *w)
{
    w->kind       = JCE_COROUTINE_WAIT_END_OF_FRAME;
    w->seconds    = 0.0;
    w->until      = (JceCoroutineUntilFn)0;
    w->until_user = (void *)0;
}

static inline void jce_coroutine_yield_until(JceCoroutineWait   *w,
                                             JceCoroutineUntilFn until,
                                             void               *user)
{
    w->kind       = JCE_COROUTINE_WAIT_UNTIL;
    w->seconds    = 0.0;
    w->until      = until;
    w->until_user = user;
}

JCE_EXTERN_C_END

#endif /* JCE_COROUTINE_H */
