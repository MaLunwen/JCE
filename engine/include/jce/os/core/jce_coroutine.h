/*
 * jce_coroutine.h  Frame-driven coroutines (Unity StartCoroutine equivalent).
 *
 * A "coroutine" here is a single-threaded state machine that runs
 * across multiple frames, suspending on `yield` directives until a
 * condition (time elapsed, predicate true, next-frame, or end-of-
 * frame) becomes satisfied.  Lighter than OS threads, perfect for
 * gameplay sequencing: timers, scripted scenes, networked confirmations.
 *
 * Coroutines are stepped from the engine main loop via
 * `jce_coroutines_tick(dt)`.  Each call advances every live
 * coroutine by checking its yield instruction; if satisfied, the
 * coroutine's `body_fn` is invoked again with its `state` argument.
 *
 * Programming model:
 *   The body function returns a JceCoroutineYield struct describing
 *   what to wait for next.  Returning `JCE_YIELD_DONE` ends the
 *   coroutine (it's reaped on the next tick).
 *
 *   The body_fn is responsible for tracking its own "step number" via
 *   the user-provided `state` pointer — typical pattern is a small
 *   struct with an `int step` field that the body switches on.
 *
 * Example:
 *
 *     typedef struct { int step; int counter; } MyState;
 *
 *     JceCoroutineYield my_body(void *ud) {
 *         MyState *s = (MyState*)ud;
 *         switch (s->step) {
 *             case 0: printf("start\n");      s->step = 1; return jce_yield_seconds(1.0f);
 *             case 1: printf("after 1s\n");   s->step = 2; return jce_yield_seconds(0.5f);
 *             case 2: printf("done\n");       return jce_yield_done();
 *         }
 *         return jce_yield_done();
 *     }
 *
 *     MyState ms = { 0 };
 *     jce_coroutine_start(my_body, &ms);
 *
 *     // each frame:
 *     jce_coroutines_tick(dt);
 *
 * Thread-safety: single-threaded, main-thread only.
 *
 * Layer: os/core (Layer 1) — public.
 */

#ifndef JCE_COROUTINE_H
#define JCE_COROUTINE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_YIELD_KIND_DONE = 0,        /* coroutine finished */
    JCE_YIELD_KIND_NEXT_FRAME = 1,  /* resume next tick */
    JCE_YIELD_KIND_SECONDS = 2,     /* resume after `seconds` of unscaled time */
    JCE_YIELD_KIND_PREDICATE = 3    /* resume when predicate(ud) returns true */
} JceCoroutineYieldKind;

typedef bool (*JceCoroutinePredicate)(void *ud);

typedef struct {
    JceCoroutineYieldKind  kind;
    float                  seconds;     /* SECONDS only */
    JceCoroutinePredicate  predicate;   /* PREDICATE only */
    void                  *predicate_ud;
} JceCoroutineYield;

/* Coroutine body — called repeatedly until it returns YIELD_DONE.
 * `user_data` is the pointer passed to jce_coroutine_start. */
typedef JceCoroutineYield (*JceCoroutineBodyFn)(void *user_data);

/* Yield helpers (constructors).  Use these in the body's `return`. */
JCE_API JceCoroutineYield jce_yield_done(void);
JCE_API JceCoroutineYield jce_yield_next_frame(void);
JCE_API JceCoroutineYield jce_yield_seconds(float seconds);
JCE_API JceCoroutineYield jce_yield_until(JceCoroutinePredicate fn, void *ud);

typedef uint32_t JceCoroutineId;
#define JCE_COROUTINE_INVALID 0u

/* Start a coroutine.  Returns its id (>0) or JCE_COROUTINE_INVALID
 * on capacity overflow.  The body_fn fires immediately and its first
 * yield directive is enforced from the next tick. */
JCE_API JceCoroutineId jce_coroutine_start(JceCoroutineBodyFn body_fn,
                                            void              *user_data);

/* Stop a coroutine before it finishes.  No-op if id is unknown. */
JCE_API void jce_coroutine_stop(JceCoroutineId id);

/* Tick all live coroutines forward by `dt` seconds.  Drives the
 * yield-satisfaction logic: coroutines whose yield condition is met
 * have their body_fn called again. */
JCE_API void jce_coroutines_tick(float dt);

/* Number of currently-alive coroutines (not finished, not stopped). */
JCE_API uint32_t jce_coroutines_alive_count(void);

JCE_EXTERN_C_END

#endif /* JCE_COROUTINE_H */
