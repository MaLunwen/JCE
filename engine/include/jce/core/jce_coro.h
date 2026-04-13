/*
 * jce_coro.h  Lightweight stackless coroutines for C.
 *
 * Provides simple coroutine macros for game programming:
 * - Animation sequences
 * - UI transitions
 * - Game event chains
 * - Scripted behaviors
 *
 * Usage:
 *   typedef struct {
 *       JCE_CORO_STATE;
 *       int counter;
 *       float timer;
 *   } MyCoroutine;
 *
 *   JceCoro my_coro_update(MyCoroutine *ctx, float dt) {
 *       JCE_CORO_BEGIN(ctx);
 *
 *       // Wait 1 second
 *       ctx->timer = 0;
 *       while (ctx->timer < 1.0f) {
 *           ctx->timer += dt;
 *           JCE_CORO_YIELD();
 *       }
 *
 *       // Do something 5 times
 *       for (ctx->counter = 0; ctx->counter < 5; ctx->counter++) {
 *           do_something();
 *           JCE_CORO_YIELD();
 *       }
 *
 *       JCE_CORO_END();
 *   }
 *
 * Note: Local variables lose their values across yields.
 *       Store state in the context struct instead.
 *
 * Layer: Utilities (Layer 0).
 */

#ifndef JCE_CORO_H
#define JCE_CORO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Core API                                                            */
/* ================================================================== */

/* Coroutine status. */
typedef enum {
    JCE_CORO_RUNNING  = 0,  /* Still executing, will continue next call */
    JCE_CORO_FINISHED = 1,  /* Completed execution */
    JCE_CORO_DEAD     = 2,  /* Error or invalid state */
} JceCoro;

/* Put this at the start of your coroutine context struct. */
#define JCE_CORO_STATE  int _coro_line

/* Initialize a coroutine context before first use. */
#define JCE_CORO_INIT(ctx)  do { (ctx)->_coro_line = 0; } while(0)

/* Reset a coroutine to its initial state. */
#define JCE_CORO_RESET(ctx) JCE_CORO_INIT(ctx)

/* Check if coroutine is finished. */
#define JCE_CORO_IS_DONE(ctx) ((ctx)->_coro_line < 0)

/* ================================================================== */
/* Coroutine body macros                                               */
/* ================================================================== */

/* Begin a coroutine function body. */
#define JCE_CORO_BEGIN(ctx) \
    switch ((ctx)->_coro_line) { case 0:;

/* Yield execution — return and resume here next call. */
#define JCE_CORO_YIELD() \
    do { \
        (ctx)->_coro_line = __LINE__; \
        return JCE_CORO_RUNNING; \
        case __LINE__:; \
    } while(0)

/* Yield with a specific status. */
#define JCE_CORO_YIELD_STATUS(status) \
    do { \
        (ctx)->_coro_line = __LINE__; \
        return (status); \
        case __LINE__:; \
    } while(0)

/* End a coroutine function body. */
#define JCE_CORO_END() \
    } \
    (ctx)->_coro_line = -1; \
    return JCE_CORO_FINISHED

/* Exit early (marks coroutine as finished). */
#define JCE_CORO_EXIT() \
    do { \
        (ctx)->_coro_line = -1; \
        return JCE_CORO_FINISHED; \
    } while(0)

/* ================================================================== */
/* Convenience macros for common patterns                              */
/* ================================================================== */

/* Wait until condition is true. */
#define JCE_CORO_WAIT_UNTIL(cond) \
    do { \
        while (!(cond)) { JCE_CORO_YIELD(); } \
    } while(0)

/* Wait while condition is true. */
#define JCE_CORO_WAIT_WHILE(cond) \
    JCE_CORO_WAIT_UNTIL(!(cond))

/* Wait for N frames. */
#define JCE_CORO_WAIT_FRAMES(ctx, n, frame_var) \
    do { \
        for ((ctx)->frame_var = 0; (ctx)->frame_var < (n); (ctx)->frame_var++) { \
            JCE_CORO_YIELD(); \
        } \
    } while(0)

/* ================================================================== */
/* Timer-based coroutine support                                       */
/* ================================================================== */

/* Wait for a specific duration (requires timer field in context).
 * Call with: JCE_CORO_WAIT_SECONDS(ctx, 1.5f, timer_field, dt);
 * where dt is the frame delta time.
 */
#define JCE_CORO_WAIT_SECONDS(ctx, duration, timer_var, delta_time) \
    do { \
        (ctx)->timer_var = 0.0f; \
        while ((ctx)->timer_var < (duration)) { \
            (ctx)->timer_var += (delta_time); \
            JCE_CORO_YIELD(); \
        } \
    } while(0)

/* ================================================================== */
/* Sub-coroutine support                                               */
/* ================================================================== */

/* Call another coroutine and wait for it to finish.
 * Usage: JCE_CORO_CALL(ctx, other_coro_func(&sub_ctx, args...));
 */
#define JCE_CORO_CALL(ctx, call_expr) \
    do { \
        (ctx)->_coro_line = __LINE__; \
        case __LINE__: \
            if ((call_expr) == JCE_CORO_RUNNING) \
                return JCE_CORO_RUNNING; \
    } while(0)

/* ================================================================== */
/* Coroutine scheduler (optional)                                      */
/* ================================================================== */

/* Maximum coroutines in a scheduler. */
#define JCE_CORO_MAX_SCHEDULED 64

/* Generic coroutine function type for scheduler. */
typedef JceCoro (*JceCoroFn)(void *ctx, float dt);

/* Coroutine scheduler for managing multiple coroutines. */
typedef struct {
    struct {
        JceCoroFn fn;
        void     *ctx;
        bool      active;
    } slots[JCE_CORO_MAX_SCHEDULED];
    int count;
} JceCoroScheduler;

/* Initialize a scheduler. */
static inline void jce_coro_scheduler_init(JceCoroScheduler *sched)
{
    if (!sched) return;
    for (int i = 0; i < JCE_CORO_MAX_SCHEDULED; i++) {
        sched->slots[i].fn = NULL;
        sched->slots[i].ctx = NULL;
        sched->slots[i].active = false;
    }
    sched->count = 0;
}

/* Start a coroutine in the scheduler. Returns slot index or -1 on failure. */
static inline int jce_coro_scheduler_start(JceCoroScheduler *sched,
                                            JceCoroFn fn, void *ctx)
{
    if (!sched || !fn) return -1;
    for (int i = 0; i < JCE_CORO_MAX_SCHEDULED; i++) {
        if (!sched->slots[i].active) {
            sched->slots[i].fn = fn;
            sched->slots[i].ctx = ctx;
            sched->slots[i].active = true;
            sched->count++;
            return i;
        }
    }
    return -1;
}

/* Stop a coroutine by slot index. */
static inline void jce_coro_scheduler_stop(JceCoroScheduler *sched, int slot)
{
    if (!sched || slot < 0 || slot >= JCE_CORO_MAX_SCHEDULED) return;
    if (sched->slots[slot].active) {
        sched->slots[slot].active = false;
        sched->slots[slot].fn = NULL;
        sched->slots[slot].ctx = NULL;
        sched->count--;
    }
}

/* Update all active coroutines. Removes finished ones. */
static inline void jce_coro_scheduler_update(JceCoroScheduler *sched, float dt)
{
    if (!sched) return;
    for (int i = 0; i < JCE_CORO_MAX_SCHEDULED; i++) {
        if (sched->slots[i].active) {
            JceCoro status = sched->slots[i].fn(sched->slots[i].ctx, dt);
            if (status != JCE_CORO_RUNNING) {
                jce_coro_scheduler_stop(sched, i);
            }
        }
    }
}

/* Get number of active coroutines. */
static inline int jce_coro_scheduler_count(const JceCoroScheduler *sched)
{
    return sched ? sched->count : 0;
}

/* Stop all coroutines. */
static inline void jce_coro_scheduler_stop_all(JceCoroScheduler *sched)
{
    if (!sched) return;
    for (int i = 0; i < JCE_CORO_MAX_SCHEDULED; i++) {
        sched->slots[i].active = false;
        sched->slots[i].fn = NULL;
        sched->slots[i].ctx = NULL;
    }
    sched->count = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_CORO_H */
