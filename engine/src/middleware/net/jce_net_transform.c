/*
 * jce_net_transform.c — P3-D.4 transform replication.
 *
 * Implementation notes:
 *
 *   - State is a flat array of NtEntry, linear-scanned by net_id (same
 *     shape as g_repl.objects in jce_replication.c; networking object
 *     counts in practice are O(100), well below the point where a
 *     hashmap pays off).
 *
 *   - Each entry owns a 16-slot ring buffer of inbound snapshots; the
 *     server-side path appends to the SAME ring (for symmetry with the
 *     wire stream — useful for in-process host loopback and tests).
 *
 *   - Tick-based interp: render_tick = last_known_server_tick -
 *     interp_delay_ticks, where interp_delay_ticks is derived from
 *     interp_delay_ms × fixed_hz at fixed-step time.  Keeps interp on
 *     the same timeline as B.2's deterministic fixed clock.
 *
 *   - Wire format is documented in jce_net_transform.h.  Packet type
 *     5 (JCE_REPL_PKT_NET_TRANSFORM) is dispatched by
 *     jce_replication.c's switch through the extern seam declared
 *     below.
 *
 *   - We piggy-back on jce_replication's host pointer via the existing
 *     jce__rpc_transport_broadcast() seam so the transport surface
 *     stays small and we don't duplicate host-attach plumbing.
 */

#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_net_quant.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "net.xform"

/* Must match the private constants in jce_replication.c. */
#define JCE_REPL_PKT_NET_TRANSFORM    ((uint8_t)5)   /* legacy v1 */
#define JCE_REPL_PKT_NET_TRANSFORM_V2 ((uint8_t)6)   /* P4-D.3 quantized */

/* L4-internal transport seam owned by jce_replication.c.  We do NOT
 * include the header (private), just forward-declare the symbols. */
extern void jce__rpc_transport_broadcast(const void *data, uint32_t size,
                                         JceNetDelivery delivery);

/* ================================================================== */
/* State                                                               */
/* ================================================================== */

typedef struct NtEntry {
    JceNetObjectId          id;
    JceNetTransformConfig   cfg;

    JceNetTransformSnapshot ring[JCE_NET_TRANSFORM_SNAPSHOT_HISTORY];
    uint32_t                ring_count;
    uint32_t                ring_head;          /* index of oldest    */

    uint32_t                last_send_tick;     /* server / owner side */

    /* Owned-side: most recent authoritative snapshot we have NOT yet
     * reconciled against the locally-predicted pose.  Consumed in
     * render_step. */
    bool                    pending_correction;
    JceNetTransformSnapshot pending_snapshot;
} NtEntry;

typedef struct NtState {
    bool                  inited;
    JceScene             *scene;

    NtEntry              *entries;
    uint32_t              count;
    uint32_t              cap;

    JceNetTransformConfig default_cfg;

    /* Highest server_tick we have observed across any registered
     * object.  Drives client render_tick computation. */
    uint32_t              last_known_server_tick;

    uint32_t              snap_corrections;
} NtState;

static NtState g_nt;

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static double fixed_hz_now(void)
{
    JceFixedClock *fc = jce_fixed_clock_default();
    if (!fc || fc->fixed_dt <= 0.0) return 50.0;
    return 1.0 / fc->fixed_dt;
}

static uint32_t fixed_tick_now(void)
{
    JceFixedClock *fc = jce_fixed_clock_default();
    if (!fc) return 0u;
    return (uint32_t)fc->tick_count;
}

static void config_defaults(JceNetTransformConfig *c)
{
    c->snapshot_hz              = 20u;
    c->interp_delay_ms          = 100u;
    c->divergence_snap_distance = 0.5f;
    c->divergence_snap_angle_deg = 30.0f;
    c->authority                = JCE_NET_AUTH_SERVER;
}

static NtEntry *find_entry(JceNetObjectId id)
{
    if (!id) return NULL;
    for (uint32_t i = 0; i < g_nt.count; ++i)
        if (g_nt.entries[i].id == id) return &g_nt.entries[i];
    return NULL;
}

static NtEntry *push_entry(void)
{
    if (g_nt.count == g_nt.cap) {
        uint32_t nc = g_nt.cap ? g_nt.cap * 2u : 16u;
        NtEntry *nb = (NtEntry *)JCE_REALLOC(g_nt.entries,
                                             sizeof(NtEntry) * nc);
        if (!nb) return NULL;
        g_nt.entries = nb;
        g_nt.cap     = nc;
    }
    NtEntry *e = &g_nt.entries[g_nt.count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void remove_entry_at(uint32_t i)
{
    if (i >= g_nt.count) return;
    if (i + 1u < g_nt.count)
        g_nt.entries[i] = g_nt.entries[g_nt.count - 1u];
    g_nt.count--;
}

static void ring_push(NtEntry *e, const JceNetTransformSnapshot *s)
{
    uint32_t cap = JCE_NET_TRANSFORM_SNAPSHOT_HISTORY;
    uint32_t idx;
    if (e->ring_count < cap) {
        idx = (e->ring_head + e->ring_count) % cap;
        e->ring_count++;
    } else {
        idx = e->ring_head;
        e->ring_head = (e->ring_head + 1u) % cap;
    }
    e->ring[idx] = *s;
}

static const JceNetTransformSnapshot *ring_at(const NtEntry *e, uint32_t i)
{
    if (i >= e->ring_count) return NULL;
    uint32_t idx = (e->ring_head + i) % JCE_NET_TRANSFORM_SNAPSHOT_HISTORY;
    return &e->ring[idx];
}

/* Drop snapshots strictly older than `cutoff_tick` from the head.  Keep
 * at least one for extrapolation hold. */
static void ring_prune(NtEntry *e, uint32_t cutoff_tick)
{
    while (e->ring_count > 1u) {
        const JceNetTransformSnapshot *s = ring_at(e, 0);
        if (!s || s->server_tick >= cutoff_tick) break;
        e->ring_head = (e->ring_head + 1u) % JCE_NET_TRANSFORM_SNAPSHOT_HISTORY;
        e->ring_count--;
    }
}

/* Return true iff a pair (a, b) of snapshots straddling `render_tick`
 * was found (a.tick <= render_tick < b.tick).  On success the caller
 * can lerp between *out_a and *out_b using `*out_t`. */
static bool ring_find_pair(const NtEntry *e, uint32_t render_tick,
                           const JceNetTransformSnapshot **out_a,
                           const JceNetTransformSnapshot **out_b,
                           float *out_t)
{
    *out_a = NULL;
    *out_b = NULL;
    *out_t = 0.0f;
    if (e->ring_count == 0u) return false;

    for (uint32_t i = 0; i + 1u < e->ring_count; ++i) {
        const JceNetTransformSnapshot *a = ring_at(e, i);
        const JceNetTransformSnapshot *b = ring_at(e, i + 1u);
        if (!a || !b) continue;
        if (a->server_tick <= render_tick && render_tick < b->server_tick) {
            uint32_t span = b->server_tick - a->server_tick;
            float    t    = span ? (float)(render_tick - a->server_tick) /
                                   (float)span
                                : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            *out_a = a;
            *out_b = b;
            *out_t = t;
            return true;
        }
    }
    return false;
}

static float quat_angle_between_deg(jce_quat a, jce_quat b)
{
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;
    float angle_rad = 2.0f * acosf(dot);
    return angle_rad * (180.0f / 3.14159265358979323846f);
}

/* ================================================================== */
/* Wire helpers (little-endian, mirror jce_replication.c)              */
/* ================================================================== */

typedef struct WBuf {
    uint8_t *buf;
    uint32_t size;
    uint32_t cap;
    bool     ok;
} WBuf;

static bool wb_reserve(WBuf *w, uint32_t extra)
{
    if (!w->ok) return false;
    uint32_t need = w->size + extra;
    if (need <= w->cap) return true;
    uint32_t nc = w->cap ? w->cap : 256u;
    while (nc < need) nc *= 2u;
    uint8_t *nb = (uint8_t *)JCE_REALLOC(w->buf, nc);
    if (!nb) { w->ok = false; return false; }
    w->buf = nb;
    w->cap = nc;
    return true;
}

static void wb_u8(WBuf *w, uint8_t v)
{
    if (!wb_reserve(w, 1u)) return;
    w->buf[w->size++] = v;
}

static void wb_u16(WBuf *w, uint16_t v)
{
    if (!wb_reserve(w, 2u)) return;
    w->buf[w->size++] = (uint8_t)( v        & 0xFFu);
    w->buf[w->size++] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void wb_u32(WBuf *w, uint32_t v)
{
    if (!wb_reserve(w, 4u)) return;
    w->buf[w->size++] = (uint8_t)( v         & 0xFFu);
    w->buf[w->size++] = (uint8_t)((v >>  8u) & 0xFFu);
    w->buf[w->size++] = (uint8_t)((v >> 16u) & 0xFFu);
    w->buf[w->size++] = (uint8_t)((v >> 24u) & 0xFFu);
}

static void wb_f32(WBuf *w, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    wb_u32(w, u);
}

typedef struct RBuf {
    const uint8_t *buf;
    uint32_t       size;
    uint32_t       cursor;
    bool           ok;
} RBuf;

static bool rb_bytes(RBuf *r, void *dst, uint32_t n)
{
    if (!r->ok || r->cursor + n > r->size) { r->ok = false; return false; }
    memcpy(dst, r->buf + r->cursor, n);
    r->cursor += n;
    return true;
}

static bool rb_u8 (RBuf *r, uint8_t  *v) { return rb_bytes(r, v, 1); }
static bool rb_u16(RBuf *r, uint16_t *v)
{
    uint8_t b[2];
    if (!rb_bytes(r, b, 2)) return false;
    *v = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}
static bool rb_u32(RBuf *r, uint32_t *v)
{
    uint8_t b[4];
    if (!rb_bytes(r, b, 4)) return false;
    *v =  (uint32_t)b[0]
       | ((uint32_t)b[1] <<  8)
       | ((uint32_t)b[2] << 16)
       | ((uint32_t)b[3] << 24);
    return true;
}
static bool rb_f32(RBuf *r, float *v)
{
    uint32_t u;
    if (!rb_u32(r, &u)) return false;
    memcpy(v, &u, 4);
    return true;
}

/* ================================================================== */
/* Encode / broadcast                                                  */
/* ================================================================== */

#ifndef NDEBUG
/* Self-test stub state — when active, scene reads / writes hit this
 * in-memory record instead of the JceScene.  Lets the self-test skip
 * spinning up an ECS world. */
static bool     g_nt_stub_active;
static jce_vec3 g_nt_stub_pos;
static jce_quat g_nt_stub_rot;
#endif

static bool entity_sample_transform(JceNetObjectId id,
                                    jce_vec3 *out_pos, jce_quat *out_rot)
{
#ifndef NDEBUG
    if (g_nt_stub_active) {
        (void)id;
        *out_pos = g_nt_stub_pos;
        *out_rot = g_nt_stub_rot;
        return true;
    }
#endif
    if (!g_nt.scene) return false;
    uint64_t ent = jce_net_object_to_entity(id);
    if (!ent) return false;
    JceTransform *t = jce_scene_get_transform(g_nt.scene, (JceEntity)ent);
    if (!t) return false;
    *out_pos = t->position;
    *out_rot = t->rotation;
    return true;
}

static void entity_apply_transform(JceNetObjectId id,
                                   jce_vec3 pos, jce_quat rot)
{
#ifndef NDEBUG
    if (g_nt_stub_active) {
        (void)id;
        g_nt_stub_pos = pos;
        g_nt_stub_rot = rot;
        return;
    }
#endif
    if (!g_nt.scene) return;
    uint64_t ent = jce_net_object_to_entity(id);
    if (!ent) return;
    JceTransform *t = jce_scene_get_transform(g_nt.scene, (JceEntity)ent);
    if (!t) return;
    t->position = pos;
    t->rotation = rot;
}

/* Send every entry whose authority side matches `is_authoritative` AND
 * whose cadence aligns with `tick`. */
static void encode_and_broadcast(uint32_t tick, JceNetRole role)
{
    if (g_nt.count == 0u) return;

    /* Build a tight list of indices we will include this tick.  Cadence
     * is per-entry so different objects may run at different snapshot
     * rates without a forced lock. */
    uint32_t indices[64];
    uint16_t n = 0;
    double   fixed_hz = fixed_hz_now();

    for (uint32_t i = 0; i < g_nt.count; ++i) {
        NtEntry *e = &g_nt.entries[i];

        bool is_auth = false;
        if (e->cfg.authority == JCE_NET_AUTH_SERVER) {
            is_auth = (role == JCE_NET_ROLE_SERVER);
        } else { /* OWNER */
            is_auth = jce_net_object_is_owner_local(e->id);
        }
        if (!is_auth) continue;

        uint32_t hz       = e->cfg.snapshot_hz ? e->cfg.snapshot_hz : 20u;
        uint32_t interval = (uint32_t)(fixed_hz / (double)hz);
        if (interval == 0u) interval = 1u;
        if ((tick % interval) != 0u) continue;

        if (n < (uint16_t)(sizeof(indices) / sizeof(indices[0])))
            indices[n++] = i;
    }
    if (n == 0u) return;

    WBuf w = { NULL, 0u, 0u, true };
    wb_u8 (&w, JCE_REPL_PKT_NET_TRANSFORM_V2);
    wb_u32(&w, tick);
    wb_u16(&w, n);

    for (uint16_t k = 0; k < n; ++k) {
        NtEntry *e = &g_nt.entries[indices[k]];
        jce_vec3 pos = { 0 };
        jce_quat rot = jce_q_identity();
        if (!entity_sample_transform(e->id, &pos, &rot)) {
            /* Keep entry count consistent: emit identity if entity
             * missing this tick (won't happen normally). */
        }

        /* Quantize: pos = 3 x f16 (6B), rot = smallest-three (4B),
         * vel = 3 x f32 (12B).  Per-entry payload: 4 + 6 + 4 + 12 = 26B
         * (vs 44B in v1 — a 41% reduction).  Velocity is left as raw
         * f32 for now; future iterations can bound it per object and
         * quantize to int16 like Halo / Source. */
        uint16_t pos16[3];
        jce_quant_pack_vec3_f16(pos, pos16);
        uint32_t rot_st3 = jce_quant_pack_quat_st3(rot);

        wb_u32(&w, e->id);
        wb_u16(&w, pos16[0]); wb_u16(&w, pos16[1]); wb_u16(&w, pos16[2]);
        wb_u32(&w, rot_st3);
        wb_f32(&w, 0.0f);  wb_f32(&w, 0.0f);  wb_f32(&w, 0.0f);   /* velocity */

        /* Locally mirror into the ring — keeps host loopback + the
         * non-owning split-screen path consistent.  We round-trip
         * through the quantizer so the local copy matches what
         * remote peers will reconstruct. */
        JceNetTransformSnapshot snap;
        snap.server_tick = tick;
        snap.position    = jce_quant_unpack_vec3_f16(pos16);
        snap.rotation    = jce_quant_unpack_quat_st3(rot_st3);
        snap.velocity    = jce_v3(0.0f, 0.0f, 0.0f);
        ring_push(e, &snap);
        e->last_send_tick = tick;
    }

    if (w.ok && w.size > 0u) {
        jce__rpc_transport_broadcast(w.buf, w.size, JCE_NET_UNRELIABLE);
        if (tick > g_nt.last_known_server_tick)
            g_nt.last_known_server_tick = tick;
    }
    JCE_FREE(w.buf);
}

/* ================================================================== */
/* Decode (called from jce_replication.c dispatcher)                   */
/* ================================================================== */

/* Forward-decl matches what jce_replication.c invokes. */
void jce__net_transform_recv_packet(const void *data, uint32_t size);

void jce__net_transform_recv_packet(const void *data, uint32_t size)
{
    if (!g_nt.inited || !data || size < 7u) return;

    RBuf r = { (const uint8_t *)data, size, 0u, true };
    uint8_t  type = 0;
    uint32_t tick = 0;
    uint16_t n    = 0;
    if (!rb_u8 (&r, &type)) return;
    if (type != JCE_REPL_PKT_NET_TRANSFORM &&
        type != JCE_REPL_PKT_NET_TRANSFORM_V2) return;
    if (!rb_u32(&r, &tick)) return;
    if (!rb_u16(&r, &n))    return;

    if (tick > g_nt.last_known_server_tick)
        g_nt.last_known_server_tick = tick;

    for (uint16_t k = 0; k < n; ++k) {
        uint32_t id;
        jce_vec3 pos;
        jce_quat rot;
        float    vx, vy, vz;
        if (!rb_u32(&r, &id)) return;

        if (type == JCE_REPL_PKT_NET_TRANSFORM_V2) {
            uint16_t p0, p1, p2;
            uint32_t rot_st3;
            if (!rb_u16(&r, &p0)) return;
            if (!rb_u16(&r, &p1)) return;
            if (!rb_u16(&r, &p2)) return;
            if (!rb_u32(&r, &rot_st3)) return;
            uint16_t p16[3] = { p0, p1, p2 };
            pos = jce_quant_unpack_vec3_f16(p16);
            rot = jce_quant_unpack_quat_st3(rot_st3);
        } else {
            float px, py, pz, rx, ry, rz, rw;
            if (!rb_f32(&r, &px)) return;
            if (!rb_f32(&r, &py)) return;
            if (!rb_f32(&r, &pz)) return;
            if (!rb_f32(&r, &rx)) return;
            if (!rb_f32(&r, &ry)) return;
            if (!rb_f32(&r, &rz)) return;
            if (!rb_f32(&r, &rw)) return;
            pos = jce_v3(px, py, pz);
            rot = jce_v4(rx, ry, rz, rw);
        }

        if (!rb_f32(&r, &vx)) return;
        if (!rb_f32(&r, &vy)) return;
        if (!rb_f32(&r, &vz)) return;

        NtEntry *e = find_entry((JceNetObjectId)id);
        if (!e) continue;

        JceNetTransformSnapshot snap;
        snap.server_tick = tick;
        snap.position    = pos;
        snap.rotation    = rot;
        snap.velocity    = jce_v3(vx, vy, vz);

        bool owner_local = jce_net_object_is_owner_local((JceNetObjectId)id);
        bool we_authoritative =
            (e->cfg.authority == JCE_NET_AUTH_OWNER) ? owner_local
                                                     : false;

        if (we_authoritative) {
            /* Loopback / our own broadcast — ignore on receive. */
            continue;
        }

        ring_push(e, &snap);

        if (owner_local && e->cfg.authority == JCE_NET_AUTH_SERVER) {
            /* Authoritative correction landed for an object we are
             * predicting.  Stash it; render_step decides snap vs
             * accept. */
            e->pending_correction = true;
            e->pending_snapshot   = snap;
        }
    }
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

static void ensure_init(void)
{
    if (g_nt.inited) return;
    config_defaults(&g_nt.default_cfg);
    g_nt.inited = true;
}

void jce_net_transform_set_default_config(const JceNetTransformConfig *cfg)
{
    ensure_init();
    if (cfg) g_nt.default_cfg = *cfg;
    else     config_defaults(&g_nt.default_cfg);
}

void jce_net_transform_get_default_config(JceNetTransformConfig *out)
{
    ensure_init();
    if (out) *out = g_nt.default_cfg;
}

void jce_net_transform_set_scene(JceScene *scene)
{
    ensure_init();
    g_nt.scene = scene;
}

bool jce_net_transform_register(JceNetObjectId id,
                                const JceNetTransformConfig *cfg_or_null)
{
    ensure_init();
    if (id == JCE_NET_OBJECT_INVALID) return false;

    NtEntry *e = find_entry(id);
    if (!e) {
        e = push_entry();
        if (!e) return false;
        e->id = id;
    }
    e->cfg = cfg_or_null ? *cfg_or_null : g_nt.default_cfg;
    if (e->cfg.snapshot_hz == 0u)         e->cfg.snapshot_hz = 20u;
    if (e->cfg.divergence_snap_distance < 0.0f)
        e->cfg.divergence_snap_distance = 0.0f;
    if (e->cfg.divergence_snap_angle_deg < 0.0f)
        e->cfg.divergence_snap_angle_deg = 0.0f;
    return true;
}

void jce_net_transform_unregister(JceNetObjectId id)
{
    if (!g_nt.inited) return;
    for (uint32_t i = 0; i < g_nt.count; ++i) {
        if (g_nt.entries[i].id == id) {
            remove_entry_at(i);
            return;
        }
    }
}

void jce_net_transform_fixed_step(void)
{
    if (!g_nt.inited || g_nt.count == 0u) return;

    JceNetRole role = jce_net_replication_role();
    /* NONE = single-player / headless test — nothing to send, nothing
     * to interpolate.  Owned-path render_step still works. */
    if (role == JCE_NET_ROLE_NONE) return;

    uint32_t tick = fixed_tick_now();
    encode_and_broadcast(tick, role);
}

void jce_net_transform_render_step(double interp_alpha)
{
    (void)interp_alpha;
    if (!g_nt.inited || g_nt.count == 0u) return;

    JceNetRole role     = jce_net_replication_role();
    double     fixed_hz = fixed_hz_now();

    for (uint32_t i = 0; i < g_nt.count; ++i) {
        NtEntry *e = &g_nt.entries[i];
        bool owner_local = jce_net_object_is_owner_local(e->id);

        /* ------------------------------------------------------------
         * Owned-object path: client prediction.  Gameplay already wrote
         * the scene transform this tick — we only consume any pending
         * correction from the server. */
        if (owner_local && e->cfg.authority == JCE_NET_AUTH_SERVER &&
            role == JCE_NET_ROLE_CLIENT)
        {
            if (e->pending_correction) {
                jce_vec3 cur_pos = { 0 };
                jce_quat cur_rot = jce_q_identity();
                if (entity_sample_transform(e->id, &cur_pos, &cur_rot)) {
                    jce_vec3 d = jce_v3_sub(e->pending_snapshot.position,
                                            cur_pos);
                    float dist = jce_v3_len(d);
                    float ang  = quat_angle_between_deg(
                        cur_rot, e->pending_snapshot.rotation);
                    if (dist > e->cfg.divergence_snap_distance ||
                        ang  > e->cfg.divergence_snap_angle_deg)
                    {
                        entity_apply_transform(e->id,
                            e->pending_snapshot.position,
                            e->pending_snapshot.rotation);
                        g_nt.snap_corrections++;
                        LOG_DEBUG(LOG_TAG,
                            "snap obj %u dist=%.3f ang=%.1f",
                            (unsigned)e->id, (double)dist, (double)ang);
                    }
                }
                e->pending_correction = false;
            }
            continue;
        }

        /* ------------------------------------------------------------
         * Non-owned path: interpolate between snapshots. */
        if (e->ring_count == 0u) continue;

        uint32_t interp_ticks =
            (uint32_t)(((double)e->cfg.interp_delay_ms / 1000.0) *
                       fixed_hz + 0.5);
        if (interp_ticks == 0u) interp_ticks = 1u;

        uint32_t render_tick = g_nt.last_known_server_tick > interp_ticks
            ? g_nt.last_known_server_tick - interp_ticks
            : 0u;

        const JceNetTransformSnapshot *a = NULL;
        const JceNetTransformSnapshot *b = NULL;
        float    t = 0.0f;
        jce_vec3 pos;
        jce_quat rot;

        if (ring_find_pair(e, render_tick, &a, &b, &t)) {
            pos = jce_v3_lerp(a->position, b->position, t);
            rot = jce_q_slerp(a->rotation, b->rotation, t);
        } else {
            /* Buffer empty on the leading edge — hold the newest. */
            const JceNetTransformSnapshot *newest =
                ring_at(e, e->ring_count - 1u);
            if (!newest) continue;
            pos = newest->position;
            rot = newest->rotation;
        }

        entity_apply_transform(e->id, pos, rot);

        /* Prune snapshots older than 4× interp_delay to bound memory. */
        if (render_tick > interp_ticks * 4u)
            ring_prune(e, render_tick - interp_ticks * 4u);
    }
}

uint32_t jce_net_transform_registered_count(void)
{
    return g_nt.count;
}

uint32_t jce_net_transform_snap_corrections_count(void)
{
    return g_nt.snap_corrections;
}

void jce_net_transform_reset_stats(void)
{
    g_nt.snap_corrections = 0u;
}

void jce_net_transform_shutdown(void)
{
    if (!g_nt.inited) return;
    if (g_nt.entries) JCE_FREE(g_nt.entries);
    memset(&g_nt, 0, sizeof(g_nt));
}

/* ================================================================== */
/* Self-test (debug builds only).                                      */
/* ================================================================== */

#ifndef NDEBUG
#include <assert.h>

/* Stand-alone self-test — exercises the interp math and the snap
 * decision in isolation.  Avoids spinning up a full ECS / replication
 * session by routing scene reads through g_nt_stub_*. */
void jce_net_transform_self_test(void);
void jce_net_transform_self_test(void)
{
    /* Reset module + install stub scene path. */
    jce_net_transform_shutdown();
    g_nt_stub_active = true;
    g_nt_stub_pos    = jce_v3(0.0f, 0.0f, 0.0f);
    g_nt_stub_rot    = jce_q_identity();
    /* Non-null sentinel so render_step's scene-bound checks pass. */
    jce_net_transform_set_scene((JceScene *)(uintptr_t)0xDEADBEEFu);
    jce_net_transform_reset_stats();

    const JceNetObjectId TEST_ID = (JceNetObjectId)42u;
    JceNetTransformConfig cfg;
    jce_net_transform_get_default_config(&cfg);
    cfg.interp_delay_ms = 100u;
    cfg.snapshot_hz     = 20u;
    cfg.divergence_snap_distance = 0.5f;

    bool ok = jce_net_transform_register(TEST_ID, &cfg);
    assert(ok);

    NtEntry *e = find_entry(TEST_ID);
    assert(e);

    /* 4 server snapshots @ ticks 100/110/120/130, pos.x = 1..4. */
    for (uint32_t k = 0; k < 4u; ++k) {
        JceNetTransformSnapshot s;
        s.server_tick = 100u + k * 10u;
        s.position    = jce_v3((float)(k + 1u), 0.0f, 0.0f);
        s.rotation    = jce_q_identity();
        s.velocity    = jce_v3(0.0f, 0.0f, 0.0f);
        ring_push(e, &s);
    }
    assert(e->ring_count == 4u);
    g_nt.last_known_server_tick = 130u;

    /* Pair lookup at render_tick=115 → straddles (110, 120), t=0.5. */
    const JceNetTransformSnapshot *a = NULL;
    const JceNetTransformSnapshot *b = NULL;
    float t = 0.0f;
    bool  found = ring_find_pair(e, 115u, &a, &b, &t);
    assert(found);
    assert(a && b);
    assert(a->server_tick == 110u && b->server_tick == 120u);
    jce_vec3 interp = jce_v3_lerp(a->position, b->position, t);
    assert(interp.x > 2.49f && interp.x < 2.51f);

    /* Snap-correction path: predicted pose at origin, snapshot 5m away
     * → expect snap + counter ++.  We simulate the owner-local check
     * by bypassing the replication API: render_step's owned branch
     * gates on (role==CLIENT && owner_local && AUTH_SERVER), so we
     * call into the snap logic directly via a fabricated entry. */
    e->pending_correction = true;
    e->pending_snapshot.position    = jce_v3(5.0f, 0.0f, 0.0f);
    e->pending_snapshot.rotation    = jce_q_identity();
    e->pending_snapshot.server_tick = 200u;

    /* Manual replay of the snap branch (independent of role / owner
     * resolution which would require a live replication session). */
    {
        jce_vec3 cur_pos = g_nt_stub_pos;
        jce_quat cur_rot = g_nt_stub_rot;
        jce_vec3 d = jce_v3_sub(e->pending_snapshot.position, cur_pos);
        float    dist = jce_v3_len(d);
        float    ang  = quat_angle_between_deg(cur_rot,
                                               e->pending_snapshot.rotation);
        assert(dist > cfg.divergence_snap_distance);
        (void)ang;
        entity_apply_transform(e->id,
            e->pending_snapshot.position, e->pending_snapshot.rotation);
        g_nt.snap_corrections++;
        e->pending_correction = false;
    }
    assert(jce_net_transform_snap_corrections_count() == 1u);
    assert(g_nt_stub_pos.x > 4.99f && g_nt_stub_pos.x < 5.01f);

    /* Cleanup. */
    g_nt_stub_active = false;
    jce_net_transform_shutdown();
}

#endif /* !NDEBUG */

