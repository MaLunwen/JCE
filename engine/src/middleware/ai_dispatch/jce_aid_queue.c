/* jce_aid_queue.c -- async acquisition: request slots, single worker
 * thread, main-thread pump (spec D.2/J).
 *
 * Threading contract:
 *   - public API is main-thread only;
 *   - ONE worker thread runs the chain (network I/O blocks there, the
 *     main loop never waits);
 *   - shutdown broadcasts WHILE HOLDING the lock and re-checks the
 *     predicate inside the wait loop (lost-wakeup discipline);
 *   - record_id/tick stamping happens at request time on the main
 *     thread, so the worker never touches shared counters.
 *
 * Budgets (spec J): <=8 queued acquisitions (overflow falls straight to
 * a synchronous T3 result -- never waits), pump default 2 results and
 * 0.5 ms per frame. */
#include "jce_aid_transport.h"

#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>

#define LOG_TAG "ai_dispatch"

#define AID_SLOT_COUNT   16u /* result slots (handles alive at once) */
#define AID_QUEUE_DEPTH  8u  /* max acquisitions queued/running */
#define AID_MAX_KV       16u
#define AID_PUMP_DEFAULT 2u
#define AID_PUMP_NS      500000ull /* 0.5 ms */

typedef enum SlotState {
    SLOT_FREE = 0,
    SLOT_QUEUED,   /* waiting for the worker */
    SLOT_RUNNING,  /* worker acquiring */
    SLOT_DONE,     /* record present, waiting for pump */
    SLOT_READY     /* pumped; take_record may consume */
} SlotState;

typedef struct Slot {
    SlotState      state;
    uint16_t       gen;        /* handle generation (ABA guard) */
    uint64_t       seq;        /* FIFO order for the worker */
    JceAidSchemaId schema_id;
    JceAidStamp    stamp;
    uint64_t       world_gen;  /* generation token at request time */
    char*          kv_keys[AID_MAX_KV];
    char*          kv_vals[AID_MAX_KV];
    uint32_t       kv_count;
    JceAidRecord   record;
} Slot;

static struct {
    int         alive;      /* worker running */
    int         quit;
    JceMutex*   mutex;
    JceCondVar* cond;
    JceThread*  worker;
    Slot        slots[AID_SLOT_COUNT];
    uint64_t    next_seq;
    uint32_t    queued;     /* QUEUED + RUNNING count */
} g_q;

/* ---- cross-thread stat helpers (defined here; declared internal) ------ */

void jce_aid_stat_inc(uint64_t* counter)
{
    JceAidState* st = jce_aid_state();
    if (st && st->stats_mutex) {
        jce_mutex_lock((JceMutex*)st->stats_mutex);
        (*counter)++;
        jce_mutex_unlock((JceMutex*)st->stats_mutex);
    } else {
        (*counter)++;
    }
}

uint64_t jce_aid_stat_get(const uint64_t* counter)
{
    JceAidState* st = jce_aid_state();
    uint64_t     v;
    if (st && st->stats_mutex) {
        jce_mutex_lock((JceMutex*)st->stats_mutex);
        v = *counter;
        jce_mutex_unlock((JceMutex*)st->stats_mutex);
    } else {
        v = *counter;
    }
    return v;
}

/* ---- slot helpers (mutex held) ----------------------------------------- */

static void slot_free_kv(Slot* s)
{
    uint32_t i;
    for (i = 0; i < s->kv_count; ++i) {
        if (s->kv_keys[i]) jce_aid_free(s->kv_keys[i]);
        if (s->kv_vals[i]) jce_aid_free(s->kv_vals[i]);
        s->kv_keys[i] = NULL;
        s->kv_vals[i] = NULL;
    }
    s->kv_count = 0;
}

static void slot_release(Slot* s)
{
    slot_free_kv(s);
    s->state = SLOT_FREE;
    s->gen++;               /* invalidate outstanding handles */
    if (s->gen == 0) s->gen = 1;
}

static JceAidHandle slot_handle(const Slot* s, uint32_t idx)
{
    return ((uint32_t)s->gen << 8) | (idx + 1u);
}

static Slot* slot_resolve(JceAidHandle h, uint32_t* out_idx)
{
    uint32_t idx = (h & 0xFFu);
    uint16_t gen = (uint16_t)(h >> 8);
    if (idx == 0 || idx > AID_SLOT_COUNT) return NULL;
    idx -= 1;
    if (g_q.slots[idx].gen != gen) return NULL;
    if (out_idx) *out_idx = idx;
    return &g_q.slots[idx];
}

/* ---- worker ------------------------------------------------------------- */

static void worker_main(void* arg)
{
    (void)arg;
    jce_mutex_lock(g_q.mutex);
    for (;;) {
        Slot*    pick = NULL;
        uint32_t i;
        uint64_t best = ~0ull;

        if (g_q.quit) break;
        for (i = 0; i < AID_SLOT_COUNT; ++i) {
            Slot* s = &g_q.slots[i];
            if (s->state == SLOT_QUEUED && s->seq < best) {
                best = s->seq;
                pick = s;
            }
        }
        if (!pick) {
            jce_cond_wait(g_q.cond, g_q.mutex); /* predicate re-checked */
            continue;
        }

        pick->state = SLOT_RUNNING;
        {
            /* copy what the chain needs, then run it UNLOCKED */
            JceAidSchemaId  sid   = pick->schema_id;
            JceAidStamp     stamp = pick->stamp;
            JceAidContextKV kv[AID_MAX_KV];
            uint32_t        kv_count = pick->kv_count;
            JceAidRecord    rec;
            JceAidResult    r;
            for (i = 0; i < kv_count; ++i) {
                kv[i].key   = pick->kv_keys[i];
                kv[i].value = pick->kv_vals[i];
            }
            jce_mutex_unlock(g_q.mutex);

            {
                const JceAidSchema* s = jce_aid_schema_get(sid);
                r = s ? jce_aid_chain_acquire(s, kv, kv_count, &stamp, &rec)
                      : JCE_AID_ERR_NOT_FOUND;
            }

            jce_mutex_lock(g_q.mutex);
            if (pick->state == SLOT_RUNNING) { /* not torn down meanwhile */
                if (r == JCE_AID_OK) {
                    pick->record = rec;
                    pick->state  = SLOT_DONE;
                } else {
                    /* cannot happen with a registered schema (T3 is
                     * terminal); treat as discard */
                    LOG_WARN(LOG_TAG, "acquisition failed (%d) -- discarded",
                             (int)r);
                    g_q.queued--;
                    slot_release(pick);
                    continue;
                }
            }
            g_q.queued--;
        }
    }
    jce_mutex_unlock(g_q.mutex);
}

static int queue_ensure(void)
{
    if (g_q.alive) return 1;
    memset(&g_q, 0, sizeof(g_q));
    {
        uint32_t i;
        for (i = 0; i < AID_SLOT_COUNT; ++i) g_q.slots[i].gen = 1;
    }
    g_q.mutex = jce_mutex_create();
    g_q.cond  = jce_cond_create();
    if (!g_q.mutex || !g_q.cond) return 0;
    g_q.worker = jce_thread_create(worker_main, NULL, "jce_aid_worker");
    if (!g_q.worker) return 0;
    g_q.alive = 1;
    return 1;
}

void jce_aid_queue_shutdown(void)
{
    uint32_t i;
    if (!g_q.alive) return;
    jce_mutex_lock(g_q.mutex);
    g_q.quit = 1;
    jce_cond_broadcast(g_q.cond); /* broadcast WHILE holding the lock */
    jce_mutex_unlock(g_q.mutex);
    jce_thread_join(g_q.worker);
    for (i = 0; i < AID_SLOT_COUNT; ++i) slot_free_kv(&g_q.slots[i]);
    jce_cond_destroy(g_q.cond);
    jce_mutex_destroy(g_q.mutex);
    memset(&g_q, 0, sizeof(g_q));
}

/* ---- public API ---------------------------------------------------------- */

JceAidHandle JCE_CALL
jce_aid_request(JceAidSchemaId id, const JceAidContextKV* kv, uint32_t kv_count)
{
    JceAidState* st = jce_aid_state();
    Slot*        slot = NULL;
    uint32_t     idx = 0, i;
    uint64_t     tick;
    JceAidHandle h;

    if (!st) return 0;
    JCE_AID_ASSERT(!st->replay_active);
    if (st->replay_active) return 0;         /* acquisition ban (spec H) */
    if (!jce_aid_schema_get(id)) return 0;
    if (kv_count > AID_MAX_KV) kv_count = AID_MAX_KV;
    if (!queue_ensure()) return 0;

    /* main-thread stamping (same counter as generate_local) */
    tick = jce_fixed_clock_default()->tick_count;
    if (st->record_counter_tick != tick) {
        st->record_counter_tick = tick;
        st->record_counter      = 0;
    }

    jce_mutex_lock(g_q.mutex);
    for (i = 0; i < AID_SLOT_COUNT; ++i)
        if (g_q.slots[i].state == SLOT_FREE) { slot = &g_q.slots[i]; idx = i; break; }
    if (!slot) {
        jce_mutex_unlock(g_q.mutex);
        return 0; /* every result slot holds an untaken record */
    }

    slot->schema_id       = id;
    slot->stamp.tick      = tick;
    slot->stamp.record_id = (tick << 16) | (uint64_t)st->record_counter++;
    slot->world_gen       = st->generation;
    slot->seq             = g_q.next_seq++;
    slot->kv_count        = 0;
    for (i = 0; i < kv_count; ++i) {
        if (!kv || !kv[i].key || !kv[i].value) continue;
        slot->kv_keys[slot->kv_count] = jce_aid_strdup(kv[i].key);
        slot->kv_vals[slot->kv_count] = jce_aid_strdup(kv[i].value);
        if (slot->kv_keys[slot->kv_count] && slot->kv_vals[slot->kv_count])
            slot->kv_count++;
    }

    if (g_q.queued >= AID_QUEUE_DEPTH) {
        /* spec J: queue full -> this request goes straight to T3, done
         * synchronously right here (T3 is pure compute, no blocking). */
        const JceAidSchema* s = jce_aid_schema_get(id);
        JceAidStamp         stamp = slot->stamp;
        JceAidMode          saved = st->cfg.mode;
        JceAidResult        r;
        st->cfg.mode = JCE_AID_MODE_FORCE_T3;
        r = jce_aid_chain_acquire(s, NULL, 0, &stamp, &slot->record);
        st->cfg.mode = saved;
        if (r != JCE_AID_OK) {
            slot_release(slot);
            jce_mutex_unlock(g_q.mutex);
            return 0;
        }
        slot->state = SLOT_DONE;
        h = slot_handle(slot, idx);
        jce_mutex_unlock(g_q.mutex);
        return h;
    }

    slot->state = SLOT_QUEUED;
    g_q.queued++;
    h = slot_handle(slot, idx);
    jce_cond_signal(g_q.cond);
    jce_mutex_unlock(g_q.mutex);
    return h;
}

JceAidStatus JCE_CALL jce_aid_status(JceAidHandle h)
{
    JceAidStatus out = JCE_AID_FAILED;
    Slot*        s;
    if (!g_q.alive || !jce_aid_state()) return JCE_AID_FAILED;
    jce_mutex_lock(g_q.mutex);
    s = slot_resolve(h, NULL);
    if (s) {
        switch (s->state) {
        case SLOT_QUEUED:
        case SLOT_RUNNING:
        case SLOT_DONE:  out = JCE_AID_PENDING; break;
        case SLOT_READY: out = JCE_AID_READY;   break;
        default:         out = JCE_AID_FAILED;  break;
        }
    }
    jce_mutex_unlock(g_q.mutex);
    return out;
}

JceAidResult JCE_CALL jce_aid_take_record(JceAidHandle h, JceAidRecord* out)
{
    JceAidResult r = JCE_AID_ERR_NOT_FOUND;
    Slot*        s;
    if (!out) return JCE_AID_ERR_INVALID_ARG;
    if (!g_q.alive || !jce_aid_state()) return JCE_AID_ERR_NOT_INIT;
    jce_mutex_lock(g_q.mutex);
    s = slot_resolve(h, NULL);
    if (s && s->state == SLOT_READY) {
        *out = s->record;
        slot_release(s);
        r = JCE_AID_OK;
    }
    jce_mutex_unlock(g_q.mutex);
    return r;
}

void JCE_CALL jce_aid_bind_bus(struct jce_event_bus* bus)
{
    JceAidState* st = jce_aid_state();
    if (st) st->bound_bus = bus;
}

void JCE_CALL jce_aid_bump_generation(void)
{
    JceAidState* st = jce_aid_state();
    if (st) st->generation++;
}

uint32_t JCE_CALL jce_aid_pump(uint32_t max_results)
{
    JceAidState* st = jce_aid_state();
    uint32_t     consumed = 0;
    uint64_t     t0;
    uint32_t     i;

    if (!st || !g_q.alive) return 0;
    if (max_results == 0) max_results = AID_PUMP_DEFAULT;
    t0 = jce_time_ticks_ns();

    for (i = 0; i < AID_SLOT_COUNT && consumed < max_results; ++i) {
        Slot*        s = &g_q.slots[i];
        JceAidRecord rec;
        int          have = 0, stale = 0;

        jce_mutex_lock(g_q.mutex);
        if (s->state == SLOT_DONE) {
            if (s->world_gen != st->generation) {
                stale = 1;
                slot_release(s); /* stale generation: discard (spec H) */
            } else {
                rec = s->record;
                s->state = SLOT_READY;
                have = 1;
            }
        }
        jce_mutex_unlock(g_q.mutex);

        if (have) {
            /* log + stream + bound bus (same path as manual publish) */
            jce_aid_publish_record((struct jce_event_bus*)st->bound_bus, &rec);
            consumed++;
        }
        (void)stale;
        if (jce_time_ticks_ns() - t0 > AID_PUMP_NS) break; /* 0.5 ms budget */
    }
    return consumed;
}

uint32_t JCE_CALL
jce_aid_engine_tick(uint64_t up_to_tick, uint32_t max_records)
{
    JceAidState* st = jce_aid_state();
    if (!st) return 0;
    if (st->replay_active)
        return jce_aid_replay_pump((struct jce_event_bus*)st->bound_bus,
                                   up_to_tick, max_records ? max_records
                                                           : AID_PUMP_DEFAULT,
                                   NULL);
    return jce_aid_pump(max_records);
}

JceBool JCE_CALL jce_aid_initialised(void)
{
    return jce_aid_state() ? JCE_TRUE : JCE_FALSE;
}
