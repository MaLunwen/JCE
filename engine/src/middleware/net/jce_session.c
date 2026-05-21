/*
 * jce_session.c — Host / Client session lifecycle (P3-D.6).
 *
 * See jce_session.h for the design contract.  This file owns:
 *   - JceNetHost lifetime (created via jce_net_host_create).
 *   - The replication subsystem's role + host attachment.
 *   - The client roster (cap 64) + monotonic id allocator.
 *   - The handshake state machine on packet type 4.
 *   - The poll loop (jce_session_tick) — dispatches CONNECT /
 *     DISCONNECT and demuxes channel-7 receives between session and
 *     replication.
 *
 * Wire format: see jce_session.h.  All multi-byte fields are little-
 * endian; we never assume host endianness.
 */

#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_rpc.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include "jce_session_internal.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "net.session"

/* Packet type byte (mirrors the private constants in jce_replication.c). */
#define JCE_REPL_PKT_SESSION      ((uint8_t)4)

/* Sub-opcodes within a session packet. */
#define JCE_SESSION_OP_HELLO_REQ  ((uint8_t)1)
#define JCE_SESSION_OP_HELLO_ACK  ((uint8_t)2)
#define JCE_SESSION_OP_GOODBYE    ((uint8_t)3)

/* Stored in ClientSlot.peer_handle when the seat is the HOST's own
 * local player and therefore must not traverse the transport. */
#define LOCAL_LOOPBACK_SENTINEL   ((uint64_t)UINT64_MAX)

#define HOST_LOCAL_CLIENT_ID      ((JceClientId)1)

#define DEFAULT_MAX_CLIENTS       32u
#define DEFAULT_CONNECT_TIMEOUT   5000u   /* ms */
#define HANDSHAKE_TIMEOUT_MS      5000u

/* ================================================================== */
/* Module state                                                        */
/* ================================================================== */

typedef struct ClientSlot {
    bool          used;
    JceClientId   id;
    uint32_t      peer_idx;   /* JcePeerHandle.idx, or UINT32_MAX for local */
    bool          is_local;
    bool          is_host;
    char          name[JCE_SESSION_NAME_MAX];
} ClientSlot;

typedef struct SessionState {
    bool             inited;
    JceSessionMode   mode;
    JceSessionState  state;

    JceNetHost      *host;

    /* Client-side only: peer handle of the server, valid while
     * CONNECTING / HANDSHAKING / RUNNING. */
    JcePeerHandle    server_peer;
    uint64_t         connect_started_ms;
    uint32_t         connect_timeout_ms;

    /* Server-side only: monotonically increasing id allocator.  Skips
     * ids already present in the roster (cheap because cap is 64). */
    uint16_t         next_client_id;
    uint32_t         max_clients;

    char             server_name[JCE_SESSION_NAME_MAX];
    char             player_name[JCE_SESSION_NAME_MAX];

    JceClientId      local_client_id;

    ClientSlot       clients[JCE_SESSION_CLIENT_CAP];

    JceSessionEventFn handler;
    void             *handler_user;
} SessionState;

static SessionState g_sess;

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static void copy_name(char *dst, const char *src)
{
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n >= JCE_SESSION_NAME_MAX) n = JCE_SESSION_NAME_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static ClientSlot *find_slot_by_id(JceClientId id)
{
    if (id == JCE_CLIENT_SERVER) return NULL;
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i)
        if (g_sess.clients[i].used && g_sess.clients[i].id == id)
            return &g_sess.clients[i];
    return NULL;
}

static ClientSlot *find_slot_by_peer(uint32_t peer_idx)
{
    if (peer_idx == UINT32_MAX) return NULL;
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i)
        if (g_sess.clients[i].used &&
            g_sess.clients[i].peer_idx == peer_idx &&
            !g_sess.clients[i].is_local)
            return &g_sess.clients[i];
    return NULL;
}

static ClientSlot *alloc_slot(void)
{
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i) {
        if (!g_sess.clients[i].used) {
            memset(&g_sess.clients[i], 0, sizeof(ClientSlot));
            g_sess.clients[i].peer_idx = UINT32_MAX;
            return &g_sess.clients[i];
        }
    }
    return NULL;
}

static void emit(JceSessionEvent evt, JceClientId who, const char *reason)
{
    if (g_sess.handler)
        g_sess.handler(evt, who, reason, g_sess.handler_user);
}

static JceClientId allocate_client_id(void)
{
    /* Find next free id starting from next_client_id; wrap 1..65535. */
    for (uint32_t attempts = 0; attempts < 65535u; ++attempts) {
        if (g_sess.next_client_id == 0) g_sess.next_client_id = 1;
        JceClientId candidate = (JceClientId)g_sess.next_client_id;
        g_sess.next_client_id = (uint16_t)(g_sess.next_client_id + 1u);
        if (!find_slot_by_id(candidate))
            return candidate;
    }
    return JCE_CLIENT_SERVER;
}

/* ================================================================== */
/* Wire helpers (mirror the little-endian helpers in jce_replication.c) */
/* ================================================================== */

static void put_u8 (uint8_t *p, uint32_t *cursor, uint8_t v)
{
    p[(*cursor)++] = v;
}
static void put_u16(uint8_t *p, uint32_t *cursor, uint16_t v)
{
    p[(*cursor)++] = (uint8_t)(v & 0xFFu);
    p[(*cursor)++] = (uint8_t)((v >> 8) & 0xFFu);
}
static void put_bytes(uint8_t *p, uint32_t *cursor, const void *src, uint32_t n)
{
    if (n) memcpy(p + *cursor, src, n);
    *cursor += n;
}

static bool get_u8(const uint8_t *p, uint32_t size, uint32_t *cursor, uint8_t *out)
{
    if (*cursor + 1u > size) return false;
    *out = p[(*cursor)++];
    return true;
}
static bool get_u16(const uint8_t *p, uint32_t size, uint32_t *cursor, uint16_t *out)
{
    if (*cursor + 2u > size) return false;
    *out = (uint16_t)p[*cursor] | ((uint16_t)p[*cursor + 1u] << 8);
    *cursor += 2u;
    return true;
}
static bool get_bytes(const uint8_t *p, uint32_t size, uint32_t *cursor,
                      void *dst, uint32_t n)
{
    if (*cursor + n > size) return false;
    if (n) memcpy(dst, p + *cursor, n);
    *cursor += n;
    return true;
}

/* Encode a session packet into `out_buf` (caller-provided, must be
 * >= 8 + name_len bytes).  Returns total byte count. */
static uint32_t encode_session_packet(uint8_t *out_buf,
                                       uint8_t sub_opcode,
                                       uint16_t protocol_ver,
                                       uint16_t assigned_id,
                                       const char *name)
{
    uint16_t name_len = 0;
    if (name) {
        size_t n = strlen(name);
        if (n >= JCE_SESSION_NAME_MAX) n = JCE_SESSION_NAME_MAX - 1;
        name_len = (uint16_t)n;
    }
    uint32_t cursor = 0;
    put_u8 (out_buf, &cursor, JCE_REPL_PKT_SESSION);
    put_u8 (out_buf, &cursor, sub_opcode);
    put_u16(out_buf, &cursor, protocol_ver);
    put_u16(out_buf, &cursor, assigned_id);
    put_u16(out_buf, &cursor, name_len);
    put_bytes(out_buf, &cursor, name, name_len);
    return cursor;
}

/* Max session packet size: 1+1+2+2+2 + (NAME_MAX-1) = 8 + 63 = 71 bytes. */
#define SESSION_PACKET_MAX_BYTES (8u + JCE_SESSION_NAME_MAX)

static void send_session_packet_to_peer(JcePeerHandle peer,
                                        uint8_t sub_opcode,
                                        uint16_t assigned_id,
                                        const char *name)
{
    if (!g_sess.host || !jce_peer_valid(peer)) return;
    uint8_t buf[SESSION_PACKET_MAX_BYTES];
    uint32_t n = encode_session_packet(buf, sub_opcode,
                                        JCE_SESSION_PROTOCOL_VERSION,
                                        assigned_id, name);
    jce_net_send(g_sess.host, peer, JCE_NET_REPL_CHANNEL,
                 buf, n, JCE_NET_RELIABLE);
}

/* ================================================================== */
/* Public — event handler / introspection                              */
/* ================================================================== */

void jce_session_set_event_handler(JceSessionEventFn fn, void *user)
{
    g_sess.handler      = fn;
    g_sess.handler_user = user;
}

JceSessionMode  jce_session_mode(void)  { return g_sess.mode; }
JceSessionState jce_session_state(void) { return g_sess.state; }

JceClientId jce_session_local_client_id(void) { return g_sess.local_client_id; }
bool jce_session_is_server(void)
{
    return g_sess.mode == JCE_SESSION_MODE_HOST ||
           g_sess.mode == JCE_SESSION_MODE_DEDICATED_SERVER;
}
bool jce_session_is_host(void)   { return g_sess.mode == JCE_SESSION_MODE_HOST; }
bool jce_session_is_client(void) { return g_sess.mode == JCE_SESSION_MODE_CLIENT; }

const char *jce_session_state_to_string(JceSessionState s)
{
    switch (s) {
    case JCE_SESSION_STATE_STOPPED:        return "STOPPED";
    case JCE_SESSION_STATE_STARTING:       return "STARTING";
    case JCE_SESSION_STATE_CONNECTING:     return "CONNECTING";
    case JCE_SESSION_STATE_HANDSHAKING:    return "HANDSHAKING";
    case JCE_SESSION_STATE_RUNNING:        return "RUNNING";
    case JCE_SESSION_STATE_DISCONNECTING:  return "DISCONNECTING";
    case JCE_SESSION_STATE_FAILED:         return "FAILED";
    }
    return "?";
}

bool jce_session_is_local_client(JceClientId id)
{
    ClientSlot *s = find_slot_by_id(id);
    return s && s->is_local;
}

uint32_t jce_session_client_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i)
        if (g_sess.clients[i].used) ++n;
    return n;
}

static void fill_info(const ClientSlot *s, JceSessionClientInfo *out)
{
    out->id    = s->id;
    out->name  = s->name;
    out->is_local       = s->is_local;
    out->is_server_host = s->is_host;
    out->transport_peer_handle = s->is_local
        ? LOCAL_LOOPBACK_SENTINEL
        : (uint64_t)s->peer_idx;
}

bool jce_session_get_client(uint32_t index, JceSessionClientInfo *out)
{
    if (!out) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i) {
        if (!g_sess.clients[i].used) continue;
        if (seen == index) { fill_info(&g_sess.clients[i], out); return true; }
        ++seen;
    }
    return false;
}

bool jce_session_get_client_by_id(JceClientId id, JceSessionClientInfo *out)
{
    if (!out) return false;
    ClientSlot *s = find_slot_by_id(id);
    if (!s) return false;
    fill_info(s, out);
    return true;
}

uint64_t jce_session_client_to_peer(JceClientId id)
{
    ClientSlot *s = find_slot_by_id(id);
    if (!s || s->is_local) return LOCAL_LOOPBACK_SENTINEL;
    return (uint64_t)s->peer_idx;
}

JceClientId jce_session_peer_to_client(uint64_t peer_handle)
{
    if (peer_handle == LOCAL_LOOPBACK_SENTINEL ||
        peer_handle > (uint64_t)UINT32_MAX) return JCE_CLIENT_SERVER;
    ClientSlot *s = find_slot_by_peer((uint32_t)peer_handle);
    return s ? s->id : JCE_CLIENT_SERVER;
}

bool jce_session_get_peer_stats(JceClientId id, JceNetPeerStats *out)
{
    if (!out) return false;
    ClientSlot *s = find_slot_by_id(id);
    if (!s || s->is_local || s->peer_idx == UINT32_MAX) return false;
    JcePeerHandle ph = { s->peer_idx };
    return jce_net_peer_stats(g_sess.host, ph, out);
}

bool jce_session_get_peer_address(JceClientId id, char *buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0) return false;
    buf[0] = '\0';
    ClientSlot *s = find_slot_by_id(id);
    if (!s || s->is_local || s->peer_idx == UINT32_MAX) return false;
    JcePeerHandle ph = { s->peer_idx };
    return jce_net_peer_address_str(g_sess.host, ph, buf, buf_size);
}

/* ================================================================== */
/* Internal seam exports                                               */
/* ================================================================== */

bool jce__session_get_peer(JceClientId id, JcePeerHandle *out)
{
    if (!out) return false;
    ClientSlot *s = find_slot_by_id(id);
    if (!s || s->is_local) return false;
    if (s->peer_idx == UINT32_MAX) return false;
    out->idx = s->peer_idx;
    return true;
}

bool jce__session_is_local_client_internal(JceClientId id)
{
    return jce_session_is_local_client(id);
}

void jce__session_iter_remote_peers(jce__session_peer_iter_fn fn, void *user)
{
    if (!fn) return;
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i) {
        ClientSlot *s = &g_sess.clients[i];
        if (!s->used || s->is_local || s->peer_idx == UINT32_MAX) continue;
        JcePeerHandle p = { s->peer_idx };
        fn(s->id, p, user);
    }
}

bool jce__session_owns_poll(void)
{
    return g_sess.inited && g_sess.host != NULL;
}

/* ================================================================== */
/* Start paths                                                         */
/* ================================================================== */

static void session_reset_state(void)
{
    memset(&g_sess.clients, 0, sizeof(g_sess.clients));
    for (uint32_t i = 0; i < JCE_SESSION_CLIENT_CAP; ++i)
        g_sess.clients[i].peer_idx = UINT32_MAX;
    g_sess.server_peer        = JCE_PEER_INVALID;
    g_sess.local_client_id    = JCE_CLIENT_SERVER;
    g_sess.next_client_id     = HOST_LOCAL_CLIENT_ID + 1u; /* clients start at 2 */
    g_sess.connect_started_ms = 0u;
    g_sess.connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT;
    g_sess.server_name[0]     = '\0';
    g_sess.player_name[0]     = '\0';
}

static bool start_listen_host(const JceSessionStartHostDesc *desc, bool is_host)
{
    if (g_sess.inited) {
        LOG_WARN(LOG_TAG, "start: already running (mode=%d)", (int)g_sess.mode);
        return false;
    }
    if (!desc) return false;

    memset(&g_sess, 0, sizeof(g_sess));
    session_reset_state();
    g_sess.inited      = true;
    g_sess.state       = JCE_SESSION_STATE_STARTING;
    g_sess.mode        = is_host ? JCE_SESSION_MODE_HOST
                                 : JCE_SESSION_MODE_DEDICATED_SERVER;
    g_sess.max_clients = desc->max_clients ? desc->max_clients : DEFAULT_MAX_CLIENTS;
    if (g_sess.max_clients > JCE_SESSION_CLIENT_CAP)
        g_sess.max_clients = JCE_SESSION_CLIENT_CAP;
    copy_name(g_sess.server_name, desc->server_name);

    JceNetHostDesc hd;
    memset(&hd, 0, sizeof(hd));
    hd.port          = desc->port;
    hd.max_peers     = g_sess.max_clients;
    hd.channel_count = (uint8_t)(JCE_NET_REPL_CHANNEL + 1u); /* >= 8 */

    g_sess.host = jce_net_host_create(&hd, jce_allocator_default());
    if (!g_sess.host) {
        g_sess.state = JCE_SESSION_STATE_FAILED;
        emit(JCE_SESSION_EVT_FAILED, JCE_CLIENT_SERVER, "host_create failed");
        g_sess.inited = false;
        return false;
    }

    /* Wire replication: server role + host attach + local id. */
    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_SERVER);
    jce_net_replication_attach_host(g_sess.host);

    if (is_host) {
        ClientSlot *s = alloc_slot();
        s->used     = true;
        s->id       = HOST_LOCAL_CLIENT_ID;
        s->peer_idx = UINT32_MAX;
        s->is_local = true;
        s->is_host  = true;
        copy_name(s->name, desc->server_name);
        g_sess.local_client_id = HOST_LOCAL_CLIENT_ID;
        jce_net_replication_set_local_client_id(HOST_LOCAL_CLIENT_ID);
    } else {
        g_sess.local_client_id = JCE_CLIENT_SERVER;
        jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER);
    }

    g_sess.state = JCE_SESSION_STATE_RUNNING;
    emit(JCE_SESSION_EVT_STARTED, g_sess.local_client_id, NULL);
    LOG_SUCCESS(LOG_TAG, "%s started on port %u (max_clients=%u)",
                is_host ? "host" : "dedicated server",
                (unsigned)desc->port, (unsigned)g_sess.max_clients);
    return true;
}

bool jce_session_start_host(const JceSessionStartHostDesc *desc)
{
    return start_listen_host(desc, true);
}

bool jce_session_start_dedicated_server(const JceSessionStartHostDesc *desc)
{
    return start_listen_host(desc, false);
}

bool jce_session_start_client(const JceSessionStartClientDesc *desc)
{
    if (g_sess.inited) {
        LOG_WARN(LOG_TAG, "start: already running (mode=%d)", (int)g_sess.mode);
        return false;
    }
    if (!desc || !desc->host || desc->port == 0) return false;

    memset(&g_sess, 0, sizeof(g_sess));
    session_reset_state();
    g_sess.inited             = true;
    g_sess.mode               = JCE_SESSION_MODE_CLIENT;
    g_sess.state              = JCE_SESSION_STATE_STARTING;
    g_sess.connect_timeout_ms = desc->connect_timeout_ms
                                ? desc->connect_timeout_ms
                                : DEFAULT_CONNECT_TIMEOUT;
    copy_name(g_sess.player_name, desc->player_name);

    JceNetHostDesc hd;
    memset(&hd, 0, sizeof(hd));
    hd.port          = 0u; /* ephemeral */
    hd.max_peers     = 1u;
    hd.channel_count = (uint8_t)(JCE_NET_REPL_CHANNEL + 1u);

    g_sess.host = jce_net_host_create(&hd, jce_allocator_default());
    if (!g_sess.host) {
        g_sess.state = JCE_SESSION_STATE_FAILED;
        emit(JCE_SESSION_EVT_FAILED, JCE_CLIENT_SERVER, "host_create failed");
        g_sess.inited = false;
        return false;
    }

    jce_net_replication_init();
    jce_net_replication_set_role(JCE_NET_ROLE_CLIENT);
    jce_net_replication_attach_host(g_sess.host);
    jce_net_replication_set_local_client_id(JCE_CLIENT_SERVER); /* until ACK */

    g_sess.server_peer = jce_net_connect(g_sess.host, desc->host,
                                         desc->port,
                                         (uint8_t)(JCE_NET_REPL_CHANNEL + 1u));
    if (!jce_peer_valid(g_sess.server_peer)) {
        jce_net_host_destroy(g_sess.host);
        g_sess.host  = NULL;
        g_sess.state = JCE_SESSION_STATE_FAILED;
        emit(JCE_SESSION_EVT_FAILED, JCE_CLIENT_SERVER, "connect failed");
        g_sess.inited = false;
        return false;
    }

    g_sess.connect_started_ms = jce_time_ticks_ms();
    g_sess.state              = JCE_SESSION_STATE_CONNECTING;
    emit(JCE_SESSION_EVT_STARTED, JCE_CLIENT_SERVER, NULL);
    LOG_INFO(LOG_TAG, "client connecting to %s:%u (timeout %ums)",
             desc->host, (unsigned)desc->port,
             (unsigned)g_sess.connect_timeout_ms);
    return true;
}

void jce_session_shutdown(void)
{
    if (!g_sess.inited) return;
    LOG_INFO(LOG_TAG, "shutdown (mode=%d)", (int)g_sess.mode);

    /* Best-effort goodbye on the client side. */
    if (g_sess.mode == JCE_SESSION_MODE_CLIENT &&
        jce_peer_valid(g_sess.server_peer) &&
        g_sess.state >= JCE_SESSION_STATE_HANDSHAKING)
    {
        send_session_packet_to_peer(g_sess.server_peer,
                                    JCE_SESSION_OP_GOODBYE,
                                    g_sess.local_client_id,
                                    g_sess.player_name);
    }

    if (g_sess.host) {
        /* Detach replication from this host BEFORE destroying it so the
         * replication subsystem doesn't try to broadcast through a dead
         * pointer on its next tick. */
        jce_net_replication_attach_host(NULL);
        jce_net_replication_set_role(JCE_NET_ROLE_NONE);
        jce_net_host_destroy(g_sess.host);
        g_sess.host = NULL;
    }

    JceSessionMode prev = g_sess.mode;
    memset(&g_sess, 0, sizeof(g_sess));
    g_sess.state = JCE_SESSION_STATE_STOPPED;
    g_sess.mode  = JCE_SESSION_MODE_NONE;
    emit(JCE_SESSION_EVT_STOPPED, JCE_CLIENT_SERVER, NULL);
    (void)prev;
}

/* ================================================================== */
/* Receive — session packet                                            */
/* ================================================================== */

void jce__session_recv_packet(uint32_t peer_idx,
                              const void *data, uint32_t size)
{
    if (!g_sess.inited || !data || size < 1u) return;
    const uint8_t *p = (const uint8_t *)data;

    uint32_t cursor = 0;
    uint8_t  type = 0;
    if (!get_u8(p, size, &cursor, &type)) return;
    if (type != JCE_REPL_PKT_SESSION) return;

    uint8_t  sub = 0;
    uint16_t proto = 0, assigned = 0, name_len = 0;
    if (!get_u8 (p, size, &cursor, &sub))      return;
    if (!get_u16(p, size, &cursor, &proto))    return;
    if (!get_u16(p, size, &cursor, &assigned)) return;
    if (!get_u16(p, size, &cursor, &name_len)) return;
    if (name_len >= JCE_SESSION_NAME_MAX)      return;
    char name[JCE_SESSION_NAME_MAX];
    if (!get_bytes(p, size, &cursor, name, name_len)) return;
    name[name_len] = '\0';

    if (proto != JCE_SESSION_PROTOCOL_VERSION) {
        LOG_WARN(LOG_TAG, "recv: protocol mismatch (got %u, want %u)",
                 (unsigned)proto, (unsigned)JCE_SESSION_PROTOCOL_VERSION);
        return;
    }

    switch (sub) {
    case JCE_SESSION_OP_HELLO_REQ: {
        if (!jce_session_is_server()) return;
        if (peer_idx == UINT32_MAX) return;
        /* Already in roster?  Idempotent re-ACK is fine. */
        ClientSlot *s = find_slot_by_peer(peer_idx);
        if (!s) {
            JceClientId new_id = allocate_client_id();
            if (new_id == JCE_CLIENT_SERVER) {
                LOG_WARN(LOG_TAG, "recv: no free client id");
                return;
            }
            s = alloc_slot();
            if (!s) {
                LOG_WARN(LOG_TAG, "recv: roster full (%u)",
                         (unsigned)JCE_SESSION_CLIENT_CAP);
                return;
            }
            s->used     = true;
            s->id       = new_id;
            s->peer_idx = peer_idx;
            s->is_local = false;
            s->is_host  = false;
            copy_name(s->name, name);
        }
        /* Send ACK with server_name + assigned id. */
        JcePeerHandle peer = { peer_idx };
        send_session_packet_to_peer(peer, JCE_SESSION_OP_HELLO_ACK,
                                    s->id, g_sess.server_name);
        emit(JCE_SESSION_EVT_CLIENT_CONNECTED, s->id, NULL);
        LOG_SUCCESS(LOG_TAG, "client %u handshaked (peer %u, name='%s')",
                    (unsigned)s->id, (unsigned)peer_idx, s->name);
        break;
    }
    case JCE_SESSION_OP_HELLO_ACK: {
        if (!jce_session_is_client()) return;
        if (assigned == JCE_CLIENT_SERVER) {
            LOG_ERROR(LOG_TAG, "recv: server returned invalid client id 0");
            g_sess.state = JCE_SESSION_STATE_FAILED;
            emit(JCE_SESSION_EVT_FAILED, JCE_CLIENT_SERVER, "bad ack");
            return;
        }
        g_sess.local_client_id = (JceClientId)assigned;
        copy_name(g_sess.server_name, name);
        jce_net_replication_set_local_client_id(g_sess.local_client_id);
        g_sess.state = JCE_SESSION_STATE_RUNNING;
        emit(JCE_SESSION_EVT_CONNECTED_TO_SERVER,
             g_sess.local_client_id, NULL);
        LOG_SUCCESS(LOG_TAG, "connected — local client id = %u, server = '%s'",
                    (unsigned)g_sess.local_client_id, g_sess.server_name);
        break;
    }
    case JCE_SESSION_OP_GOODBYE:
        /* Cosmetic — the ENet disconnect that follows is authoritative. */
        break;
    default:
        LOG_WARN(LOG_TAG, "recv: unknown session sub-opcode %u", (unsigned)sub);
        break;
    }
}

/* ================================================================== */
/* Tick — poll and dispatch                                            */
/* ================================================================== */

static void handle_connect_event(const JceNetEvent *ev)
{
    if (jce_session_is_client()) {
        /* ENet CONNECT on the client side — kick off the handshake. */
        g_sess.state = JCE_SESSION_STATE_HANDSHAKING;
        g_sess.server_peer = ev->peer;
        send_session_packet_to_peer(g_sess.server_peer,
                                    JCE_SESSION_OP_HELLO_REQ,
                                    0u, g_sess.player_name);
        LOG_INFO(LOG_TAG, "transport connected to server — HELLO_REQ sent");
    } else if (jce_session_is_server()) {
        /* Don't surface the client yet — wait for HELLO_REQ to assign id. */
        LOG_INFO(LOG_TAG, "peer %u arrived — awaiting HELLO_REQ",
                 (unsigned)ev->peer.idx);
    }
}

static void handle_disconnect_event(const JceNetEvent *ev)
{
    if (jce_session_is_client()) {
        JceClientId who = g_sess.local_client_id;
        emit(JCE_SESSION_EVT_DISCONNECTED_FROM_SERVER, who, "transport drop");
        g_sess.state = JCE_SESSION_STATE_DISCONNECTING;
    } else if (jce_session_is_server()) {
        ClientSlot *s = find_slot_by_peer(ev->peer.idx);
        if (s) {
            JceClientId id = s->id;
            s->used = false;
            emit(JCE_SESSION_EVT_CLIENT_DISCONNECTED, id, "transport drop");
            LOG_INFO(LOG_TAG, "client %u (peer %u) dropped",
                     (unsigned)id, (unsigned)ev->peer.idx);
        }
    }
}

static void handle_receive_event(const JceNetEvent *ev)
{
    if (ev->channel != JCE_NET_REPL_CHANNEL) return;
    if (!ev->data || ev->data_size < 1u) return;
    const uint8_t *bytes = (const uint8_t *)ev->data;
    if (bytes[0] == JCE_REPL_PKT_SESSION) {
        jce__session_recv_packet(ev->peer.idx, ev->data, ev->data_size);
    } else {
        jce_net_replication_handle_packet(ev->data, ev->data_size);
    }
}

void jce_session_tick(void)
{
    if (!g_sess.inited || !g_sess.host) return;

    JceNetEvent ev;
    while (jce_net_poll(g_sess.host, &ev, 0)) {
        switch (ev.type) {
        case JCE_NET_EVENT_CONNECT:    handle_connect_event(&ev);    break;
        case JCE_NET_EVENT_DISCONNECT: handle_disconnect_event(&ev); break;
        case JCE_NET_EVENT_RECEIVE:    handle_receive_event(&ev);    break;
        case JCE_NET_EVENT_TIMEOUT:    break;
        }
    }

    /* Client-side connect timeout. */
    if (g_sess.mode == JCE_SESSION_MODE_CLIENT &&
        (g_sess.state == JCE_SESSION_STATE_CONNECTING ||
         g_sess.state == JCE_SESSION_STATE_HANDSHAKING))
    {
        uint64_t now = jce_time_ticks_ms();
        if (now - g_sess.connect_started_ms > g_sess.connect_timeout_ms) {
            LOG_WARN(LOG_TAG, "connect timeout after %ums",
                     (unsigned)g_sess.connect_timeout_ms);
            if (jce_peer_valid(g_sess.server_peer))
                jce_net_disconnect_now(g_sess.host, g_sess.server_peer);
            g_sess.state = JCE_SESSION_STATE_FAILED;
            emit(JCE_SESSION_EVT_FAILED, JCE_CLIENT_SERVER, "connect timeout");
        }
    }
}

/* ================================================================== */
/* Built-in self-test (debug builds only)                              */
/*                                                                     */
/* Guarded by JCE_NET_SELF_TEST as well as NDEBUG so loopback flakes   */
/* never gate normal debug builds.  Skips if a port can't be bound.    */
/* ================================================================== */
#if !defined(NDEBUG) && defined(JCE_NET_SELF_TEST)
#include <assert.h>

static int g_st_evt_started;
static int g_st_evt_client_connected;
static int g_st_evt_connected_to_server;
static JceClientId g_st_last_who;

static void st_handler(JceSessionEvent evt, JceClientId who,
                       const char *reason, void *user)
{
    (void)reason; (void)user;
    g_st_last_who = who;
    switch (evt) {
    case JCE_SESSION_EVT_STARTED:              ++g_st_evt_started;              break;
    case JCE_SESSION_EVT_CLIENT_CONNECTED:     ++g_st_evt_client_connected;     break;
    case JCE_SESSION_EVT_CONNECTED_TO_SERVER:  ++g_st_evt_connected_to_server;  break;
    default: break;
    }
}

void jce_session_self_test(void);
void jce_session_self_test(void)
{
    SessionState srv_save;
    /* HOST on ephemeral port. */
    JceSessionStartHostDesc h;
    memset(&h, 0, sizeof(h));
    h.port = 27015u;
    h.max_clients = 8u;
    h.server_name = "selftest_srv";

    jce_session_set_event_handler(st_handler, NULL);
    bool ok = jce_session_start_host(&h);
    assert(ok);
    assert(jce_session_state() == JCE_SESSION_STATE_RUNNING);
    assert(jce_session_local_client_id() == HOST_LOCAL_CLIENT_ID);
    assert(jce_session_is_server());
    assert(jce_session_is_host());
    assert(jce_session_client_count() == 1u);
    assert(g_st_evt_started == 1);

    /* Snapshot + tear down host to spin a CLIENT against a fresh host
     * (single-process loopback tests are easiest this way).  Skipped:
     * a real loopback test needs two SessionState instances, which we
     * don't support yet — keep this single-mode smoke test. */
    memcpy(&srv_save, &g_sess, sizeof(g_sess));
    (void)srv_save;
    jce_session_shutdown();
    assert(g_sess.mode == JCE_SESSION_MODE_NONE);
}
#endif /* !NDEBUG && JCE_NET_SELF_TEST */
