/*
 * jce_coroutine.c  Deferred-callback coroutine scheduler (P3-B.4).
 *
 * Storage: a static pool of JCE_COROUTINE_MAX_ACTIVE slots.  Each slot
 * carries an 8-bit `alive` flag and a 32-bit generation counter; the
 * public handle packs `(generation << 32) | slot_index` so a stale
 * handle whose slot has been recycled compares unequal to the new
 * occupant.  No heap, no allocator dependency.
 *
 * Tick model: three PlayerLoop hooks fire each frame in this order:
 *   UPDATE        → dispatches NEXT_FRAME / SECONDS / UNTIL waits
 *   FIXED_UPDATE  → dispatches FIXED_UPDATE waits (0..N times per frame)
 *   END_OF_FRAME  → dispatches END_OF_FRAME waits
 *
 * `s_time_seconds` is the cooperative "Time.time" — it advances on the
 * UPDATE callback using the supplied dt, so WAIT_SECONDS deadlines stay
 * in lock step with the simulation regardless of wall-clock drift.
 *
 * Re-entrancy: dispatchers snapshot the active-slot count before
 * iterating, so a coroutine that starts another coroutine inside its
 * callback will see the new one on the NEXT applicable phase, never
 * inside the current dispatch — matches Unity semantics and prevents
 * unbounded recursion.
 */

#include <jce/runtime/jce_coroutine.h>

#include <jce/os/core/jce_log.h>
#include <jce/runtime/jce_player_loop.h>

#include <stddef.h>
#include <string.h>

#define LOG_TAG "coroutine"

typedef struct CoroSlot {
    JceCoroutineFn   fn;
    void            *user;
    JceCoroutineWait wait;
    double           wake_at;        /* WAIT_SECONDS deadline (s)        */
    uint32_t         generation;     /* bumped on free                   */
    bool             alive;
    bool             pending_first;  /* set on start, cleared after first
                                      * Update dispatch (mirrors Unity's
                                      * "first resume on next frame")    */
} CoroSlot;

static CoroSlot            s_slots[JCE_COROUTINE_MAX_ACTIVE];
static uint32_t            s_active_count;
static double              s_time_seconds;
static bool                s_initialised;
static JcePlayerLoopHandle s_h_update;
static JcePlayerLoopHandle s_h_fixed;
static JcePlayerLoopHandle s_h_eof;

/* ── Helpers ──────────────────────────────────────────────────────── */

static inline JceCoroutineHandle pack_handle(uint32_t slot, uint32_t gen)
{
    return ((JceCoroutineHandle)gen << 32) | (JceCoroutineHandle)slot;
}

static inline uint32_t handle_slot(JceCoroutineHandle h)
{
    return (uint32_t)(h & 0xFFFFFFFFu);
}

static inline uint32_t handle_gen(JceCoroutineHandle h)
{
    return (uint32_t)(h >> 32);
}

static CoroSlot *resolve(JceCoroutineHandle h)
{
    if (h == JCE_COROUTINE_INVALID) return NULL;
    const uint32_t idx = handle_slot(h);
    if (idx >= JCE_COROUTINE_MAX_ACTIVE) return NULL;
    CoroSlot *s = &s_slots[idx];
    if (!s->alive || s->generation != handle_gen(h)) return NULL;
    return s;
}

static void free_slot(CoroSlot *s)
{
    s->alive         = false;
    s->fn            = NULL;
    s->user          = NULL;
    s->pending_first = false;
    s->generation++;
    if (s_active_count > 0u) s_active_count--;
}

static void apply_next_wait(CoroSlot *s, const JceCoroutineWait *next)
{
    s->wait = *next;
    if (next->kind == JCE_COROUTINE_WAIT_SECONDS) {
        const double secs = (next->seconds > 0.0) ? next->seconds : 0.0;
        s->wake_at = s_time_seconds + secs;
    } else {
        s->wake_at = 0.0;
    }
}

/* Invoke the coroutine body once.  Frees the slot if the body returns
 * false; otherwise stores the new wait spec. */
static void invoke(CoroSlot *s)
{
    JceCoroutineWait next;
    memset(&next, 0, sizeof(next));
    const bool keep_going = s->fn(s->user, &next);
    /* The callback may have called jce_coroutine_cancel(self); honour
     * that by checking the alive flag again before mutating state. */
    if (!s->alive) return;
    if (!keep_going) {
        free_slot(s);
        return;
    }
    apply_next_wait(s, &next);
}

/* ── PlayerLoop phase hooks ───────────────────────────────────────── */

static void on_update(float dt, void *user)
{
    (void)user;
    s_time_seconds += (double)dt;

    /* Snapshot count: new coroutines started inside a body must wait
     * for the next Update. */
    const uint32_t cap = JCE_COROUTINE_MAX_ACTIVE;
    for (uint32_t i = 0u; i < cap; ++i) {
        CoroSlot *s = &s_slots[i];
        if (!s->alive) continue;

        /* Freshly-started coroutines resume here on their first frame. */
        if (s->pending_first) {
            s->pending_first = false;
            invoke(s);
            continue;
        }

        switch (s->wait.kind) {
        case JCE_COROUTINE_WAIT_NEXT_FRAME:
            invoke(s);
            break;
        case JCE_COROUTINE_WAIT_SECONDS:
            if (s_time_seconds >= s->wake_at) invoke(s);
            break;
        case JCE_COROUTINE_WAIT_UNTIL:
            if (s->wait.until && s->wait.until(s->wait.until_user))
                invoke(s);
            break;
        case JCE_COROUTINE_WAIT_FIXED_UPDATE:
        case JCE_COROUTINE_WAIT_END_OF_FRAME:
        default:
            break;
        }
    }
}

static void on_fixed_update(float dt, void *user)
{
    (void)dt;
    (void)user;
    const uint32_t cap = JCE_COROUTINE_MAX_ACTIVE;
    for (uint32_t i = 0u; i < cap; ++i) {
        CoroSlot *s = &s_slots[i];
        if (!s->alive) continue;
        if (s->pending_first) continue;
        if (s->wait.kind == JCE_COROUTINE_WAIT_FIXED_UPDATE) invoke(s);
    }
}

static void on_end_of_frame(float dt, void *user)
{
    (void)dt;
    (void)user;
    const uint32_t cap = JCE_COROUTINE_MAX_ACTIVE;
    for (uint32_t i = 0u; i < cap; ++i) {
        CoroSlot *s = &s_slots[i];
        if (!s->alive) continue;
        if (s->pending_first) continue;
        if (s->wait.kind == JCE_COROUTINE_WAIT_END_OF_FRAME) invoke(s);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void JCE_CALL jce_coroutine_system_init(void)
{
    if (s_initialised) return;
    s_active_count = 0u;
    s_time_seconds = 0.0;
    memset(s_slots, 0, sizeof(s_slots));

    /* Generation starts at 1, never 0.  The public handle is
     * (generation << 32) | slot and JCE_COROUTINE_INVALID is 0, so a slot 0
     * with generation 0 packs to 0 -- the failure sentinel.  That made the
     * FIRST coroutine started after init indistinguishable from a failed
     * start, and worse: resolve() short-circuits on the sentinel, so
     * jce_coroutine_is_alive() reported it dead while it ran and
     * jce_coroutine_cancel() silently did nothing, leaving it running for
     * the life of the process.
     *
     * Found 2026-08-31 by running jce_coroutine_self_test() for the first
     * time -- it had existed, complete and correct, with no caller anywhere
     * in the tree.  It catches this on its very first assertion. */
    for (uint32_t i = 0u; i < JCE_COROUTINE_MAX_ACTIVE; ++i)
        s_slots[i].generation = 1u;

    /* Run after the editor's per-frame work but before user middleware
     * — priority 1000 keeps us well out of the way of P3-B.1 / P3-B.2
     * subsystems that register near priority 0. */
    s_h_update = jce_player_loop_register(
        JCE_PHASE_UPDATE, 1000, on_update, NULL);
    s_h_fixed  = jce_player_loop_register(
        JCE_PHASE_FIXED_UPDATE, 1000, on_fixed_update, NULL);
    s_h_eof    = jce_player_loop_register(
        JCE_PHASE_END_OF_FRAME, 1000, on_end_of_frame, NULL);

    jce_player_loop_set_debug_name(s_h_update.id, "coroutine.update");
    jce_player_loop_set_debug_name(s_h_fixed.id,  "coroutine.fixed");
    jce_player_loop_set_debug_name(s_h_eof.id,    "coroutine.eof");

    s_initialised = true;
    LOG_INFO(LOG_TAG, "coroutine scheduler initialised (pool=%u)",
             (unsigned)JCE_COROUTINE_MAX_ACTIVE);
}

void JCE_CALL jce_coroutine_system_shutdown(void)
{
    if (!s_initialised) return;
    for (uint32_t i = 0u; i < JCE_COROUTINE_MAX_ACTIVE; ++i) {
        if (s_slots[i].alive) free_slot(&s_slots[i]);
    }
    jce_player_loop_unregister(s_h_update);
    jce_player_loop_unregister(s_h_fixed);
    jce_player_loop_unregister(s_h_eof);
    s_h_update.id = 0u;
    s_h_fixed.id  = 0u;
    s_h_eof.id    = 0u;
    s_initialised = false;
    s_active_count = 0u;
    s_time_seconds = 0.0;
}

JceCoroutineHandle JCE_CALL
jce_coroutine_start(JceCoroutineFn fn, void *user)
{
    if (!fn) {
        LOG_WARN(LOG_TAG, "start rejected: NULL fn");
        return JCE_COROUTINE_INVALID;
    }
    if (!s_initialised) jce_coroutine_system_init();

    for (uint32_t i = 0u; i < JCE_COROUTINE_MAX_ACTIVE; ++i) {
        CoroSlot *s = &s_slots[i];
        if (s->alive) continue;
        s->fn            = fn;
        s->user          = user;
        s->wait.kind     = JCE_COROUTINE_WAIT_NEXT_FRAME;
        s->wait.seconds  = 0.0;
        s->wait.until    = NULL;
        s->wait.until_user = NULL;
        s->wake_at       = 0.0;
        s->alive         = true;
        s->pending_first = true;
        s_active_count++;
        return pack_handle(i, s->generation);
    }
    LOG_ERROR(LOG_TAG, "pool exhausted (max=%u)",
              (unsigned)JCE_COROUTINE_MAX_ACTIVE);
    return JCE_COROUTINE_INVALID;
}

void JCE_CALL jce_coroutine_cancel(JceCoroutineHandle h)
{
    CoroSlot *s = resolve(h);
    if (s) free_slot(s);
}

bool JCE_CALL jce_coroutine_is_alive(JceCoroutineHandle h)
{
    return resolve(h) != NULL;
}

uint32_t JCE_CALL jce_coroutine_active_count(void)
{
    return s_active_count;
}

/* ── Self-test ────────────────────────────────────────────────────── */

#ifdef JCE_SELF_TESTS

typedef struct StA { int count; } StA;
static bool body_next_frame(void *user, JceCoroutineWait *next)
{
    StA *s = (StA *)user;
    s->count++;
    if (s->count >= 3) return false;
    jce_coroutine_yield_next_frame(next);
    return true;
}

typedef struct StB { int phase; int fired; } StB;
static bool body_seconds(void *user, JceCoroutineWait *next)
{
    StB *s = (StB *)user;
    s->fired++;
    if (s->phase == 0) {
        s->phase = 1;
        jce_coroutine_yield_seconds(next, 0.05);
        return true;
    }
    return false;
}

typedef struct StC { int tick_now; int wake_on; int fired; } StC;
static bool until_pred(void *user)
{
    StC *s = (StC *)user;
    return s->tick_now >= s->wake_on;
}
static bool body_until(void *user, JceCoroutineWait *next)
{
    StC *s = (StC *)user;
    s->fired++;
    if (s->fired == 1) {
        jce_coroutine_yield_until(next, until_pred, s);
        return true;
    }
    return false;
}

typedef struct StD { int fired; } StD;
static bool body_cancel(void *user, JceCoroutineWait *next)
{
    StD *s = (StD *)user;
    s->fired++;
    jce_coroutine_yield_next_frame(next);
    return true;
}

bool JCE_CALL jce_coroutine_self_test(void)
{
    /* Save/restore so we don't trample a live scheduler. */
    const bool was_init = s_initialised;
    if (was_init) jce_coroutine_system_shutdown();
    jce_coroutine_system_init();

    bool ok = true;

    /* 1) yield_next_frame 3 times then exit. */
    StA a = { 0 };
    JceCoroutineHandle ha = jce_coroutine_start(body_next_frame, &a);
    if (ha == JCE_COROUTINE_INVALID) {
        /* Was a silent `goto done`.  A bail-out that logs nothing is
         * why this self-test's first ever run (2026-08-31) reported
         * only "FAIL" with no clue which of four starts refused. */
        LOG_ERROR(LOG_TAG, "self_test next_frame: jce_coroutine_start returned "
                  "INVALID");
        ok = false; goto done;
    }
    /* Frame 1 → pending_first fires (count=1, yields next_frame).
     * Frame 2 → resume (count=2, yields).  Frame 3 → resume (count=3,
     * returns false; slot freed). */
    for (int i = 0; i < 3; ++i) jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    if (a.count != 3) {
        LOG_ERROR(LOG_TAG, "self_test next_frame: count=%d (want 3)", a.count);
        ok = false;
    }
    if (jce_coroutine_is_alive(ha)) {
        LOG_ERROR(LOG_TAG, "self_test next_frame: still alive");
        ok = false;
    }

    /* 2) yield_seconds(0.05). */
    StB b = { 0, 0 };
    JceCoroutineHandle hb = jce_coroutine_start(body_seconds, &b);
    if (hb == JCE_COROUTINE_INVALID) {
        /* Was a silent `goto done`.  A bail-out that logs nothing is
         * why this self-test's first ever run (2026-08-31) reported
         * only "FAIL" with no clue which of four starts refused. */
        LOG_ERROR(LOG_TAG, "self_test seconds: jce_coroutine_start returned "
                  "INVALID");
        ok = false; goto done;
    }
    /* First frame: pending_first dispatches → phase=1, wake at +0.05.
     * Advance 0.03 → no wake.  Advance 0.03 more → wake. */
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f);
    if (b.phase != 1 || b.fired != 1) {
        LOG_ERROR(LOG_TAG, "self_test seconds: initial dispatch failed");
        ok = false;
    }
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.03f);
    if (b.fired != 1) {
        LOG_ERROR(LOG_TAG, "self_test seconds: early wake fired=%d", b.fired);
        ok = false;
    }
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.03f);
    if (b.fired != 2) {
        LOG_ERROR(LOG_TAG, "self_test seconds: late wake fired=%d", b.fired);
        ok = false;
    }

    /* 3) yield_until predicate, flips on tick 3. */
    StC c = { 0, 3, 0 };
    JceCoroutineHandle hc = jce_coroutine_start(body_until, &c);
    if (hc == JCE_COROUTINE_INVALID) {
        /* Was a silent `goto done`.  A bail-out that logs nothing is
         * why this self-test's first ever run (2026-08-31) reported
         * only "FAIL" with no clue which of four starts refused. */
        LOG_ERROR(LOG_TAG, "self_test until: jce_coroutine_start returned "
                  "INVALID");
        ok = false; goto done;
    }
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f); /* pending_first; fired=1, yields_until */
    if (c.fired != 1) { LOG_ERROR(LOG_TAG, "self_test until: first fire"); ok = false; }
    c.tick_now = 1; jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f);
    c.tick_now = 2; jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f);
    if (c.fired != 1) {
        LOG_ERROR(LOG_TAG, "self_test until: woke early fired=%d", c.fired);
        ok = false;
    }
    c.tick_now = 3; jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f);
    if (c.fired != 2) {
        LOG_ERROR(LOG_TAG, "self_test until: didn't wake fired=%d", c.fired);
        ok = false;
    }

    /* 4) Cancel mid-flight. */
    StD d = { 0 };
    JceCoroutineHandle hd = jce_coroutine_start(body_cancel, &d);
    if (hd == JCE_COROUTINE_INVALID) {
        /* Was a silent `goto done`.  A bail-out that logs nothing is
         * why this self-test's first ever run (2026-08-31) reported
         * only "FAIL" with no clue which of four starts refused. */
        LOG_ERROR(LOG_TAG, "self_test cancel: jce_coroutine_start returned "
                  "INVALID");
        ok = false; goto done;
    }
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f); /* fired=1 */
    if (d.fired != 1) { LOG_ERROR(LOG_TAG, "self_test cancel: initial"); ok = false; }
    jce_coroutine_cancel(hd);
    if (jce_coroutine_is_alive(hd)) {
        LOG_ERROR(LOG_TAG, "self_test cancel: still alive");
        ok = false;
    }
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.0f);
    if (d.fired != 1) {
        LOG_ERROR(LOG_TAG, "self_test cancel: fired after cancel d=%d", d.fired);
        ok = false;
    }

done:
    jce_coroutine_system_shutdown();
    if (was_init) jce_coroutine_system_init();

    if (ok)
        LOG_INFO(LOG_TAG, "self_test: PASS");
    else
        LOG_ERROR(LOG_TAG, "self_test: FAIL");
    return ok;
}

#endif /* JCE_SELF_TESTS */
