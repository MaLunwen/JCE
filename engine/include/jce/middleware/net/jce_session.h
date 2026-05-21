/*
 * jce_session.h — Host / Client session lifecycle (P3-D.6).
 *
 * Unity-NetworkManager-style lifecycle on top of jce_net + jce_replication:
 *
 *     jce_session_start_host(...)              ─┐
 *     jce_session_start_dedicated_server(...)   ├─►  one of three modes
 *     jce_session_start_client(...)            ─┘    (mutually exclusive)
 *     jce_session_shutdown()                          tears everything down
 *
 * What this layer owns:
 *   - The single JceNetHost (created via jce_net_host_create) and the
 *     replication subsystem's attach/role wiring on top of it.
 *   - The client roster (fixed cap 64) mapping
 *         JceClientId  <->  JcePeerHandle  + display name + flags
 *   - The handshake on a new packet type JCE_REPL_PKT_SESSION (= 4)
 *     riding the existing replication channel (JCE_NET_REPL_CHANNEL = 7).
 *   - Pumping the host (jce_session_tick()): poll, dispatch
 *     CONNECT/DISCONNECT, route channel-7 packets to either the session
 *     dispatcher (packet type 4) or the replication dispatcher (1/2/3).
 *
 * What this layer does NOT own:
 *   - The byte-level transport — that stays in jce_net_*.
 *   - The on-wire snapshot / RPC encoding — that stays in jce_replication
 *     and jce_rpc.  Session only adds packet type 4 + targeted send.
 *   - Lobby / matchmaking — out of scope.
 *
 * Handshake (little-endian on the wire, channel = JCE_NET_REPL_CHANNEL,
 * delivery = JCE_NET_RELIABLE):
 *
 *   [ packet_type    : u8  = 4 (JCE_REPL_PKT_SESSION) ]
 *   [ sub_opcode     : u8  ]   1 = HELLO_REQ  (client -> server)
 *                              2 = HELLO_ACK  (server -> client)
 *                              3 = GOODBYE    (either way, optional)
 *   [ protocol_ver   : u16 ]   == JCE_SESSION_PROTOCOL_VERSION
 *   [ assigned_id    : u16 ]   0 in HELLO_REQ; server's allocation in ACK
 *   [ name_len       : u16 ]   bytes of `name` (0..63)
 *   [ name           : bytes ] player_name in REQ; server_name in ACK
 *
 * Sequence:
 *   1. Client TCP-like connects via ENet.  enet emits CONNECT on both sides.
 *   2. The CLIENT immediately sends HELLO_REQ with its player_name.
 *   3. The SERVER allocates a new JceClientId (monotonic uint16, skipping
 *      ids already in the roster — HOST always reserves id 1 for itself),
 *      adds the client to the roster, and replies with HELLO_ACK carrying
 *      the assigned id and the server_name.  CONNECTED event fires server-
 *      side now (not on CONNECT — we don't surface a client until the
 *      handshake completes).
 *   4. The CLIENT receives HELLO_ACK, sets its local_client_id (also wired
 *      into jce_replication via jce_net_replication_set_local_client_id),
 *      transitions to RUNNING, and fires CONNECTED_TO_SERVER.
 *   5. Either side may send GOODBYE before a normal ENet disconnect (it's
 *      cosmetic — the disconnect alone is sufficient).
 *
 * HOST mode short-circuits this: the local player is registered as
 * client_id = 1 at start() time, no transport packets exchanged.  The
 * loopback peer handle stored in the roster is the sentinel
 * UINT64_MAX; jce_session_is_local_client() lets RPC / replication
 * skip the wire for self-targeted messages.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 */

#ifndef JCE_SESSION_H
#define JCE_SESSION_H

#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SESSION_PROTOCOL_VERSION ((uint16_t)1)
#define JCE_SESSION_NAME_MAX         64u
#define JCE_SESSION_CLIENT_CAP       64u

typedef enum JceSessionMode {
    JCE_SESSION_MODE_NONE = 0,
    JCE_SESSION_MODE_HOST,
    JCE_SESSION_MODE_DEDICATED_SERVER,
    JCE_SESSION_MODE_CLIENT
} JceSessionMode;

typedef enum JceSessionState {
    JCE_SESSION_STATE_STOPPED = 0,
    JCE_SESSION_STATE_STARTING,
    JCE_SESSION_STATE_CONNECTING,
    JCE_SESSION_STATE_HANDSHAKING,
    JCE_SESSION_STATE_RUNNING,
    JCE_SESSION_STATE_DISCONNECTING,
    JCE_SESSION_STATE_FAILED
} JceSessionState;

typedef struct JceSessionStartHostDesc {
    uint16_t    port;            /* 0 → ephemeral (test-only)            */
    uint32_t    max_clients;     /* default 32, hard cap JCE_SESSION_CLIENT_CAP */
    const char *server_name;     /* display name; may be NULL            */
} JceSessionStartHostDesc;

typedef struct JceSessionStartClientDesc {
    const char *host;              /* "127.0.0.1", "example.com", ...    */
    uint16_t    port;
    uint32_t    connect_timeout_ms; /* default 5000                      */
    const char *player_name;        /* display name; may be NULL         */
} JceSessionStartClientDesc;

typedef struct JceSessionClientInfo {
    JceClientId id;
    const char *name;                    /* points into roster storage   */
    bool        is_local;                /* HOST's local seat            */
    bool        is_server_host;          /* HOST seat flag (id == 1)     */
    uint64_t    transport_peer_handle;   /* JcePeerHandle.idx, or
                                          * UINT64_MAX for local loopback */
} JceSessionClientInfo;

typedef enum JceSessionEvent {
    JCE_SESSION_EVT_STARTED = 0,             /* host / server / client  */
    JCE_SESSION_EVT_CLIENT_CONNECTED,        /* server-side, post-handshake */
    JCE_SESSION_EVT_CLIENT_DISCONNECTED,     /* server-side */
    JCE_SESSION_EVT_CONNECTED_TO_SERVER,     /* client-side, post-handshake */
    JCE_SESSION_EVT_DISCONNECTED_FROM_SERVER,/* client-side */
    JCE_SESSION_EVT_STOPPED,
    JCE_SESSION_EVT_FAILED
} JceSessionEvent;

/* Event callback.  `who` is the affected client id (the connecting /
 * disconnecting peer, or the local client id for STARTED/STOPPED on the
 * client side; 0 if not applicable).  `reason` may be NULL. */
typedef void (*JceSessionEventFn)(JceSessionEvent evt,
                                  JceClientId     who,
                                  const char     *reason,
                                  void           *user);

JCE_API void JCE_CALL
jce_session_set_event_handler(JceSessionEventFn fn, void *user);

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API bool JCE_CALL jce_session_start_host(const JceSessionStartHostDesc *desc);
JCE_API bool JCE_CALL jce_session_start_dedicated_server(const JceSessionStartHostDesc *desc);
JCE_API bool JCE_CALL jce_session_start_client(const JceSessionStartClientDesc *desc);
JCE_API void JCE_CALL jce_session_shutdown(void);

/* Drive the session: poll the host, advance handshake / timeout state,
 * dispatch packets.  Call once per frame from the application loop. */
JCE_API void JCE_CALL jce_session_tick(void);

/* ================================================================== */
/* Introspection                                                       */
/* ================================================================== */

JCE_API JceSessionMode  JCE_CALL jce_session_mode(void);
JCE_API JceSessionState JCE_CALL jce_session_state(void);
JCE_API const char *    JCE_CALL jce_session_state_to_string(JceSessionState s);

JCE_API JceClientId JCE_CALL jce_session_local_client_id(void);
JCE_API bool        JCE_CALL jce_session_is_server(void);
JCE_API bool        JCE_CALL jce_session_is_host(void);
JCE_API bool        JCE_CALL jce_session_is_client(void);

/* True iff `id` matches a roster entry whose peer is the local loopback
 * sentinel (i.e. HOST's own seat).  RPC and replication use this to
 * short-circuit self-targeted messages and avoid serializing through
 * the transport. */
JCE_API bool JCE_CALL jce_session_is_local_client(JceClientId id);

/* Roster access.  `index` is a stable [0, client_count()) slot; the slot
 * order is not guaranteed across add/remove. */
JCE_API uint32_t JCE_CALL jce_session_client_count(void);
JCE_API bool     JCE_CALL jce_session_get_client(uint32_t index,
                                                 JceSessionClientInfo *out);
JCE_API bool     JCE_CALL jce_session_get_client_by_id(JceClientId id,
                                                       JceSessionClientInfo *out);

/* Mapping helpers used by RPC + replication for targeted sends.
 * client_to_peer() returns UINT64_MAX for unknown / local-loopback ids.
 * peer_to_client() returns 0 (JCE_CLIENT_SERVER) when unknown. */
JCE_API uint64_t    JCE_CALL jce_session_client_to_peer(JceClientId id);
JCE_API JceClientId JCE_CALL jce_session_peer_to_client(uint64_t peer_handle);

/* Transport stats for a roster client.  Returns false for the local
 * loopback seat or any id that has no live transport peer. */
JCE_API bool JCE_CALL jce_session_get_peer_stats(JceClientId id,
                                                 JceNetPeerStats *out);

/* Best-effort "ip:port" string for a roster client.  Returns false for
 * the local loopback seat or unknown ids. */
JCE_API bool JCE_CALL jce_session_get_peer_address(JceClientId id,
                                                   char *buf,
                                                   uint32_t buf_size);

JCE_EXTERN_C_END

#endif /* JCE_SESSION_H */
