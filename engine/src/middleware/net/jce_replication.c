/*
 * jce_replication.c — NetworkObject + component snapshot replication.
 *
 * P3-D.1 — see jce_replication.h for the design contract.  The on-wire
 * format mirrors the spec in the header comment; little-endian writes
 * happen byte-by-byte (we do not assume host endianness even on x86).
 *
 * Design echoes jce_snapshot.c deliberately: a small registry of
 * Provider/CompEntry records, identified by stable string, with a
 * write_fn / read_fn / version / user quartet.  The save framework
 * already solved the "serialize a versioned bag of typed sections"
 * problem; we recycle that mental model on top of the ENet transport.
 */

#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_rpc.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_mem_profile.h>

#include "jce_session_internal.h"
#include "os/core/jce_memory.h"

#include <flecs.h>

#include <math.h>
#include <string.h>

#define LOG_TAG "net.repl"

/* On-wire packet type prefix (1 byte). */
#define JCE_REPL_PKT_SNAPSHOT    ((uint8_t)1)
#define JCE_REPL_PKT_OWNER_CHG   ((uint8_t)2)
#define JCE_REPL_PKT_RPC         ((uint8_t)3)
#define JCE_REPL_PKT_SESSION     ((uint8_t)4)
#define JCE_REPL_PKT_NET_TRANSFORM ((uint8_t)5)
#define JCE_REPL_PKT_NET_TRANSFORM_V2 ((uint8_t)6)
#define JCE_REPL_PKT_ACK         ((uint8_t)8)   /* client -> server tick ack */

/* P3-D.4 transform replication seam — defined in jce_net_transform.c.
 * Declared here (not in any public header) so the dispatcher can route
 * packet types 5 / 6 without pulling the transform module into the
 * public surface of replication. */
extern void jce__net_transform_recv_packet(const void *data, uint32_t size);

#define JCE_REPL_EVENT_LISTENER_CAP 16

/* ================================================================== */
/* Module state                                                        */
/* ================================================================== */

typedef struct ObjectEntry {
    JceNetObjectId id;
    JceClientId    owner;
    uint16_t       flags;
    uint64_t       entity;       /* ecs_entity_t cast to u64           */
    char          *prefab_path;  /* heap copy; NULL for code-spawned   */
    bool           pending_spawn;
    bool           pending_despawn;
    bool           pending_owner_change; /* server queues to broadcast */
} ObjectEntry;

typedef struct CompEntry {
    char             *name;
    uint16_t          name_len;   /* cached strlen(name), set at registration */
    uint32_t          version;
    uint64_t          flecs_component_id;
    uint32_t          size;
    JceNetCompWriteFn write;
    JceNetCompReadFn  read;
    void             *user;
} CompEntry;

typedef struct EventListener {
    uint32_t            handle;
    JceNetObjectEventFn fn;
    void               *user;
} EventListener;

/* ── Acked-baseline delta state (P1-networking-full) ──────────────────
 *
 * For each (object, component) pair the server caches the LAST PAYLOAD
 * it sent so the next encode can diff against it and skip unchanged
 * entries.  The baseline is keyed by net id + component index; we keep a
 * flat array and linear-scan (object * comp counts are O(100)).  When a
 * peer is at fault (joined late / acked nothing) the late-joiner burst
 * resets the baseline by forcing a full send for that peer.
 *
 * NOTE: v1 used a single broadcast stream so a true PER-PEER baseline is
 * not free.  We keep ONE authoritative baseline (the last broadcast) and
 * track per-peer "needs full burst" + last-acked-tick separately; the
 * common-case stream stays a single delta broadcast, and a freshly
 * joined peer gets its own reliable full burst first. */
#define JCE_REPL_BASELINE_PAYLOAD_MAX 256u

typedef struct BaselineEntry {
    JceNetObjectId id;
    uint16_t       comp_index;
    uint16_t       size;
    bool           valid;
    uint8_t        payload[JCE_REPL_BASELINE_PAYLOAD_MAX];
} BaselineEntry;

/* jce_session caps the roster at 64; mirror it here (we only need the
 * count for the table, not the session public header). */
#define JCE_REPL_PEER_CAP 64u

typedef struct PeerState {
    bool       used;
    uint32_t   peer_idx;        /* JcePeerHandle.idx */
    JceClientId client;
    bool       needs_full_burst; /* late joiner — send baseline once    */
    JceNetTick last_acked_tick;  /* most recent snapshot the peer acked  */
} PeerState;

typedef struct ReplState {
    bool             inited;
    JceNetRole       role;
    ecs_world_t     *world;
    JceNetHost      *host;

    ObjectEntry     *objects;
    uint32_t         object_count;
    uint32_t         object_cap;
    JceNetObjectId   next_id;          /* server-side allocator        */

    CompEntry       *comps;
    uint32_t         comp_count;
    uint32_t         comp_cap;

    JceNetTick       last_tick_sent;
    JceNetTick       last_tick_received;

    /* P3-D.3 — ownership + events. */
    JceClientId      local_client_id;
    ecs_entity_t     net_obj_comp_id;  /* cached id for JceNetworkObjectComponent */
    EventListener    listeners[JCE_REPL_EVENT_LISTENER_CAP];
    uint32_t         next_listener_handle;

    /* P1-networking-full — acked-baseline delta + interest + peers. */
    BaselineEntry   *baselines;
    uint32_t         baseline_count;
    uint32_t         baseline_cap;
    PeerState        peers[JCE_REPL_PEER_CAP];
    float            interest_radius_sq;   /* 0 = disabled */
    ecs_entity_t     transform_comp_id;    /* cached JceTransform id */
    uint64_t         comp_entries_sent;    /* changed entries shipped */
} ReplState;

static ReplState g_repl;

/* ================================================================== */
/* Internal helpers — object table                                     */
/* ================================================================== */

static ObjectEntry *find_object(JceNetObjectId id)
{
    if (id == JCE_NET_OBJECT_INVALID) return NULL;
    for (uint32_t i = 0; i < g_repl.object_count; ++i)
        if (g_repl.objects[i].id == id) return &g_repl.objects[i];
    return NULL;
}

static ObjectEntry *object_table_push(void)
{
    if (g_repl.object_count == g_repl.object_cap) {
        uint32_t nc = g_repl.object_cap ? g_repl.object_cap * 2u : 16u;
        ObjectEntry *nb = (ObjectEntry *)JCE_REALLOC(g_repl.objects,
                                                     sizeof(ObjectEntry) * nc);
        if (!nb) return NULL;
        g_repl.objects   = nb;
        g_repl.object_cap = nc;
    }
    ObjectEntry *e = &g_repl.objects[g_repl.object_count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void object_table_remove(uint32_t idx)
{
    if (idx >= g_repl.object_count) return;
    JCE_FREE(g_repl.objects[idx].prefab_path);
    for (uint32_t i = idx + 1; i < g_repl.object_count; ++i)
        g_repl.objects[i - 1] = g_repl.objects[i];
    g_repl.object_count--;
}

/* ================================================================== */
/* Internal helpers — component registry                               */
/* ================================================================== */

static CompEntry *find_comp_by_name(const char *name)
{
    for (uint32_t i = 0; i < g_repl.comp_count; ++i)
        if (strcmp(g_repl.comps[i].name, name) == 0) return &g_repl.comps[i];
    return NULL;
}

/* P1-networking-full — index <-> component for u16 interning on the
 * wire.  Registration order is deterministic on both peers (the engine
 * registers replicated components in a fixed order at runtime bring-up),
 * so an index is a stable, name-free key. */
static CompEntry *find_comp_by_index(uint16_t idx)
{
    return idx < g_repl.comp_count ? &g_repl.comps[idx] : NULL;
}

/* Resolve a component's flecs id lazily from its registered name when the
 * descriptor was registered with flecs_component_id == 0.  This lets L6
 * callers (the runtime) register replicated scene components without
 * pulling flecs into their TU — they pass name + size + write/read and we
 * bind the flecs id from the attached world (mirrors the net-obj lookup). */
static uint64_t comp_flecs_id(CompEntry *cd)
{
    if (cd->flecs_component_id || !cd->name || !g_repl.world)
        return cd->flecs_component_id;
    ecs_entity_t id = ecs_lookup(g_repl.world, cd->name);
    if (id) cd->flecs_component_id = (uint64_t)id;
    return cd->flecs_component_id;
}

/* ================================================================== */
/* Internal helpers — baseline (acked delta) + peers + interest        */
/* ================================================================== */

static BaselineEntry *baseline_find(JceNetObjectId id, uint16_t comp_index)
{
    for (uint32_t i = 0; i < g_repl.baseline_count; ++i) {
        BaselineEntry *b = &g_repl.baselines[i];
        if (b->id == id && b->comp_index == comp_index) return b;
    }
    return NULL;
}

static BaselineEntry *baseline_get_or_push(JceNetObjectId id, uint16_t comp_index)
{
    BaselineEntry *b = baseline_find(id, comp_index);
    if (b) return b;
    if (g_repl.baseline_count == g_repl.baseline_cap) {
        uint32_t nc = g_repl.baseline_cap ? g_repl.baseline_cap * 2u : 32u;
        BaselineEntry *nb = (BaselineEntry *)JCE_REALLOC(
            g_repl.baselines, sizeof(BaselineEntry) * nc);
        if (!nb) return NULL;
        g_repl.baselines    = nb;
        g_repl.baseline_cap = nc;
    }
    b = &g_repl.baselines[g_repl.baseline_count++];
    memset(b, 0, sizeof(*b));
    b->id         = id;
    b->comp_index = comp_index;
    return b;
}

/* Drop every baseline row referencing `id` (object despawned). */
static void baseline_drop_object(JceNetObjectId id)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < g_repl.baseline_count; ++i) {
        if (g_repl.baselines[i].id == id) continue;
        if (w != i) g_repl.baselines[w] = g_repl.baselines[i];
        ++w;
    }
    g_repl.baseline_count = w;
}

static PeerState *peer_find(uint32_t peer_idx)
{
    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i)
        if (g_repl.peers[i].used && g_repl.peers[i].peer_idx == peer_idx)
            return &g_repl.peers[i];
    return NULL;
}

static PeerState *peer_get_or_add(uint32_t peer_idx, JceClientId client)
{
    PeerState *p = peer_find(peer_idx);
    if (p) { p->client = client; return p; }
    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i) {
        if (!g_repl.peers[i].used) {
            p = &g_repl.peers[i];
            memset(p, 0, sizeof(*p));
            p->used             = true;
            p->peer_idx         = peer_idx;
            p->client           = client;
            p->needs_full_burst = true;   /* late joiner — burst first */
            return p;
        }
    }
    return NULL;
}

static ecs_entity_t transform_comp_id(void)
{
    if (g_repl.transform_comp_id || !g_repl.world)
        return g_repl.transform_comp_id;
    g_repl.transform_comp_id = ecs_lookup(g_repl.world, "JceTransform");
    return g_repl.transform_comp_id;
}

/* Read an entity's local position (x,y,z) from its JceTransform.  We do
 * NOT include jce_scene.h — the struct begins with `jce_vec3 position`
 * (3 contiguous floats), which is all interest filtering needs. */
static bool object_local_pos(const ObjectEntry *e, float out[3])
{
    if (!g_repl.world || !e || !e->entity) return false;
    ecs_entity_t cid = transform_comp_id();
    if (!cid) return false;
    const void *p = ecs_get_id(g_repl.world, (ecs_entity_t)e->entity,
                               (ecs_id_t)cid);
    if (!p) return false;
    memcpy(out, p, sizeof(float) * 3u);
    return true;
}

/* Per-peer interest origin = position of the first object the peer's
 * client owns (the player's avatar).  Returns false if the peer owns no
 * positioned object (then we do NOT filter — everything is relevant). */
static bool peer_interest_origin(JceClientId client, float out[3])
{
    for (uint32_t i = 0; i < g_repl.object_count; ++i) {
        ObjectEntry *e = &g_repl.objects[i];
        if (e->owner != client || e->pending_despawn) continue;
        if (object_local_pos(e, out)) return true;
    }
    return false;
}

static bool object_relevant_to_origin(const ObjectEntry *e,
                                      const float origin[3])
{
    if (g_repl.interest_radius_sq <= 0.0f) return true;
    float p[3];
    if (!object_local_pos(e, p)) return true;  /* no pos -> always send */
    float dx = p[0] - origin[0];
    float dy = p[1] - origin[1];
    float dz = p[2] - origin[2];
    return (dx * dx + dy * dy + dz * dz) <= g_repl.interest_radius_sq;
}

/* ================================================================== */
/* Little-endian byte writers / readers                                */
/* ================================================================== */

typedef struct WBuf {
    uint8_t *buf;
    uint32_t size;
    uint32_t cap;
    bool     ok;
} WBuf;

static void wbuf_reserve(WBuf *w, uint32_t need)
{
    if (!w->ok) return;
    if (w->size + need <= w->cap) return;
    uint32_t nc = w->cap ? w->cap : 256u;
    while (nc < w->size + need) nc *= 2u;
    uint8_t *nb = (uint8_t *)JCE_REALLOC(w->buf, nc);
    if (!nb) { w->ok = false; return; }
    w->buf = nb;
    w->cap = nc;
}
static void w_bytes(WBuf *w, const void *p, uint32_t n)
{
    wbuf_reserve(w, n);
    if (!w->ok) return;
    memcpy(w->buf + w->size, p, n);
    w->size += n;
}
static void w_u8 (WBuf *w, uint8_t  v) { w_bytes(w, &v, 1); }
static void w_u16(WBuf *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
    w_bytes(w, b, 2);
}
static void w_u32(WBuf *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v        & 0xFFu),
                     (uint8_t)((v >> 8) & 0xFFu),
                     (uint8_t)((v >> 16)& 0xFFu),
                     (uint8_t)((v >> 24)& 0xFFu) };
    w_bytes(w, b, 4);
}

typedef struct RBuf {
    const uint8_t *buf;
    uint32_t       size;
    uint32_t       cursor;
    bool           ok;
} RBuf;

static bool r_bytes(RBuf *r, void *dst, uint32_t n)
{
    if (!r->ok || r->cursor + n > r->size) { r->ok = false; return false; }
    memcpy(dst, r->buf + r->cursor, n);
    r->cursor += n;
    return true;
}
static bool r_u16(RBuf *r, uint16_t *out)
{
    uint8_t b[2];
    if (!r_bytes(r, b, 2)) return false;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}
static bool r_u32(RBuf *r, uint32_t *out)
{
    uint8_t b[4];
    if (!r_bytes(r, b, 4)) return false;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

/* ================================================================== */
/* Internal — event listeners + ECS sync (P3-D.3)                      */
/* ================================================================== */

static void fire_event(JceNetObjectEvent ev, JceNetObjectId id, JceClientId owner)
{
    for (uint32_t i = 0; i < JCE_REPL_EVENT_LISTENER_CAP; ++i) {
        if (g_repl.listeners[i].fn)
            g_repl.listeners[i].fn(ev, id, owner, g_repl.listeners[i].user);
    }
}

static ecs_entity_t net_obj_comp_id(void)
{
    if (g_repl.net_obj_comp_id || !g_repl.world) return g_repl.net_obj_comp_id;
    /* ECS_COMPONENT_DEFINE in jce_scene.c registers an entity named
     * after the type — look it up lazily, once the world is attached. */
    g_repl.net_obj_comp_id = ecs_lookup(g_repl.world, "JceNetworkObjectComponent");
    if (!g_repl.net_obj_comp_id)
        LOG_WARN(LOG_TAG, "JceNetworkObjectComponent not registered on world");
    return g_repl.net_obj_comp_id;
}

static void sync_net_obj_component(ObjectEntry *e)
{
    if (!g_repl.world || !e || !e->entity) return;
    ecs_entity_t cid = net_obj_comp_id();
    if (!cid) return;

    JceNetworkObjectComponent comp;
    memset(&comp, 0, sizeof(comp));
    comp.net_id   = e->id;
    comp.owner    = e->owner;
    comp.flags    = e->flags;
    comp.is_owner = (e->owner == g_repl.local_client_id);

    void *slot = ecs_ensure_id(g_repl.world, (ecs_entity_t)e->entity,
                               cid, sizeof(JceNetworkObjectComponent));
    if (!slot) return;
    memcpy(slot, &comp, sizeof(comp));
    ecs_modified_id(g_repl.world, (ecs_entity_t)e->entity, cid);
}

static void remove_net_obj_component(ObjectEntry *e)
{
    if (!g_repl.world || !e || !e->entity) return;
    ecs_entity_t cid = net_obj_comp_id();
    if (!cid) return;
    if (ecs_is_alive(g_repl.world, (ecs_entity_t)e->entity))
        ecs_remove_id(g_repl.world, (ecs_entity_t)e->entity, cid);
}

/* ================================================================== */
/* Public — role / init / shutdown                                     */
/* ================================================================== */

JceNetRole jce_net_replication_role(void)        { return g_repl.role; }
void       jce_net_replication_set_role(JceNetRole r) { g_repl.role = r; }

void jce_net_replication_init(void)
{
    if (g_repl.inited) return;
    memset(&g_repl, 0, sizeof(g_repl));
    g_repl.inited               = true;
    g_repl.role                 = JCE_NET_ROLE_NONE;
    g_repl.next_id              = 1u;
    g_repl.local_client_id      = JCE_CLIENT_SERVER;
    g_repl.next_listener_handle = 1u;
    LOG_INFO(LOG_TAG, "replication initialised");
}

void jce_net_replication_shutdown(void)
{
    if (!g_repl.inited) return;
    for (uint32_t i = 0; i < g_repl.object_count; ++i)
        JCE_FREE(g_repl.objects[i].prefab_path);
    JCE_FREE(g_repl.objects);
    for (uint32_t i = 0; i < g_repl.comp_count; ++i)
        JCE_FREE(g_repl.comps[i].name);
    JCE_FREE(g_repl.comps);
    JCE_FREE(g_repl.baselines);
    memset(&g_repl, 0, sizeof(g_repl));
}

void jce_net_replication_set_world(void *ecs_world)
{
    g_repl.world             = (ecs_world_t *)ecs_world;
    g_repl.net_obj_comp_id   = 0;  /* re-resolve lazily on next use */
    g_repl.transform_comp_id = 0;  /* re-resolve lazily on next use */
}

void jce_net_replication_attach_host(JceNetHost *host) { g_repl.host = host; }

/* ================================================================== */
/* Public — NetworkObject                                              */
/* ================================================================== */

static char *strdup_jce(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)JCE_MALLOC(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

JceNetObjectId jce_net_object_spawn(const JceNetObjectDesc *desc)
{
    if (!g_repl.inited || !desc) return JCE_NET_OBJECT_INVALID;
    if (g_repl.role != JCE_NET_ROLE_SERVER) {
        LOG_WARN(LOG_TAG, "spawn() called on non-server role");
        return JCE_NET_OBJECT_INVALID;
    }

    ObjectEntry *e = object_table_push();
    if (!e) return JCE_NET_OBJECT_INVALID;

    e->id      = g_repl.next_id++;
    e->owner   = desc->owner;
    e->flags   = desc->flags;
    e->prefab_path  = strdup_jce(desc->prefab_path);
    e->pending_spawn = true;

    if (g_repl.world)
        e->entity = (uint64_t)ecs_new(g_repl.world);

    sync_net_obj_component(e);
    fire_event(JCE_NETOBJ_SPAWNED, e->id, e->owner);

    LOG_INFO(LOG_TAG, "spawn obj %u (owner=%u, entity=%llu)",
             e->id, (unsigned)e->owner, (unsigned long long)e->entity);
    return e->id;
}

JceNetObjectId jce_net_object_adopt(uint64_t entity, JceClientId owner,
                                    uint16_t flags, const char *prefab_path)
{
    if (!g_repl.inited || !entity) return JCE_NET_OBJECT_INVALID;
    if (g_repl.role != JCE_NET_ROLE_SERVER) {
        LOG_WARN(LOG_TAG, "adopt() called on non-server role");
        return JCE_NET_OBJECT_INVALID;
    }

    /* Already adopted?  Return the existing id (idempotent). */
    JceNetObjectId existing = jce_net_object_from_entity(entity);
    if (existing != JCE_NET_OBJECT_INVALID) return existing;

    ObjectEntry *e = object_table_push();
    if (!e) return JCE_NET_OBJECT_INVALID;

    e->id            = g_repl.next_id++;
    e->owner         = owner;
    e->flags         = flags;
    e->prefab_path   = strdup_jce(prefab_path);
    e->pending_spawn = true;
    e->entity        = entity;   /* bind the AUTHORED entity, do not ecs_new */

    sync_net_obj_component(e);
    fire_event(JCE_NETOBJ_SPAWNED, e->id, e->owner);

    LOG_INFO(LOG_TAG, "adopt obj %u (owner=%u, entity=%llu)",
             e->id, (unsigned)e->owner, (unsigned long long)e->entity);
    return e->id;
}

void jce_net_object_despawn(JceNetObjectId id)
{
    if (!g_repl.inited) return;
    ObjectEntry *e = find_object(id);
    if (!e) return;

    /* Local teardown happens immediately; despawn message goes out on
     * the next tick.  We keep the entry around until then so the
     * outbound packet can name the id. */
    if (g_repl.world && e->entity) {
        remove_net_obj_component(e);
        ecs_delete(g_repl.world, (ecs_entity_t)e->entity);
    }
    e->entity = 0;
    e->pending_despawn = true;
    fire_event(JCE_NETOBJ_DESPAWNED, e->id, e->owner);
    LOG_INFO(LOG_TAG, "despawn obj %u", id);
}

JceClientId jce_net_object_owner(JceNetObjectId id)
{
    ObjectEntry *e = find_object(id);
    return e ? e->owner : JCE_CLIENT_SERVER;
}

uint64_t jce_net_object_to_entity(JceNetObjectId id)
{
    ObjectEntry *e = find_object(id);
    return e ? e->entity : 0u;
}

JceNetObjectId jce_net_object_from_entity(uint64_t entity)
{
    for (uint32_t i = 0; i < g_repl.object_count; ++i)
        if (g_repl.objects[i].entity == entity) return g_repl.objects[i].id;
    return JCE_NET_OBJECT_INVALID;
}

uint32_t jce_net_object_count(void) { return g_repl.object_count; }

/* ================================================================== */
/* Public — component registry                                         */
/* ================================================================== */

void jce_net_replication_register_component(const JceNetCompDesc *desc)
{
    if (!g_repl.inited || !desc || !desc->name) return;

    CompEntry *existing = find_comp_by_name(desc->name);
    if (existing) {
        existing->version            = desc->version;
        existing->flecs_component_id = desc->flecs_component_id;
        existing->size               = desc->size;
        existing->write              = desc->write;
        existing->read               = desc->read;
        existing->user               = desc->user;
        return;
    }
    if (g_repl.comp_count == g_repl.comp_cap) {
        uint32_t nc = g_repl.comp_cap ? g_repl.comp_cap * 2u : 8u;
        CompEntry *nb = (CompEntry *)JCE_REALLOC(g_repl.comps,
                                                 sizeof(CompEntry) * nc);
        if (!nb) return;
        g_repl.comps   = nb;
        g_repl.comp_cap = nc;
    }
    CompEntry *e = &g_repl.comps[g_repl.comp_count++];
    memset(e, 0, sizeof(*e));
    e->name               = strdup_jce(desc->name);
    e->name_len           = e->name ? (uint16_t)strlen(e->name) : 0;
    e->version            = desc->version;
    e->flecs_component_id = desc->flecs_component_id;
    e->size               = desc->size;
    e->write              = desc->write;
    e->read               = desc->read;
    e->user               = desc->user;
    LOG_INFO(LOG_TAG, "registered comp '%s' v%u", desc->name, desc->version);
}

uint32_t jce_net_replication_component_count(void) { return g_repl.comp_count; }

void jce_net_replication_set_interest_radius(float radius_m)
{
    g_repl.interest_radius_sq = (radius_m > 0.0f) ? radius_m * radius_m : 0.0f;
}

float jce_net_replication_get_interest_radius(void)
{
    float sq = g_repl.interest_radius_sq;
    return sq > 0.0f ? (float)sqrt((double)sq) : 0.0f;
}

uint64_t jce_net_replication_comp_entries_sent(void)
{
    return g_repl.comp_entries_sent;
}

/* ================================================================== */
/* Snapshot encode (server)                                            */
/* ================================================================== */

/* Per-tick scratch — reused across ticks to avoid allocator churn. */
static uint8_t *g_scratch     = NULL;
static uint32_t g_scratch_cap = 0;

static uint8_t *scratch_get(uint32_t want)
{
    if (want > g_scratch_cap) {
        uint32_t nc = g_scratch_cap ? g_scratch_cap : 1024u;
        while (nc < want) nc *= 2u;
        uint8_t *nb = (uint8_t *)JCE_REALLOC(g_scratch, nc);
        if (!nb) return NULL;
        g_scratch     = nb;
        g_scratch_cap = nc;
    }
    return g_scratch;
}

/* Snapshot flags (header `flags` field). */
#define JCE_REPL_SNAP_FLAG_FULL  ((uint16_t)0x0001u)  /* full-state burst */

/* Serialise one entity's component into `scratch`; returns byte count or
 * 0 (skip).  Shared by the delta broadcast + late-joiner burst. */
static int comp_serialize(ObjectEntry *e, CompEntry *cd, uint8_t **out_scratch)
{
    uint64_t fid = comp_flecs_id(cd);
    if (!cd->write || !fid) return 0;
    const void *cptr = ecs_get_id(g_repl.world, (ecs_entity_t)e->entity,
                                  (ecs_id_t)fid);
    if (!cptr) return 0;
    uint8_t *scratch = scratch_get(cd->size > 0 ? cd->size : 1024u);
    if (!scratch) return 0;
    int n = cd->write(scratch, cd->size ? cd->size : g_scratch_cap,
                      cptr, cd->user);
    if (n <= 0) return 0;
    *out_scratch = scratch;
    return n;
}

/* Write the spawn + despawn blocks (common to delta + burst).  For a
 * FULL burst `force_all_spawn` re-spawns EVERY live object regardless of
 * the pending flag so a late joiner learns the whole world. */
static void write_spawn_despawn(WBuf *w, bool force_all_spawn,
                                uint16_t *out_spawn_n, uint16_t *out_desp_n)
{
    uint16_t spawn_n = 0, desp_n = 0;
    for (uint32_t i = 0; i < g_repl.object_count; ++i) {
        ObjectEntry *e = &g_repl.objects[i];
        if (e->pending_despawn) { if (!force_all_spawn) desp_n++; continue; }
        if (force_all_spawn || e->pending_spawn) spawn_n++;
    }
    *out_spawn_n = spawn_n;
    *out_desp_n = desp_n;
}

/* Encode a snapshot for ONE recipient.  When `interest_origin` is non-
 * NULL the per-tick component stream is interest-filtered against it.
 * `full` forces a full-state burst (every object, every component,
 * ignores baseline).  Returns the encoded WBuf (caller frees). */
static void encode_snapshot_for(WBuf *w, JceNetTick tick, bool full,
                                const float *interest_origin)
{
    w_u8 (w, JCE_REPL_PKT_SNAPSHOT);
    w_u32(w, tick);
    w_u32(w, g_repl.last_tick_received);

    uint16_t spawn_n = 0, desp_n = 0;
    write_spawn_despawn(w, full, &spawn_n, &desp_n);

    /* comp_n is patched after we know how many entries we wrote. */
    uint32_t comp_n_off = w->size;
    w_u16(w, spawn_n);
    w_u16(w, desp_n);
    w_u16(w, 0u);  /* comp_n placeholder (offset comp_n_off + 4) */
    w_u16(w, full ? JCE_REPL_SNAP_FLAG_FULL : 0u);

    /* Spawns. */
    for (uint32_t i = 0; i < g_repl.object_count; ++i) {
        ObjectEntry *e = &g_repl.objects[i];
        if (e->pending_despawn) continue;
        if (!full && !e->pending_spawn) continue;
        w_u32(w, e->id);
        w_u16(w, e->owner);
        w_u16(w, e->flags);
        uint16_t plen = e->prefab_path ? (uint16_t)strlen(e->prefab_path) : 0u;
        w_u16(w, plen);
        if (plen) w_bytes(w, e->prefab_path, plen);
    }
    /* Despawns (never in a full burst — a burst describes a fresh world). */
    if (!full) {
        for (uint32_t i = 0; i < g_repl.object_count; ++i) {
            ObjectEntry *e = &g_repl.objects[i];
            if (!e->pending_despawn) continue;
            w_u32(w, e->id);
        }
    }

    /* Component entries: delta (vs baseline) for the stream, or every
     * component for a burst.  Interest filter drops far objects. */
    uint16_t written = 0;
    if (g_repl.world) {
        for (uint32_t i = 0; i < g_repl.object_count; ++i) {
            ObjectEntry *e = &g_repl.objects[i];
            if (e->pending_despawn || !e->entity) continue;
            if (interest_origin && !object_relevant_to_origin(e, interest_origin))
                continue;
            for (uint32_t c = 0; c < g_repl.comp_count; ++c) {
                CompEntry *cd = &g_repl.comps[c];
                uint8_t *scratch = NULL;
                int n = comp_serialize(e, cd, &scratch);
                if (n <= 0) continue;

                if (!full) {
                    /* Delta: skip if identical to the last broadcast and
                     * update the baseline when changed. */
                    BaselineEntry *b = baseline_get_or_push(e->id, (uint16_t)c);
                    bool changed = true;
                    if (b && b->valid && b->size == (uint16_t)n &&
                        (uint32_t)n <= JCE_REPL_BASELINE_PAYLOAD_MAX &&
                        memcmp(b->payload, scratch, (size_t)n) == 0) {
                        changed = false;
                    }
                    if (!changed) continue;
                    if (b && (uint32_t)n <= JCE_REPL_BASELINE_PAYLOAD_MAX) {
                        memcpy(b->payload, scratch, (size_t)n);
                        b->size  = (uint16_t)n;
                        b->valid = true;
                    }
                }

                w_u32(w, e->id);
                w_u16(w, (uint16_t)c);     /* interned component index */
                w_u32(w, cd->version);
                w_u32(w, (uint32_t)n);
                w_bytes(w, scratch, (uint32_t)n);
                written++;
                g_repl.comp_entries_sent++;
            }
        }
    }
    /* Patch comp_n. */
    if (w->ok && w->size >= comp_n_off + 4u) {
        w->buf[comp_n_off + 4u] = (uint8_t)( written        & 0xFFu);
        w->buf[comp_n_off + 5u] = (uint8_t)((written >> 8u) & 0xFFu);
    }
}

/* Refresh the peer table from the session roster: add freshly-connected
 * peers (flagged needs_full_burst) and drop ones that left. */
struct PeerSweep { bool seen[JCE_REPL_PEER_CAP]; };

static void peer_sweep_cb(JceClientId id, JcePeerHandle peer, void *user)
{
    struct PeerSweep *sw = (struct PeerSweep *)user;
    PeerState *p = peer_get_or_add(peer.idx, id);
    if (!p) return;
    p->client = id;
    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i)
        if (&g_repl.peers[i] == p) { sw->seen[i] = true; break; }
}

static void refresh_peers_from_session(void)
{
    if (!jce__session_owns_poll()) return;   /* no session -> no roster */
    struct PeerSweep sw;
    memset(&sw, 0, sizeof(sw));
    jce__session_iter_remote_peers(peer_sweep_cb, &sw);
    /* Drop peers that left the roster. */
    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i)
        if (g_repl.peers[i].used && !sw.seen[i])
            memset(&g_repl.peers[i], 0, sizeof(g_repl.peers[i]));
}

static void encode_and_broadcast(JceNetTick tick)
{
    if (!g_repl.host) return;

    /* Bring the peer table in step with the live roster.  New peers get
     * a one-shot reliable full burst before they join the delta stream;
     * this is the late-joiner spawn sync. */
    refresh_peers_from_session();

    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i) {
        PeerState *p = &g_repl.peers[i];
        if (!p->used || !p->needs_full_burst) continue;
        WBuf w = { NULL, 0, 0, true };
        encode_snapshot_for(&w, tick, /*full=*/true, /*interest=*/NULL);
        if (w.ok && w.size > 0) {
            JcePeerHandle ph = { p->peer_idx };
            jce_mem_profile_record_alloc(JCE_MEM_TAG_NETWORK, w.size);
            jce_net_send(g_repl.host, ph, JCE_NET_REPL_CHANNEL,
                         w.buf, w.size, JCE_NET_RELIABLE);
            jce_mem_profile_record_free(JCE_MEM_TAG_NETWORK, w.size);
            LOG_INFO(LOG_TAG, "late-join burst -> client %u (peer %u, %u B)",
                     (unsigned)p->client, (unsigned)p->peer_idx,
                     (unsigned)w.size);
        }
        JCE_FREE(w.buf);
        p->needs_full_burst = false;
    }

    /* Per-peer delta stream when interest management is on; otherwise a
     * single shared delta broadcast (cheaper, identical to everyone).
     * NOTE: with a shared broadcast the baseline is authoritative for the
     * whole audience; with per-peer interest we still update ONE baseline
     * (the union of what was sent) which can re-send an unchanged entry
     * when it re-enters a peer's radius — acceptable + self-healing. */
    if (g_repl.interest_radius_sq > 0.0f && jce__session_owns_poll()) {
        uint32_t sent = 0;
        for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i) {
            PeerState *p = &g_repl.peers[i];
            if (!p->used) continue;
            float origin[3];
            const float *use_origin =
                peer_interest_origin(p->client, origin) ? origin : NULL;
            WBuf w = { NULL, 0, 0, true };
            encode_snapshot_for(&w, tick, /*full=*/false, use_origin);
            if (w.ok && w.size > 0) {
                JcePeerHandle ph = { p->peer_idx };
                jce_mem_profile_record_alloc(JCE_MEM_TAG_NETWORK, w.size);
                jce_net_send(g_repl.host, ph, JCE_NET_REPL_CHANNEL,
                             w.buf, w.size, JCE_NET_UNRELIABLE);
                jce_mem_profile_record_free(JCE_MEM_TAG_NETWORK, w.size);
                sent++;
            }
            JCE_FREE(w.buf);
        }
        (void)sent;
    } else {
        WBuf w = { NULL, 0, 0, true };
        encode_snapshot_for(&w, tick, /*full=*/false, /*interest=*/NULL);
        if (w.ok && w.size > 0) {
            jce_mem_profile_record_alloc(JCE_MEM_TAG_NETWORK, w.size);
            jce_net_broadcast(g_repl.host, JCE_NET_REPL_CHANNEL,
                              w.buf, w.size, JCE_NET_UNRELIABLE);
            jce_mem_profile_record_free(JCE_MEM_TAG_NETWORK, w.size);
        }
        JCE_FREE(w.buf);
    }
}

/* ================================================================== */
/* Snapshot decode (client)                                            */
/* ================================================================== */

static ObjectEntry *ensure_client_object(JceNetObjectId id,
                                         JceClientId owner,
                                         uint16_t flags,
                                         const char *prefab_path)
{
    ObjectEntry *e = find_object(id);
    if (e) return e;
    e = object_table_push();
    if (!e) return NULL;
    e->id          = id;
    e->owner       = owner;
    e->flags       = flags;
    e->prefab_path = strdup_jce(prefab_path);
    if (g_repl.world)
        e->entity = (uint64_t)ecs_new(g_repl.world);
    if (g_repl.next_id <= id) g_repl.next_id = id + 1u;
    sync_net_obj_component(e);
    fire_event(JCE_NETOBJ_SPAWNED, e->id, e->owner);
    return e;
}

/* Snapshot decode (client) — assumes type byte already consumed. */
static void decode_snapshot(RBuf *r)
{
    uint32_t tick = 0, ack_tick = 0;
    uint16_t spawn_n = 0, desp_n = 0, comp_n = 0, flags = 0;
    if (!r_u32(r, &tick))     return;
    if (!r_u32(r, &ack_tick)) return;
    if (!r_u16(r, &spawn_n))  return;
    if (!r_u16(r, &desp_n))   return;
    if (!r_u16(r, &comp_n))   return;
    if (!r_u16(r, &flags))    return;
    (void)ack_tick; (void)flags;
    g_repl.last_tick_received = tick;

    /* Spawns. */
    for (uint16_t i = 0; i < spawn_n; ++i) {
        uint32_t id; uint16_t owner, fl, plen;
        if (!r_u32(r, &id))    return;
        if (!r_u16(r, &owner)) return;
        if (!r_u16(r, &fl))    return;
        if (!r_u16(r, &plen))  return;
        char path[512];
        uint16_t copy = plen < (uint16_t)(sizeof(path) - 1) ? plen
                                                            : (uint16_t)(sizeof(path) - 1);
        if (plen) {
            if (!r_bytes(r, path, copy)) return;
            /* Skip overflow tail. */
            if (plen > copy) r->cursor += (uint32_t)(plen - copy);
        }
        path[copy] = '\0';
        ensure_client_object(id, owner, fl, plen ? path : NULL);
    }
    /* Despawns. */
    for (uint16_t i = 0; i < desp_n; ++i) {
        uint32_t id;
        if (!r_u32(r, &id)) return;
        for (uint32_t k = 0; k < g_repl.object_count; ++k) {
            if (g_repl.objects[k].id != id) continue;
            ObjectEntry *e = &g_repl.objects[k];
            JceClientId own = e->owner;
            if (g_repl.world && e->entity) {
                remove_net_obj_component(e);
                ecs_delete(g_repl.world, (ecs_entity_t)e->entity);
            }
            fire_event(JCE_NETOBJ_DESPAWNED, id, own);
            object_table_remove(k);
            break;
        }
    }
    /* Component deltas.  v2 wire interns the component name to a u16
     * index (registration order is identical on both peers). */
    for (uint16_t i = 0; i < comp_n; ++i) {
        uint32_t id; uint16_t comp_index; uint32_t ver, payload_sz;
        if (!r_u32(r, &id))         return;
        if (!r_u16(r, &comp_index)) return;
        if (!r_u32(r, &ver))        return;
        if (!r_u32(r, &payload_sz)) return;
        if (r->cursor + payload_sz > r->size) return;
        const uint8_t *payload = r->buf + r->cursor;
        r->cursor += payload_sz;

        CompEntry *cd = find_comp_by_index(comp_index);
        ObjectEntry *e = find_object(id);
        if (!cd || !cd->read || !e || !g_repl.world || !e->entity) continue;
        uint64_t fid = comp_flecs_id(cd);
        if (!fid) continue;
        (void)ver; /* TODO: version negotiation */

        void *slot = ecs_ensure_id(g_repl.world,
                                   (ecs_entity_t)e->entity,
                                   (ecs_id_t)fid,
                                   (size_t)(cd->size ? cd->size : payload_sz));
        if (!slot) continue;
        if (cd->read(payload, payload_sz, slot, cd->user) <= 0) continue;
        ecs_modified_id(g_repl.world,
                        (ecs_entity_t)e->entity,
                        (ecs_id_t)fid);
    }

    /* Ack the snapshot back to the server so it can bound its retransmit
     * / baseline reset window.  The client has exactly one peer (the
     * server), so a broadcast on the repl channel reaches it.  Skipped on
     * a FULL burst is unnecessary — acking the burst tick is also useful. */
    if (g_repl.role == JCE_NET_ROLE_CLIENT && g_repl.host) {
        WBuf a = { NULL, 0, 0, true };
        w_u8 (&a, JCE_REPL_PKT_ACK);
        w_u32(&a, tick);
        w_u16(&a, (uint16_t)g_repl.local_client_id);
        w_u16(&a, 0u); /* reserved */
        if (a.ok && a.size > 0)
            jce_net_broadcast(g_repl.host, JCE_NET_REPL_CHANNEL,
                              a.buf, a.size, JCE_NET_UNRELIABLE);
        JCE_FREE(a.buf);
    }
}

/* Ack decode (server).  Wire: tick u32, client u16, reserved u16.
 * Records the most recent tick a client acknowledged so future work can
 * cap retransmit windows / baseline staleness per peer. */
static void decode_ack(RBuf *r)
{
    uint32_t acked_tick = 0;
    uint16_t client = 0, reserved = 0;
    if (!r_u32(r, &acked_tick)) return;
    if (!r_u16(r, &client))     return;
    if (!r_u16(r, &reserved))   return;
    (void)reserved;
    if (g_repl.role != JCE_NET_ROLE_SERVER) return;
    for (uint32_t i = 0; i < JCE_REPL_PEER_CAP; ++i) {
        PeerState *p = &g_repl.peers[i];
        if (p->used && p->client == (JceClientId)client) {
            if (acked_tick > p->last_acked_tick)
                p->last_acked_tick = acked_tick;
            break;
        }
    }
}

/* Ownership-change decode (clients).  Wire: tick u32, net_id u32,
 * new_owner u16, reserved u16. */
static void decode_owner_change(RBuf *r)
{
    uint32_t tick = 0, id = 0;
    uint16_t new_owner = 0, reserved = 0;
    if (!r_u32(r, &tick))      return;
    if (!r_u32(r, &id))        return;
    if (!r_u16(r, &new_owner)) return;
    if (!r_u16(r, &reserved))  return;
    (void)tick; (void)reserved;

    ObjectEntry *e = find_object(id);
    if (!e) return;
    if (e->owner == new_owner) return;
    e->owner = (JceClientId)new_owner;
    sync_net_obj_component(e);
    fire_event(JCE_NETOBJ_OWNER_CHANGED, e->id, e->owner);
    LOG_INFO(LOG_TAG, "owner-change obj %u -> client %u",
             e->id, (unsigned)e->owner);
}

void jce_net_replication_handle_packet(const void *data, uint32_t size)
{
    if (!g_repl.inited || !data || size < 1u) return;
    RBuf r = { (const uint8_t *)data, size, 0u, true };

    uint8_t type = 0;
    {
        uint8_t b;
        if (!r_bytes(&r, &b, 1)) return;
        type = b;
    }
    switch (type) {
    case JCE_REPL_PKT_SNAPSHOT:  decode_snapshot(&r);     break;
    case JCE_REPL_PKT_ACK:       decode_ack(&r);          break;
    case JCE_REPL_PKT_OWNER_CHG: decode_owner_change(&r); break;
    case JCE_REPL_PKT_RPC:
        /* Re-feed the full packet (incl. type byte) to the RPC
         * dispatcher — it re-parses the type to stay independent. */
        jce_rpc_handle_packet(data, size);
        break;
    case JCE_REPL_PKT_SESSION:
        /* P3-D.6 — session handshake.  We don't know the sender peer
         * here (this entry point is peer-less for back-compat); session
         * handles peer-less injection by treating it as "local" — only
         * matters server-side and the session tick poll loop is the
         * primary feed path. */
        jce__session_recv_packet(UINT32_MAX, data, size);
        break;
    case JCE_REPL_PKT_NET_TRANSFORM:
    case JCE_REPL_PKT_NET_TRANSFORM_V2:
        /* P3-D.4 / P4-D.3 — full transform packet (incl. type byte)
         * handed to the transform module's decoder, which inspects the
         * type byte to select legacy (v1, 44B/entry) vs quantized
         * (v2, 26B/entry) decoding.  Mirrors the RPC path. */
        jce__net_transform_recv_packet(data, size);
        break;
    default:
        LOG_WARN(LOG_TAG, "unknown packet type %u (size=%u)",
                 (unsigned)type, (unsigned)size);
        break;
    }
}

/* ================================================================== */
/* Public — ownership + authority (P3-D.3)                             */
/* ================================================================== */

JceClientId jce_net_local_client_id(void)
{
    return g_repl.local_client_id;
}

void jce_net_replication_set_local_client_id(JceClientId id)
{
    if (g_repl.local_client_id == id) return;
    g_repl.local_client_id = id;
    /* Refresh is_owner across the table — local identity changed. */
    for (uint32_t i = 0; i < g_repl.object_count; ++i)
        sync_net_obj_component(&g_repl.objects[i]);
}

bool jce_net_object_is_owner_local(JceNetObjectId id)
{
    ObjectEntry *e = find_object(id);
    if (!e) return false;
    return e->owner == g_repl.local_client_id;
}

bool jce_net_object_has_authority(JceNetObjectId id)
{
    if (g_repl.role == JCE_NET_ROLE_SERVER) return true;
    return jce_net_object_is_owner_local(id);
}

bool jce_net_object_set_owner(JceNetObjectId id, JceClientId new_owner)
{
    if (!g_repl.inited) return false;
    if (g_repl.role != JCE_NET_ROLE_SERVER) {
        LOG_WARN(LOG_TAG, "set_owner() called on non-server role");
        return false;
    }
    ObjectEntry *e = find_object(id);
    if (!e) return false;
    if (e->owner == new_owner) return true;

    e->owner                = new_owner;
    e->pending_owner_change = true;
    sync_net_obj_component(e);
    fire_event(JCE_NETOBJ_OWNER_CHANGED, e->id, e->owner);
    LOG_INFO(LOG_TAG, "set_owner obj %u -> client %u",
             e->id, (unsigned)new_owner);
    return true;
}

void jce_net_object_iterate(JceNetObjectIterFn fn, void *user)
{
    if (!fn) return;
    for (uint32_t i = 0; i < g_repl.object_count; ++i) {
        ObjectEntry *e = &g_repl.objects[i];
        if (e->pending_despawn) continue;
        fn(e->id, e->owner, user);
    }
}

uint32_t jce_net_object_add_event_listener(JceNetObjectEventFn fn, void *user)
{
    if (!g_repl.inited || !fn) return 0;
    for (uint32_t i = 0; i < JCE_REPL_EVENT_LISTENER_CAP; ++i) {
        if (g_repl.listeners[i].fn == NULL) {
            uint32_t h = g_repl.next_listener_handle++;
            if (h == 0) h = g_repl.next_listener_handle++;
            g_repl.listeners[i].handle = h;
            g_repl.listeners[i].fn     = fn;
            g_repl.listeners[i].user   = user;
            return h;
        }
    }
    LOG_WARN(LOG_TAG, "event listener cap reached (%d)",
             (int)JCE_REPL_EVENT_LISTENER_CAP);
    return 0;
}

void jce_net_object_remove_event_listener(uint32_t handle)
{
    if (!handle) return;
    for (uint32_t i = 0; i < JCE_REPL_EVENT_LISTENER_CAP; ++i) {
        if (g_repl.listeners[i].handle == handle) {
            g_repl.listeners[i].handle = 0;
            g_repl.listeners[i].fn     = NULL;
            g_repl.listeners[i].user   = NULL;
            return;
        }
    }
}

/* Broadcast queued ownership-change packets.  Reliable so late
 * joiners + dropped frames cannot leave clients believing in a stale
 * owner.  Called from the server-side tick before pending flags are
 * cleared. */
static void broadcast_owner_changes(JceNetTick tick)
{
    if (!g_repl.host) return;
    for (uint32_t i = 0; i < g_repl.object_count; ++i) {
        ObjectEntry *e = &g_repl.objects[i];
        if (!e->pending_owner_change) continue;
        WBuf w = { NULL, 0, 0, true };
        w_u8 (&w, JCE_REPL_PKT_OWNER_CHG);
        w_u32(&w, tick);
        w_u32(&w, e->id);
        w_u16(&w, e->owner);
        w_u16(&w, 0u); /* reserved */
        if (w.ok && w.size > 0) {
            jce_net_broadcast(g_repl.host, JCE_NET_REPL_CHANNEL,
                              w.buf, w.size, JCE_NET_RELIABLE);
        }
        JCE_FREE(w.buf);
        e->pending_owner_change = false;
    }
}

/* ================================================================== */
/* Tick                                                                */
/* ================================================================== */

void jce_net_replication_tick(JceNetTick tick)
{
    if (!g_repl.inited) return;

    if (g_repl.role == JCE_NET_ROLE_SERVER && g_repl.host) {
        broadcast_owner_changes(tick);
        encode_and_broadcast(tick);
        g_repl.last_tick_sent = tick;

        /* Clear pending flags / sweep despawned entries. */
        for (uint32_t i = 0; i < g_repl.object_count;) {
            ObjectEntry *e = &g_repl.objects[i];
            e->pending_spawn = false;
            if (e->pending_despawn) {
                baseline_drop_object(e->id);   /* don't carry stale deltas */
                object_table_remove(i);
                continue;
            }
            ++i;
        }
    }

    /* Client-side packet drain: poll the attached host on the
     * replication channel.  Other channels are left to the existing
     * jce_net consumer.  When the P3-D.6 session subsystem is active,
     * it owns the poll loop (and routes channel-7 packets back through
     * jce_net_replication_handle_packet) — skip here to avoid stealing
     * events from it. */
    if (g_repl.host && g_repl.role == JCE_NET_ROLE_CLIENT &&
        !jce__session_owns_poll()) {
        JceNetEvent ev;
        while (jce_net_poll(g_repl.host, &ev, 0)) {
            if (ev.type == JCE_NET_EVENT_RECEIVE &&
                ev.channel == JCE_NET_REPL_CHANNEL) {
                jce_net_replication_handle_packet(ev.data, ev.data_size);
            }
        }
    }
}

/* ================================================================== */
/* L4-internal transport hand-off for jce_rpc.c                        */
/*                                                                     */
/* Exposed via an extern declaration in jce_rpc.c (NOT in any public   */
/* header).  RPC owns its packet encoding but reuses the replication   */
/* host + channel so we don't fragment the transport surface.          */
/* ================================================================== */

void jce__rpc_transport_broadcast(const void *data, uint32_t size,
                                  JceNetDelivery delivery);
void jce__rpc_transport_broadcast(const void *data, uint32_t size,
                                  JceNetDelivery delivery)
{
    if (!g_repl.host || !data || !size) return;
    jce_net_broadcast(g_repl.host, JCE_NET_REPL_CHANNEL,
                      data, size, delivery);
}

bool jce__rpc_transport_send(JcePeerHandle peer,
                             const void *data, uint32_t size,
                             JceNetDelivery delivery);
bool jce__rpc_transport_send(JcePeerHandle peer,
                             const void *data, uint32_t size,
                             JceNetDelivery delivery)
{
    if (!g_repl.host || !data || !size) return false;
    return jce_net_send(g_repl.host, peer, JCE_NET_REPL_CHANNEL,
                        data, size, delivery);
}

/* ================================================================== */
/* Built-in self-test (debug builds only)                              */
/* ================================================================== */
#ifndef NDEBUG
#include <assert.h>

static int self_write(void *dst, uint32_t cap, const void *c, void *u)
{
    (void)u;
    if (cap < 4) return 0;
    memcpy(dst, c, 4);
    return 4;
}
static int self_read(const void *src, uint32_t sz, void *c, void *u)
{
    (void)u;
    if (sz < 4) return 0;
    memcpy(c, src, 4);
    return 4;
}

static int g_selftest_events;
static JceNetObjectEvent g_selftest_last_event;
static JceNetObjectId    g_selftest_last_id;
static JceClientId       g_selftest_last_owner;

static void self_event(JceNetObjectEvent ev, JceNetObjectId nid,
                       JceClientId owner, void *user)
{
    (void)user;
    ++g_selftest_events;
    g_selftest_last_event = ev;
    g_selftest_last_id    = nid;
    g_selftest_last_owner = owner;
}

void jce_net_replication_self_test(void);
void jce_net_replication_self_test(void)
{
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);
    assert(jce_net_local_client_id() == JCE_CLIENT_SERVER);

    JceNetCompDesc cd;
    memset(&cd, 0, sizeof(cd));
    cd.name    = "selftest_comp";
    cd.version = 1;
    cd.size    = 4;
    cd.write   = self_write;
    cd.read    = self_read;
    jce_net_replication_register_component(&cd);
    assert(jce_net_replication_component_count() >= 1);

    g_selftest_events = 0;
    uint32_t lh = jce_net_object_add_event_listener(self_event, NULL);
    assert(lh != 0);

    JceNetObjectDesc d;
    memset(&d, 0, sizeof(d));
    d.prefab_path = "test/prefab";
    d.owner       = JCE_CLIENT_SERVER;
    JceNetObjectId id = jce_net_object_spawn(&d);
    assert(id != JCE_NET_OBJECT_INVALID);
    assert(jce_net_object_owner(id) == JCE_CLIENT_SERVER);
    assert(jce_net_object_from_entity(jce_net_object_to_entity(id)) == id
           || jce_net_object_to_entity(id) == 0u);
    assert(g_selftest_events >= 1);
    assert(g_selftest_last_event == JCE_NETOBJ_SPAWNED);
    assert(g_selftest_last_id == id);

    /* Authority: server has authority over everything. */
    assert(jce_net_object_has_authority(id));

    /* Ownership transfer to client #5. */
    bool ok = jce_net_object_set_owner(id, (JceClientId)5);
    assert(ok);
    assert(jce_net_object_owner(id) == 5);
    assert(g_selftest_last_event == JCE_NETOBJ_OWNER_CHANGED);
    assert(g_selftest_last_owner == 5);
    /* Server still has authority regardless of owner. */
    assert(jce_net_object_has_authority(id));

    /* Despawn fires event. */
    int before = g_selftest_events;
    jce_net_object_despawn(id);
    jce_net_replication_tick(0u);
    assert(jce_net_object_count() == 0u);
    assert(g_selftest_events > before);
    assert(g_selftest_last_event == JCE_NETOBJ_DESPAWNED);

    jce_net_object_remove_event_listener(lh);
    jce_net_replication_shutdown();
}
#endif /* NDEBUG */
