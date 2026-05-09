/*
 * jce_coroutine.c  Frame-driven coroutine state machine.
 *
 * Fixed-size pool (256 slots) — sufficient for typical gameplay
 * scripting workloads.  Each slot tracks its body_fn / user_data /
 * current yield directive / accumulated wait time.  Each tick:
 *   1. Apply dt to active timers.
 *   2. For each slot whose yield condition is satisfied, call body_fn.
 *   3. Update slot with body_fn's returned yield.  YIELD_DONE frees it.
 */

#include <jce/os/core/jce_coroutine.h>

#include <string.h>

#define COROUTINE_MAX 256

typedef struct {
    bool                  alive;
    JceCoroutineBodyFn    body_fn;
    void                 *user_data;
    JceCoroutineYield     yield;
    float                 wait_remaining;
    bool                  first_step_pending;
} Slot;

static Slot     s_slots[COROUTINE_MAX];
static uint32_t s_id_counter = 1u;
static uint32_t s_id_for_slot[COROUTINE_MAX];

/* ── Yield constructors ──────────────────────────────────────────── */

JceCoroutineYield jce_yield_done(void)
{
    JceCoroutineYield y = { JCE_YIELD_KIND_DONE, 0.0f, NULL, NULL };
    return y;
}

JceCoroutineYield jce_yield_next_frame(void)
{
    JceCoroutineYield y = { JCE_YIELD_KIND_NEXT_FRAME, 0.0f, NULL, NULL };
    return y;
}

JceCoroutineYield jce_yield_seconds(float seconds)
{
    JceCoroutineYield y = { JCE_YIELD_KIND_SECONDS,
                            seconds < 0.0f ? 0.0f : seconds,
                            NULL, NULL };
    return y;
}

JceCoroutineYield jce_yield_until(JceCoroutinePredicate fn, void *ud)
{
    JceCoroutineYield y = { JCE_YIELD_KIND_PREDICATE, 0.0f, fn, ud };
    return y;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static int find_free_slot(void)
{
    for (int i = 0; i < COROUTINE_MAX; ++i)
        if (!s_slots[i].alive) return i;
    return -1;
}

static int find_slot_by_id(JceCoroutineId id)
{
    if (id == JCE_COROUTINE_INVALID) return -1;
    for (int i = 0; i < COROUTINE_MAX; ++i)
        if (s_slots[i].alive && s_id_for_slot[i] == id) return i;
    return -1;
}

JceCoroutineId jce_coroutine_start(JceCoroutineBodyFn body_fn, void *user_data)
{
    if (!body_fn) return JCE_COROUTINE_INVALID;
    int slot = find_free_slot();
    if (slot < 0) return JCE_COROUTINE_INVALID;

    Slot *s = &s_slots[slot];
    s->alive          = true;
    s->body_fn        = body_fn;
    s->user_data      = user_data;
    s->wait_remaining = 0.0f;
    /* Run the body once immediately to get the first yield directive.
     * This matches Unity's behaviour: StartCoroutine runs up to the
     * first yield synchronously. */
    s->yield = body_fn(user_data);
    if (s->yield.kind == JCE_YIELD_KIND_DONE) {
        s->alive = false;
        return JCE_COROUTINE_INVALID;
    }
    if (s->yield.kind == JCE_YIELD_KIND_SECONDS)
        s->wait_remaining = s->yield.seconds;
    s->first_step_pending = false;

    JceCoroutineId id = s_id_counter++;
    if (s_id_counter == JCE_COROUTINE_INVALID) s_id_counter = 1u;
    s_id_for_slot[slot] = id;
    return id;
}

void jce_coroutine_stop(JceCoroutineId id)
{
    int slot = find_slot_by_id(id);
    if (slot < 0) return;
    s_slots[slot].alive = false;
    s_id_for_slot[slot] = 0;
}

void jce_coroutines_tick(float dt)
{
    if (dt < 0.0f) dt = 0.0f;
    for (int i = 0; i < COROUTINE_MAX; ++i) {
        Slot *s = &s_slots[i];
        if (!s->alive) continue;

        bool resume = false;
        switch (s->yield.kind) {
            case JCE_YIELD_KIND_NEXT_FRAME:
                resume = true; /* always step on next tick */
                break;
            case JCE_YIELD_KIND_SECONDS:
                s->wait_remaining -= dt;
                if (s->wait_remaining <= 0.0f) resume = true;
                break;
            case JCE_YIELD_KIND_PREDICATE:
                if (s->yield.predicate &&
                    s->yield.predicate(s->yield.predicate_ud))
                    resume = true;
                break;
            case JCE_YIELD_KIND_DONE:
            default:
                /* Shouldn't happen — DONE slots are reaped on yield. */
                s->alive = false;
                continue;
        }

        if (!resume) continue;

        s->yield = s->body_fn(s->user_data);
        if (s->yield.kind == JCE_YIELD_KIND_DONE) {
            s->alive = false;
            s_id_for_slot[i] = 0;
            continue;
        }
        if (s->yield.kind == JCE_YIELD_KIND_SECONDS)
            s->wait_remaining = s->yield.seconds;
    }
}

uint32_t jce_coroutines_alive_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < COROUTINE_MAX; ++i) if (s_slots[i].alive) n++;
    return n;
}
