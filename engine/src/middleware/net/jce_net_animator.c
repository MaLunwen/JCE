/*
 * jce_net_animator.c  Animator state replication.  See jce_net_animator.h.
 *
 * Shaped after jce_net_transform.c on purpose: flat array linear-scanned by
 * net_id, a per-entry ring of inbound snapshots, a tick-based interpolation
 * timeline, and the same jce__rpc_transport_broadcast seam.  Two networking
 * subsystems with different vocabularies would be two things to learn for one
 * idea.
 */

#include <jce/middleware/net/jce_net_animator.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_log.h>

#include "jce_net_bytes.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "net.anim"

/* Must not collide with the private packet ids in jce_replication.c, which
 * are 1 snapshot, 2 owner-change, 3 rpc, 4 session, 5/6 transform, 8 ACK.
 * 7 and 9 are what is left -- the first draft took 8 and the compiler caught
 * it only because the dispatch switch happens to name both.  These two lines
 * and their twins in jce_replication.c are the whole registry there is. */
#define JCE_REPL_PKT_NET_ANIMATOR         ((uint8_t)7)
#define JCE_REPL_PKT_NET_ANIMATOR_TRIGGER ((uint8_t)9)

#define TRIGGER_QUEUE_MAX 8u

extern void jce__rpc_transport_broadcast(const void *data, uint32_t size,
                                         JceNetDelivery delivery);

typedef struct NaEntry {
    JceNetObjectId       id;
    bool                 used;
    JceNetAnimatorConfig cfg;

    /* What the authority most recently said this animator is doing. */
    JceNetAnimatorState  local;
    bool                 has_local;

    JceNetAnimatorSnapshot ring[JCE_NET_ANIMATOR_SNAPSHOT_HISTORY];
    uint32_t               ring_count;   /* total ever written */

    /* The interpolated result render_step produced, and whether there is one. */
    JceNetAnimatorState  applied;
    bool                 has_applied;

    uint32_t             triggers[TRIGGER_QUEUE_MAX];
    uint8_t              trigger_head, trigger_count;
} NaEntry;

static struct {
    NaEntry             *entries;
    uint32_t             count, cap;
    JceScene            *scene;
    JceNetAnimatorConfig defaults;
    bool                 defaults_set;
    uint32_t             last_known_tick;
    uint32_t             stat_state_changes;
    uint32_t             stat_trig_sent;
    uint32_t             stat_trig_recv;
} g_na;

/* ================================================================== */

/* The fixed rate, from the same source jce_net_transform.c reads -- so the
 * two subsystems' interpolation timelines cannot disagree about what a tick
 * is. */
static uint32_t fixed_hz_now(void)
{
    JceFixedClock *fc = jce_fixed_clock_default();
    if (!fc || fc->fixed_dt <= 0.0) return 60u;   /* engine fixed default */
    const double hz = 1.0 / fc->fixed_dt;
    return (hz > 0.0) ? (uint32_t)(hz + 0.5) : 60u;
}

uint32_t jce_net_animator_hash(const char *name)
{
    /* FNV-1a.  PUBLIC because both sides must agree -- a caller that hashed
     * names its own way would produce ids this module transports faithfully
     * and the receiver never matches. */
    uint32_t h = 2166136261u;
    if (!name) return 0u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

static void defaults_fill(JceNetAnimatorConfig *c)
{
    if (c->snapshot_hz == 0u)     c->snapshot_hz = 20u;
    if (c->interp_delay_ms == 0u) c->interp_delay_ms = 100u;
}

void jce_net_animator_set_default_config(const JceNetAnimatorConfig *cfg)
{
    if (!cfg) { memset(&g_na.defaults, 0, sizeof g_na.defaults);
                g_na.defaults_set = false; return; }
    g_na.defaults = *cfg;
    defaults_fill(&g_na.defaults);
    g_na.defaults_set = true;
}

void jce_net_animator_get_default_config(JceNetAnimatorConfig *out)
{
    if (!out) return;
    if (g_na.defaults_set) { *out = g_na.defaults; return; }
    memset(out, 0, sizeof *out);
    defaults_fill(out);
}

void jce_net_animator_set_scene(JceScene *scene) { g_na.scene = scene; }

static NaEntry *find(JceNetObjectId id)
{
    for (uint32_t i = 0; i < g_na.count; i++)
        if (g_na.entries[i].used && g_na.entries[i].id == id)
            return &g_na.entries[i];
    return NULL;
}

bool jce_net_animator_register(JceNetObjectId id,
                               const JceNetAnimatorConfig *cfg_or_null)
{
    JceNetAnimatorConfig cfg;
    if (cfg_or_null) { cfg = *cfg_or_null; defaults_fill(&cfg); }
    else             { jce_net_animator_get_default_config(&cfg); }

    NaEntry *e = find(id);
    if (e) { e->cfg = cfg; return true; }   /* idempotent refresh */

    /* Reuse a freed slot before growing: registrations churn with spawns. */
    for (uint32_t i = 0; i < g_na.count; i++) {
        if (!g_na.entries[i].used) { e = &g_na.entries[i]; break; }
    }
    if (!e) {
        if (g_na.count >= g_na.cap) {
            const uint32_t nc = g_na.cap ? g_na.cap * 2u : 16u;
            NaEntry *nb = (NaEntry *)JCE_REALLOC(g_na.entries,
                                                 (size_t)nc * sizeof *nb);
            if (!nb) return false;
            memset(nb + g_na.cap, 0, (size_t)(nc - g_na.cap) * sizeof *nb);
            g_na.entries = nb;
            g_na.cap = nc;
        }
        e = &g_na.entries[g_na.count++];
    }
    memset(e, 0, sizeof *e);
    e->id = id;
    e->used = true;
    e->cfg = cfg;
    return true;
}

void jce_net_animator_unregister(JceNetObjectId id)
{
    NaEntry *e = find(id);
    if (e) { memset(e, 0, sizeof *e); }
}

void jce_net_animator_set_local_state(JceNetObjectId id,
                                      const JceNetAnimatorState *state)
{
    NaEntry *e = find(id);
    if (!e || !state) return;
    if (e->has_local && e->local.state_hash != state->state_hash)
        g_na.stat_state_changes++;
    e->local = *state;
    if (e->local.param_count > JCE_NET_ANIMATOR_MAX_PARAMS)
        e->local.param_count = JCE_NET_ANIMATOR_MAX_PARAMS;
    e->has_local = true;
}

bool jce_net_animator_get_state(JceNetObjectId id, JceNetAnimatorState *out)
{
    NaEntry *e = find(id);
    if (!e || !out) return false;
    if (e->has_applied) { *out = e->applied; return true; }
    /* An authority reads back its own state; a receiver that has been told
     * nothing gets false -- which is NOT the same as "it is idle". */
    if (e->has_local) { *out = e->local; return true; }
    return false;
}

/* ================================================================== */
/* Snapshots                                                           */
/* ================================================================== */

static void ring_push(NaEntry *e, uint32_t tick, const JceNetAnimatorState *s)
{
    JceNetAnimatorSnapshot *slot =
        &e->ring[e->ring_count % JCE_NET_ANIMATOR_SNAPSHOT_HISTORY];
    slot->server_tick = tick;
    slot->state = *s;
    e->ring_count++;
    if (tick > g_na.last_known_tick) g_na.last_known_tick = tick;
}

void jce_net_animator_inject_snapshot(JceNetObjectId id, uint32_t server_tick,
                                      const JceNetAnimatorState *state)
{
    NaEntry *e = find(id);
    if (!e || !state) return;
    ring_push(e, server_tick, state);
}

/* NORMALISED TIME WRAPS, and this is the whole reason this is not a lerp.
 *
 * A looping clip runs 0 -> 1 -> 0.  Between a snapshot at 0.95 and one at
 * 0.05 the animation moved FORWARD by 0.10 through the loop point; a plain
 * lerp reads it as moving backward by 0.90 and plays the clip in reverse,
 * once per loop, forever.  Detected by the shorter direction around the
 * circle, which is the same rule an angle interpolator uses. */
static float lerp_normalized_time(float a, float b, float t)
{
    float d = b - a;
    if (d > 0.5f)  d -= 1.0f;
    if (d < -0.5f) d += 1.0f;
    float v = a + d * t;
    while (v < 0.0f) v += 1.0f;
    while (v >= 1.0f) v -= 1.0f;
    return v;
}

static void interpolate(NaEntry *e, uint32_t render_tick)
{
    const uint32_t n = e->ring_count < JCE_NET_ANIMATOR_SNAPSHOT_HISTORY
                     ? e->ring_count : JCE_NET_ANIMATOR_SNAPSHOT_HISTORY;
    if (n == 0u) return;

    const JceNetAnimatorSnapshot *older = NULL, *newer = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const JceNetAnimatorSnapshot *s = &e->ring[i];
        if (s->server_tick <= render_tick &&
            (!older || s->server_tick > older->server_tick)) older = s;
        if (s->server_tick > render_tick &&
            (!newer || s->server_tick < newer->server_tick)) newer = s;
    }

    if (!older && !newer) return;
    if (!older) { e->applied = newer->state; e->has_applied = true; return; }
    if (!newer) { e->applied = older->state; e->has_applied = true; return; }

    /* A STATE CHANGE IS NOT A BLEND.  Forty per cent of the way from "idle"
     * to "jump" is not a pose, and neither is a parameter set that is half
     * one state's and half another's.  Snap to the newer snapshot. */
    if (older->state.state_hash != newer->state.state_hash) {
        e->applied = newer->state;
        e->has_applied = true;
        return;
    }

    const uint32_t span = newer->server_tick - older->server_tick;
    float t = span ? (float)(render_tick - older->server_tick) / (float)span
                   : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    JceNetAnimatorState out = older->state;
    out.normalized_time = lerp_normalized_time(older->state.normalized_time,
                                               newer->state.normalized_time, t);
    out.speed = older->state.speed + (newer->state.speed - older->state.speed) * t;
    /* Parameters interpolate only where BOTH snapshots carry the same
     * parameter in the same slot; a graph that changed its parameter set
     * between two snapshots takes the newer values whole. */
    for (uint8_t p = 0; p < out.param_count; p++) {
        if (p < newer->state.param_count &&
            newer->state.param_hash[p] == out.param_hash[p]) {
            out.param_value[p] += (newer->state.param_value[p] -
                                   out.param_value[p]) * t;
        }
    }
    e->applied = out;
    e->has_applied = true;
}

/* ================================================================== */
/* Wire                                                                */
/* ================================================================== */

static bool is_authority_for(const NaEntry *e)
{
    const JceNetRole role = jce_net_replication_role();
    if (e->cfg.authority == JCE_NET_ANIM_AUTHORITY_OWNER)
        return jce_net_object_owner(e->id) == jce_net_local_client_id();
    return role == JCE_NET_ROLE_SERVER;
}

void jce_net_animator_fixed_step(uint32_t tick)
{
    if (g_na.count == 0u) return;
    if (tick > g_na.last_known_tick) g_na.last_known_tick = tick;

    const uint32_t fixed_hz = fixed_hz_now();

    JceNetWBuf w = JCE_NET_WBUF_INIT;
    uint16_t count = 0;
    jce_net_w_u8(&w, JCE_REPL_PKT_NET_ANIMATOR);
    jce_net_w_u32(&w, tick);
    const uint32_t count_at = w.size;
    jce_net_w_u16(&w, 0);

    for (uint32_t i = 0; i < g_na.count; i++) {
        NaEntry *e = &g_na.entries[i];
        if (!e->used || !e->has_local || !is_authority_for(e)) continue;
        const uint32_t hzs = e->cfg.snapshot_hz ? e->cfg.snapshot_hz : 20u;
        const uint32_t every = fixed_hz / (hzs ? hzs : 1u);
        if (every > 1u && (tick % every) != 0u) continue;

        jce_net_w_u32(&w, (uint32_t)e->id);
        jce_net_w_u32(&w, e->local.state_hash);
        jce_net_w_f32(&w, e->local.normalized_time);
        jce_net_w_f32(&w, e->local.speed);
        jce_net_w_u8 (&w, e->local.param_count);
        for (uint8_t p = 0; p < e->local.param_count; p++) {
            jce_net_w_u32(&w, e->local.param_hash[p]);
            jce_net_w_f32(&w, e->local.param_value[p]);
        }
        count++;

        /* The authority appends its OWN sample to its inbound ring, the same
         * way the transform layer does: it keeps the in-process host and the
         * receive path on one code path instead of two that can disagree. */
        ring_push(e, tick, &e->local);
    }

    if (count && w.ok && w.buf) {
        w.buf[count_at]     = (uint8_t)(count & 0xFFu);
        w.buf[count_at + 1] = (uint8_t)(count >> 8);
        jce__rpc_transport_broadcast(w.buf, w.size, JCE_NET_UNRELIABLE);
    }
    if (w.buf) JCE_FREE(w.buf);
}

void jce_net_animator_render_step(double interp_alpha)
{
    (void)interp_alpha;   /* the timeline is snapshot-tick based, as for transforms */
    const uint32_t fixed_hz = fixed_hz_now();

    for (uint32_t i = 0; i < g_na.count; i++) {
        NaEntry *e = &g_na.entries[i];
        if (!e->used || e->ring_count == 0u) continue;
        if (is_authority_for(e)) continue;   /* it is driving itself */

        const uint32_t delay_ticks =
            ((e->cfg.interp_delay_ms ? e->cfg.interp_delay_ms : 100u) * fixed_hz)
            / 1000u;
        const uint32_t render_tick =
            (g_na.last_known_tick > delay_ticks)
                ? (g_na.last_known_tick - delay_ticks) : 0u;
        interpolate(e, render_tick);
    }
}

void jce_net_animator_fire_trigger(JceNetObjectId id, uint32_t trigger_hash)
{
    NaEntry *e = find(id);
    if (!e) return;
    JceNetWBuf w = JCE_NET_WBUF_INIT;
    jce_net_w_u8 (&w, JCE_REPL_PKT_NET_ANIMATOR_TRIGGER);
    jce_net_w_u32(&w, (uint32_t)id);
    jce_net_w_u32(&w, trigger_hash);
    if (w.ok && w.buf) {
        /* RELIABLE.  A trigger is an edge, and the snapshot stream that
         * carries state is unreliable by design -- a dropped footstep never
         * fires and no later packet corrects it. */
        jce__rpc_transport_broadcast(w.buf, w.size, JCE_NET_RELIABLE);
        g_na.stat_trig_sent++;
    }
    if (w.buf) JCE_FREE(w.buf);
}

static void trigger_push(NaEntry *e, uint32_t hash)
{
    if (e->trigger_count >= TRIGGER_QUEUE_MAX) {
        /* Drop the OLDEST, not the newest: a queue that discards new triggers
         * when full stops responding exactly when the most is happening. */
        e->trigger_head = (uint8_t)((e->trigger_head + 1u) % TRIGGER_QUEUE_MAX);
        e->trigger_count--;
    }
    e->triggers[(e->trigger_head + e->trigger_count) % TRIGGER_QUEUE_MAX] = hash;
    e->trigger_count++;
    g_na.stat_trig_recv++;
}

void jce_net_animator_inject_trigger(JceNetObjectId id, uint32_t trigger_hash)
{
    NaEntry *e = find(id);
    if (e) trigger_push(e, trigger_hash);
}

bool jce_net_animator_poll_trigger(JceNetObjectId id, uint32_t *out_trigger_hash)
{
    NaEntry *e = find(id);
    if (!e || e->trigger_count == 0u) return false;
    if (out_trigger_hash) *out_trigger_hash = e->triggers[e->trigger_head];
    e->trigger_head = (uint8_t)((e->trigger_head + 1u) % TRIGGER_QUEUE_MAX);
    e->trigger_count--;
    return true;
}

/* Called by jce_replication.c's packet switch. */
void jce__net_animator_recv_packet(const void *data, uint32_t size)
{
    if (!data || size < 1u) return;
    JceNetRBuf r = JCE_NET_RBUF_INIT(data, size);
    uint8_t type = 0;
    if (!jce_net_r_bytes(&r, &type, 1)) return;

    if (type == JCE_REPL_PKT_NET_ANIMATOR_TRIGGER) {
        uint32_t id = 0, hash = 0;
        if (!jce_net_r_u32(&r, &id) || !jce_net_r_u32(&r, &hash)) return;
        NaEntry *e = find((JceNetObjectId)id);
        if (e) trigger_push(e, hash);
        return;
    }
    if (type != JCE_REPL_PKT_NET_ANIMATOR) return;

    uint32_t tick = 0;
    uint16_t count = 0;
    if (!jce_net_r_u32(&r, &tick) || !jce_net_r_u16(&r, &count)) return;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t id = 0;
        JceNetAnimatorState st;
        memset(&st, 0, sizeof st);
        uint8_t pc = 0;
        if (!jce_net_r_u32(&r, &id) ||
            !jce_net_r_u32(&r, &st.state_hash) ||
            !jce_net_r_f32(&r, &st.normalized_time) ||
            !jce_net_r_f32(&r, &st.speed) ||
            !jce_net_r_bytes(&r, &pc, 1)) return;
        if (pc > JCE_NET_ANIMATOR_MAX_PARAMS) return;   /* refuse, do not clamp:
                                                         * a bad count means the
                                                         * rest of the packet is
                                                         * not what it claims */
        st.param_count = pc;
        for (uint8_t p = 0; p < pc; p++) {
            if (!jce_net_r_u32(&r, &st.param_hash[p]) ||
                !jce_net_r_f32(&r, &st.param_value[p])) return;
        }
        NaEntry *e = find((JceNetObjectId)id);
        if (!e) continue;              /* not registered here; ignore */
        if (is_authority_for(e)) continue;   /* never overwrite our own */
        ring_push(e, tick, &st);
    }
}

/* ================================================================== */

uint32_t jce_net_animator_registered_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_na.count; i++) if (g_na.entries[i].used) n++;
    return n;
}
uint32_t jce_net_animator_state_changes_count(void)   { return g_na.stat_state_changes; }
uint32_t jce_net_animator_triggers_sent_count(void)    { return g_na.stat_trig_sent; }
uint32_t jce_net_animator_triggers_received_count(void){ return g_na.stat_trig_recv; }

void jce_net_animator_reset_stats(void)
{
    g_na.stat_state_changes = 0;
    g_na.stat_trig_sent = 0;
    g_na.stat_trig_recv = 0;
}

void jce_net_animator_shutdown(void)
{
    JCE_FREE(g_na.entries);
    memset(&g_na, 0, sizeof g_na);
}
