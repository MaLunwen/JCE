/*
 * jce_rpc.c — Remote Procedure Calls (P3-D.2).
 *
 * See jce_rpc.h for the design contract.  Wire format extends the
 * jce_replication packet-type byte (1=snapshot, 2=owner-change,
 * 3=rpc).  Inbound RPC packets are demuxed by the replication
 * dispatcher; the actual decode + handler dispatch lives here.
 *
 * The registry is intentionally simple: a fixed-cap array of
 * RpcEntry { name, desc } with linear scan on lookup.  256 entries is
 * far above the realistic count of distinct RPCs for v1; if a project
 * outgrows it we'll add a hash table behind the same API.
 *
 * Counters are plain uint64_t — the send and receive paths run on the
 * same thread as the rest of the replication subsystem (the net poll
 * loop drives both).  If we ever push net I/O onto a worker thread,
 * swap these for jce_atomic_u64.
 */

#include <jce/middleware/net/jce_rpc.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/os/core/jce_log.h>

#include "jce_net_bytes.h"
#include "jce_session_internal.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "net.rpc"

/* L4-internal seam: jce_replication.c owns the host pointer and
 * exposes these symbols so RPC can transmit on the shared replication
 * channel without reaching into replication's static state.  Not in
 * any public header. */
extern void jce__rpc_transport_broadcast(const void *data, uint32_t size,
                                         JceNetDelivery delivery);
extern bool jce__rpc_transport_send(JcePeerHandle peer,
                                    const void *data, uint32_t size,
                                    JceNetDelivery delivery);

/* On-wire packet type for the replication dispatcher.Mirrors the
 * private JCE_REPL_PKT_SNAPSHOT (1) / JCE_REPL_PKT_OWNER_CHG (2)
 * constants in jce_replication.c. */
#define JCE_REPL_PKT_RPC          ((uint8_t)3)

#define JCE_RPC_REGISTRY_CAP      256u
#define JCE_RPC_NAME_MAX          120u   /* bytes incl. NUL on the wire */

/* ================================================================== */
/* Module state                                                        */
/* ================================================================== */

typedef struct RpcEntry {
    bool         used;
    char         name[JCE_RPC_NAME_MAX];
    JceRpcDesc   desc;
} RpcEntry;

typedef struct RpcState {
    bool      inited;
    RpcEntry  table[JCE_RPC_REGISTRY_CAP];
    uint32_t  count;

    uint64_t  total_sent;
    uint64_t  total_received;
    uint64_t  rejected_authority;
} RpcState;

static RpcState g_rpc;

/* Little-endian wire codec: JceNetWBuf / JceNetRBuf from
 * jce_net_bytes.h — shared with jce_replication.c so both ends of the
 * packet-type-byte protocol encode identically. */

/* ================================================================== */
/* Registry helpers                                                    */
/* ================================================================== */

static RpcEntry *find_entry(const char *name)
{
    if (!name || !*name) return NULL;
    for (uint32_t i = 0; i < JCE_RPC_REGISTRY_CAP; ++i) {
        if (g_rpc.table[i].used && strcmp(g_rpc.table[i].name, name) == 0)
            return &g_rpc.table[i];
    }
    return NULL;
}

static RpcEntry *find_free_slot(void)
{
    for (uint32_t i = 0; i < JCE_RPC_REGISTRY_CAP; ++i)
        if (!g_rpc.table[i].used) return &g_rpc.table[i];
    return NULL;
}

/* ================================================================== */
/* Public — lifecycle + registry                                       */
/* ================================================================== */

void jce_rpc_init(void)
{
    if (g_rpc.inited) return;
    memset(&g_rpc, 0, sizeof(g_rpc));
    g_rpc.inited = true;
    LOG_INFO(LOG_TAG, "rpc initialised (cap=%u)", (unsigned)JCE_RPC_REGISTRY_CAP);
}

void jce_rpc_shutdown(void)
{
    if (!g_rpc.inited) return;
    memset(&g_rpc, 0, sizeof(g_rpc));
}

bool jce_rpc_register(const JceRpcDesc *desc)
{
    if (!desc || !desc->name || !*desc->name || !desc->handler) {
        LOG_WARN(LOG_TAG, "register: bad descriptor (need name + handler)");
        return false;
    }
    if (!g_rpc.inited) jce_rpc_init();

    size_t nlen = strlen(desc->name);
    if (nlen + 1u > JCE_RPC_NAME_MAX) {
        LOG_WARN(LOG_TAG, "register: name '%s' exceeds %u bytes",
                 desc->name, (unsigned)JCE_RPC_NAME_MAX - 1u);
        return false;
    }

    RpcEntry *e = find_entry(desc->name);
    if (!e) {
        e = find_free_slot();
        if (!e) {
            LOG_WARN(LOG_TAG, "register: table full (cap=%u)",
                     (unsigned)JCE_RPC_REGISTRY_CAP);
            return false;
        }
        e->used = true;
        memcpy(e->name, desc->name, nlen + 1u);
        g_rpc.count++;
    }
    e->desc = *desc;
    /* Keep our copy of the name authoritative on lookups. */
    e->desc.name = e->name;
    return true;
}

uint32_t jce_rpc_registered_count(void) { return g_rpc.count; }

uint64_t jce_rpc_total_sent(void)          { return g_rpc.total_sent; }
uint64_t jce_rpc_total_received(void)      { return g_rpc.total_received; }
uint64_t jce_rpc_rejected_authority(void)  { return g_rpc.rejected_authority; }

/* ================================================================== */
/* Send                                                                */
/* ================================================================== */

/* Build the wire packet.  Returns NULL on failure; caller must
 * JCE_FREE the buffer.  `out_size` receives the byte count. */
static uint8_t *encode_packet(JceNetObjectId net_id,
                              const char    *rpc_name,
                              JceClientId    sender,
                              JceRpcTarget   target,
                              JceClientId    specific_client,
                              JceRpcReliability reliability,
                              const void    *payload,
                              uint32_t       payload_size,
                              uint32_t      *out_size)
{
    size_t nlen = strlen(rpc_name);
    if (nlen + 1u > JCE_RPC_NAME_MAX) return NULL;

    JceNetWBuf w = JCE_NET_WBUF_INIT;
    jce_net_w_u8 (&w, JCE_REPL_PKT_RPC);
    jce_net_w_u32(&w, 0u);            /* tick placeholder (v1 unused) */
    jce_net_w_u32(&w, net_id);
    jce_net_w_u16(&w, (uint16_t)nlen);
    jce_net_w_bytes(&w, rpc_name, (uint32_t)nlen);
    jce_net_w_u16(&w, sender);
    jce_net_w_u8 (&w, (uint8_t)target);
    jce_net_w_u16(&w, specific_client);  /* extension over the v1 spec */
    jce_net_w_u8 (&w, (uint8_t)reliability);
    jce_net_w_u32(&w, payload_size);
    if (payload_size && payload) jce_net_w_bytes(&w, payload, payload_size);

    if (!w.ok) { JCE_FREE(w.buf); return NULL; }
    *out_size = w.size;
    return w.buf;
}

/* Forward decls for the TO_NOT_OWNER iterator trampoline used below. */
struct RpcNotOwnerCtx {
    uint8_t        *pkt;
    uint32_t        pkt_size;
    JceNetDelivery  delivery;
    JceClientId     owner;
    uint32_t        sent_count;
};

static void rpc_not_owner_send_cb(JceClientId   id,
                                  JcePeerHandle peer,
                                  void         *user)
{
    struct RpcNotOwnerCtx *ctx = (struct RpcNotOwnerCtx *)user;
    if (id == ctx->owner) return;
    if (jce__rpc_transport_send(peer, ctx->pkt, ctx->pkt_size, ctx->delivery))
        ctx->sent_count++;
}

bool jce_rpc_send(JceNetObjectId net_id,
                  const char    *rpc_name,
                  JceRpcTarget   target,
                  JceClientId    specific_client,
                  const void    *payload,
                  uint32_t       payload_size)
{
    if (!g_rpc.inited) jce_rpc_init();
    if (!rpc_name || !*rpc_name) {
        LOG_WARN(LOG_TAG, "send: missing rpc_name");
        return false;
    }
    RpcEntry *entry = find_entry(rpc_name);
    if (!entry) {
        LOG_WARN(LOG_TAG, "send: unknown rpc '%s'", rpc_name);
        return false;
    }

    JceNetRole  role     = jce_net_replication_role();
    JceClientId local    = jce_net_local_client_id();
    JceClientId sender   = local;

    /* --- Authority pre-check on the send side. ----------------------
     * ServerRpc (TO_SERVER): client must own net_id (we still validate
     * server-side, but failing locally avoids a wasted packet).  The
     * server short-circuits TO_SERVER to a local call.
     * ClientRpc (any other target): must be SERVER with authority. */
    if (target == JCE_RPC_TO_SERVER) {
        if (role == JCE_NET_ROLE_CLIENT) {
            if (!jce_net_object_is_owner_local(net_id)) {
                LOG_WARN(LOG_TAG,
                    "send: ServerRpc '%s' on obj %u rejected — not owner",
                    rpc_name, net_id);
                return false;
            }
        } else if (role != JCE_NET_ROLE_SERVER) {
            LOG_WARN(LOG_TAG, "send: ServerRpc '%s' with no net role", rpc_name);
            return false;
        }
    } else {
        if (role != JCE_NET_ROLE_SERVER) {
            LOG_WARN(LOG_TAG,
                "send: ClientRpc '%s' rejected — only server may invoke",
                rpc_name);
            return false;
        }
        if (!jce_net_object_has_authority(net_id)) {
            LOG_WARN(LOG_TAG,
                "send: ClientRpc '%s' on obj %u rejected — no authority",
                rpc_name, net_id);
            return false;
        }
    }

    g_rpc.total_sent++;

    /* --- Local short-circuit ---------------------------------------
     * Server invoking TO_SERVER on itself, or server broadcasting in a
     * loopback build: run the handler directly so headless tests work
     * without an attached host.  Wire transmission still happens below
     * when a host is present and there are remote recipients. */
    if (target == JCE_RPC_TO_SERVER && role == JCE_NET_ROLE_SERVER) {
        entry->desc.handler(net_id, sender, payload, payload_size,
                            entry->desc.user);
        g_rpc.total_received++;
        return true;
    }

    /* --- Wire path. ------------------------------------------------- */
    /* For headless single-process tests there may be no attached host;
     * still report success — replication has the same behaviour. */

    uint32_t pkt_size = 0;
    uint8_t *pkt = encode_packet(net_id, rpc_name, sender, target,
                                  specific_client, entry->desc.reliability,
                                  payload, payload_size, &pkt_size);
    if (!pkt) {
        LOG_WARN(LOG_TAG, "send: encode failed for '%s'", rpc_name);
        return false;
    }

    JceNetDelivery delivery = (entry->desc.reliability == JCE_RPC_RELIABLE)
        ? JCE_NET_RELIABLE
        : JCE_NET_UNRELIABLE;

    /* --- P3-D.6: targeted sends use the session's client_id -> peer
     * map.  TO_ALL_CLIENTS still broadcasts.  TO_SERVER from a client
     * sends directly to the server peer (broadcast also works because
     * a client only has one connection, but a direct send keeps the
     * intent honest).  TO_OWNER / TO_CLIENT_ID resolve to a single
     * peer; TO_NOT_OWNER fans out one send per non-owner remote peer.
     *
     * If session has the owner registered as the local seat (HOST), we
     * also fire the handler locally so HOST sees its own ClientRpc.
     *
     * Receive-side filtering stays as defence in depth — clients still
     * verify the wire `target` + `specific_client` fields before
     * dispatching the handler. */
    switch ((JceRpcTarget)target) {
    case JCE_RPC_TO_SERVER:
        /* Client -> server: single peer (broadcast also works, but is
         * misleading).  We don't have a per-side seam for "the server
         * peer"; broadcast is functionally equivalent on a client. */
        jce__rpc_transport_broadcast(pkt, pkt_size, delivery);
        break;
    case JCE_RPC_TO_ALL_CLIENTS:
        jce__rpc_transport_broadcast(pkt, pkt_size, delivery);
        /* HOST also runs the handler locally (the local seat counts as
         * "a client" for ClientRpc broadcasts). */
        if (jce__session_is_local_client_internal(jce_net_local_client_id())) {
            entry->desc.handler(net_id, sender, payload, payload_size,
                                entry->desc.user);
            g_rpc.total_received++;
        }
        break;
    case JCE_RPC_TO_OWNER: {
        JceClientId owner = jce_net_object_owner(net_id);
        if (jce__session_is_local_client_internal(owner)) {
            entry->desc.handler(net_id, sender, payload, payload_size,
                                entry->desc.user);
            g_rpc.total_received++;
        } else {
            JcePeerHandle peer;
            if (jce__session_get_peer(owner, &peer)) {
                jce__rpc_transport_send(peer, pkt, pkt_size, delivery);
            } else {
                /* No session — fall back to broadcast + receive-filter. */
                jce__rpc_transport_broadcast(pkt, pkt_size, delivery);
            }
        }
        break;
    }
    case JCE_RPC_TO_CLIENT_ID: {
        if (jce__session_is_local_client_internal((JceClientId)specific_client)) {
            entry->desc.handler(net_id, sender, payload, payload_size,
                                entry->desc.user);
            g_rpc.total_received++;
        } else {
            JcePeerHandle peer;
            if (jce__session_get_peer((JceClientId)specific_client, &peer)) {
                jce__rpc_transport_send(peer, pkt, pkt_size, delivery);
            } else {
                jce__rpc_transport_broadcast(pkt, pkt_size, delivery);
            }
        }
        break;
    }
    case JCE_RPC_TO_NOT_OWNER: {
        JceClientId owner = jce_net_object_owner(net_id);
        struct RpcNotOwnerCtx ctx = { pkt, pkt_size, delivery, owner, 0u };
        jce__session_iter_remote_peers(rpc_not_owner_send_cb, &ctx);
        if (ctx.sent_count == 0u) {
            /* Either no session active or no remote peers — fall back
             * to broadcast so receive-side filtering still delivers. */
            jce__rpc_transport_broadcast(pkt, pkt_size, delivery);
        }
        /* HOST: if HOST seat is not the owner, also run locally. */
        if (!jce__session_is_local_client_internal(owner) &&
            jce__session_is_local_client_internal(jce_net_local_client_id()))
        {
            entry->desc.handler(net_id, sender, payload, payload_size,
                                entry->desc.user);
            g_rpc.total_received++;
        }
        break;
    }
    }

    JCE_FREE(pkt);
    return true;
}

/* ================================================================== */
/* Receive                                                             */
/* ================================================================== */

/* Returns true if this peer should run the handler for an inbound RPC
 * with the given target/specific_client/sender + net_id. */
static bool target_applies_locally(JceRpcTarget target,
                                   JceClientId  specific_client,
                                   JceNetObjectId net_id)
{
    JceNetRole  role  = jce_net_replication_role();
    JceClientId local = jce_net_local_client_id();
    JceClientId owner = jce_net_object_owner(net_id);

    switch (target) {
    case JCE_RPC_TO_SERVER:
        return role == JCE_NET_ROLE_SERVER;
    case JCE_RPC_TO_ALL_CLIENTS:
        return role == JCE_NET_ROLE_CLIENT;
    case JCE_RPC_TO_OWNER:
        return role == JCE_NET_ROLE_CLIENT && local == owner;
    case JCE_RPC_TO_NOT_OWNER:
        return role == JCE_NET_ROLE_CLIENT && local != owner;
    case JCE_RPC_TO_CLIENT_ID:
        return role == JCE_NET_ROLE_CLIENT && local == specific_client;
    default:
        return false;
    }
}

void jce_rpc_handle_packet(const void *data, uint32_t size)
{
    if (!g_rpc.inited || !data || size < 1u) return;

    JceNetRBuf r = JCE_NET_RBUF_INIT(data, size);
    uint8_t type = 0;
    if (!jce_net_r_u8(&r, &type)) return;
    if (type != JCE_REPL_PKT_RPC) return;

    uint32_t tick = 0, net_id = 0, payload_size = 0;
    uint16_t name_len = 0, sender = 0, specific_client = 0;
    uint8_t  target = 0, reliability = 0;
    char     name[JCE_RPC_NAME_MAX];

    if (!jce_net_r_u32(&r, &tick))           return;
    if (!jce_net_r_u32(&r, &net_id))         return;
    if (!jce_net_r_u16(&r, &name_len))       return;
    if (name_len + 1u > JCE_RPC_NAME_MAX) {
        LOG_WARN(LOG_TAG, "recv: rpc_name too long (%u)", (unsigned)name_len);
        return;
    }
    if (!jce_net_r_bytes(&r, name, name_len)) return;
    name[name_len] = '\0';
    if (!jce_net_r_u16(&r, &sender))         return;
    if (!jce_net_r_u8 (&r, &target))         return;
    if (!jce_net_r_u16(&r, &specific_client)) return;
    if (!jce_net_r_u8 (&r, &reliability))    return;
    if (!jce_net_r_u32(&r, &payload_size))   return;

    const void *payload = NULL;
    if (payload_size) {
        if (r.cursor + payload_size > r.size) {
            LOG_WARN(LOG_TAG, "recv: payload truncated (need=%u have=%u)",
                     (unsigned)payload_size, (unsigned)(r.size - r.cursor));
            return;
        }
        payload = r.buf + r.cursor;
    }
    (void)tick;
    (void)reliability;

    RpcEntry *entry = find_entry(name);
    if (!entry) {
        LOG_WARN(LOG_TAG, "recv: unknown rpc '%s'", name);
        return;
    }

    /* --- Receive-side authority gate. ------------------------------- */
    if (entry->desc.server_authoritative) {
        if (jce_net_replication_role() != JCE_NET_ROLE_SERVER) {
            /* A ServerRpc must never run on a client. */
            g_rpc.rejected_authority++;
            return;
        }
        /* ServerRpc + owner check: sender must own net_id. */
        if (jce_net_object_owner(net_id) != sender) {
            LOG_WARN(LOG_TAG,
                "recv: ServerRpc '%s' from client %u on obj %u rejected — not owner (owner=%u)",
                name, (unsigned)sender, net_id,
                (unsigned)jce_net_object_owner(net_id));
            g_rpc.rejected_authority++;
            return;
        }
    }

    if (!target_applies_locally((JceRpcTarget)target,
                                (JceClientId)specific_client, net_id)) {
        /* Packet was broadcast but isn't for us — drop quietly. */
        return;
    }

    g_rpc.total_received++;
    entry->desc.handler(net_id, (JceClientId)sender, payload, payload_size,
                        entry->desc.user);
}

/* ================================================================== */
/* Transport hand-off                                                  */
/* --------------------------------------------------------------------
 * jce_replication.c implements this so the host pointer it already
 * owns can be reused without exposing internal state.  The symbol is
 * intentionally not in the public header — it is an L4-internal seam.
 * ================================================================== */
/* (declared `extern` at the call site in jce_rpc_send.) */

/* ================================================================== */
/* Built-in self-test (debug builds only)                              */
/* ================================================================== */
#ifndef NDEBUG
#include <assert.h>

static int g_st_server_rpc_calls;
static int g_st_client_rpc_calls;
static JceClientId g_st_last_sender;
static char        g_st_last_payload[64];
static uint32_t    g_st_last_payload_size;

static void st_server_handler(JceNetObjectId nid, JceClientId sender,
                              const void *payload, uint32_t payload_size,
                              void *user)
{
    (void)nid; (void)user;
    ++g_st_server_rpc_calls;
    g_st_last_sender = sender;
    g_st_last_payload_size = payload_size;
    if (payload && payload_size && payload_size <= sizeof(g_st_last_payload))
        memcpy(g_st_last_payload, payload, payload_size);
}

static void st_client_handler(JceNetObjectId nid, JceClientId sender,
                              const void *payload, uint32_t payload_size,
                              void *user)
{
    (void)nid; (void)sender; (void)payload; (void)payload_size; (void)user;
    ++g_st_client_rpc_calls;
}

void jce_rpc_self_test(void);
void jce_rpc_self_test(void)
{
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);

    jce_rpc_init();

    /* Register both RPC flavours. */
    JceRpcDesc srv;
    memset(&srv, 0, sizeof(srv));
    srv.name = "selftest.server_rpc";
    srv.reliability = JCE_RPC_RELIABLE;
    srv.server_authoritative = true;
    srv.handler = st_server_handler;
    jce_rpc_register(&srv);

    JceRpcDesc cli;
    memset(&cli, 0, sizeof(cli));
    cli.name = "selftest.client_rpc";
    cli.reliability = JCE_RPC_UNRELIABLE;
    cli.server_authoritative = false;
    cli.handler = st_client_handler;
    jce_rpc_register(&cli);

    assert(jce_rpc_registered_count() == 2);
    assert(find_entry("selftest.server_rpc") != NULL);
    assert(find_entry("selftest.client_rpc") != NULL);

    /* Spawn an object owned by client 1. */
    JceNetObjectDesc d;
    memset(&d, 0, sizeof(d));
    d.owner = (JceClientId)1;
    JceNetObjectId id = jce_net_object_spawn(&d);
    assert(id != JCE_NET_OBJECT_INVALID);

    /* --- Case 1: client 1 sends a ServerRpc, server handler fires. -- */
    g_st_server_rpc_calls = 0;
    g_rpc.rejected_authority = 0;
    {
        const char payload[] = "ping";
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.server_rpc",
                                      /*sender=*/(JceClientId)1,
                                      JCE_RPC_TO_SERVER,
                                      /*specific_client=*/0,
                                      JCE_RPC_RELIABLE,
                                      payload, (uint32_t)sizeof(payload),
                                      &pkt_size);
        assert(pkt && pkt_size > 1);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_server_rpc_calls == 1);
    assert(g_st_last_sender == 1);
    assert(g_st_last_payload_size == 5);
    assert(memcmp(g_st_last_payload, "ping", 5) == 0);
    assert(jce_rpc_rejected_authority() == 0);

    /* --- Case 2: client 2 tries the same RPC, server drops + counts. - */
    g_st_server_rpc_calls = 0;
    {
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.server_rpc",
                                      /*sender=*/(JceClientId)2,
                                      JCE_RPC_TO_SERVER,
                                      /*specific_client=*/0,
                                      JCE_RPC_RELIABLE,
                                      NULL, 0u, &pkt_size);
        assert(pkt && pkt_size > 0);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_server_rpc_calls == 0);
    assert(jce_rpc_rejected_authority() == 1);

    /* --- Case 3: server broadcasts ClientRpc to all clients, but we    *
     *             are the server — broadcast packets only fire on       *
     *             clients.  Simulate the client side by flipping role.  */
    g_st_client_rpc_calls = 0;
    jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
    jce_net_replication_set_local_client_id((JceClientId)1);
    {
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.client_rpc",
                                      /*sender=*/JCE_CLIENT_SERVER,
                                      JCE_RPC_TO_ALL_CLIENTS,
                                      /*specific_client=*/0,
                                      JCE_RPC_UNRELIABLE,
                                      NULL, 0u, &pkt_size);
        assert(pkt && pkt_size > 0);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_client_rpc_calls == 1);

    /* TO_OWNER → only client 1 (current local id) should fire. */
    g_st_client_rpc_calls = 0;
    {
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.client_rpc",
                                      JCE_CLIENT_SERVER,
                                      JCE_RPC_TO_OWNER, 0u,
                                      JCE_RPC_UNRELIABLE,
                                      NULL, 0u, &pkt_size);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_client_rpc_calls == 1);

    /* TO_NOT_OWNER on client 1 (the owner) → drop. */
    g_st_client_rpc_calls = 0;
    {
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.client_rpc",
                                      JCE_CLIENT_SERVER,
                                      JCE_RPC_TO_NOT_OWNER, 0u,
                                      JCE_RPC_UNRELIABLE,
                                      NULL, 0u, &pkt_size);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_client_rpc_calls == 0);

    /* TO_CLIENT_ID matching local — fires. */
    g_st_client_rpc_calls = 0;
    {
        uint32_t pkt_size = 0;
        uint8_t *pkt = encode_packet(id, "selftest.client_rpc",
                                      JCE_CLIENT_SERVER,
                                      JCE_RPC_TO_CLIENT_ID,
                                      /*specific_client=*/1,
                                      JCE_RPC_UNRELIABLE,
                                      NULL, 0u, &pkt_size);
        jce_rpc_handle_packet(pkt, pkt_size);
        JCE_FREE(pkt);
    }
    assert(g_st_client_rpc_calls == 1);

    /* Restore + tear down. */
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);
    jce_net_object_despawn(id);
    jce_net_replication_tick(0u);

    jce_rpc_shutdown();
    jce_net_replication_shutdown();
}
#endif /* NDEBUG */
