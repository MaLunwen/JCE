/*
 * jce_rpc.h — Remote Procedure Calls (P3-D.2).
 *
 * Two flavours, Unity-NGO style:
 *
 *   - ServerRpc : client -> server, authority-gated.  Only the OWNER of
 *                 a NetworkObject may invoke a ServerRpc on it; the
 *                 server drops anything else and bumps the
 *                 `rejected_authority` counter.
 *   - ClientRpc : server -> client, broadcast (TO_ALL_CLIENTS),
 *                 targeted (TO_OWNER, TO_NOT_OWNER, TO_CLIENT_ID).
 *
 * Wire format extension on top of jce_replication's packet-type byte:
 *
 *   [ packet_type=3 : u8 ]
 *   [ tick           : u32 ]
 *   [ net_id         : u32 ]
 *   [ rpc_name_len   : u16 ]
 *   [ rpc_name       : bytes ]
 *   [ sender         : u16 ]   (JceClientId of the original caller)
 *   [ target         : u8  ]   (JceRpcTarget)
 *   [ reliability    : u8  ]   (JceRpcReliability)
 *   [ payload_size   : u32 ]
 *   [ payload        : bytes ] (opaque caller-owned bytes)
 *
 * Payload is OPAQUE: the caller is responsible for serialization.
 * Codegen for typed RPCs is a future P3-D+ task.
 *
 * TODO: intern rpc_name -> u16 id table at handshake (D.6) so we don't
 *       ship the string on every call.  v1 keeps strings on the wire
 *       to stay debuggable.
 *
 * Reliability flag maps directly onto the transport's delivery channel
 * (RELIABLE -> JCE_NET_RELIABLE, UNRELIABLE -> JCE_NET_UNRELIABLE) on
 * JCE_NET_REPL_CHANNEL.  We do not allocate a second channel for v1.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 */

#ifndef JCE_RPC_H
#define JCE_RPC_H

#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceRpcTarget {
    JCE_RPC_TO_SERVER       = 0, /* client -> server (only valid for ServerRpc) */
    JCE_RPC_TO_OWNER        = 1, /* server -> the net object's owner            */
    JCE_RPC_TO_ALL_CLIENTS  = 2, /* server -> broadcast to every client         */
    JCE_RPC_TO_NOT_OWNER    = 3, /* server -> all clients except the owner      */
    JCE_RPC_TO_CLIENT_ID    = 4  /* server -> specific client (specific_client) */
} JceRpcTarget;

typedef enum JceRpcReliability {
    JCE_RPC_RELIABLE   = 0,
    JCE_RPC_UNRELIABLE = 1
} JceRpcReliability;

/*
 * Handler signature.  The implementor reads `payload` (opaque bytes) as
 * it sees fit; `payload_size` is the byte count.  `sender` is the
 * originating client id (JCE_CLIENT_SERVER if the server invoked it).
 */
typedef void (*JceRpcHandlerFn)(JceNetObjectId net_id,
                                JceClientId    sender,
                                const void    *payload,
                                uint32_t       payload_size,
                                void          *user);

typedef struct JceRpcDesc {
    const char       *name;                /* stable id used on the wire; must be unique */
    JceRpcReliability reliability;
    bool              server_authoritative; /* true -> only the server may receive */
    JceRpcHandlerFn   handler;
    void             *user;
} JceRpcDesc;

/* ================================================================== */
/* Subsystem                                                           */
/* ================================================================== */

/* Initialise the RPC subsystem.  Safe to call more than once.  The
 * replication subsystem must already be initialised (jce_rpc piggy-
 * backs on its host + role + local client id). */
JCE_API void JCE_CALL jce_rpc_init(void);

/* Tear down the registry.  Called by the engine on shutdown. */
JCE_API void JCE_CALL jce_rpc_shutdown(void);

/* Register an RPC name + handler.  Call during init.  Re-registering by
 * the same name updates the entry in place.
 *
 * Returns false when the RPC was NOT registered: bad descriptor (missing
 * name or handler), name longer than JCE_RPC_NAME_MAX, or the registry is
 * full.  CHECK IT — a dropped registration has no runtime symptom at the
 * call site; the RPC simply never fires, which surfaces much later as "this
 * ability does not replicate".  The table-full case in particular is
 * load-dependent, so it appears only once a project is large and only for
 * whatever registered last. */
JCE_API bool JCE_CALL jce_rpc_register(const JceRpcDesc *desc);

/* Number of currently registered RPCs (diagnostic). */
JCE_API uint32_t JCE_CALL jce_rpc_registered_count(void);

/* ================================================================== */
/* Send                                                                */
/* ================================================================== */

/*
 * Send an RPC.  `payload` may be NULL when `payload_size == 0`.
 *
 * ServerRpc rules (target == JCE_RPC_TO_SERVER):
 *   - Local role must be CLIENT (server uses TO_SERVER short-circuit
 *     for self-call, which is also accepted and runs the handler
 *     in-process).
 *   - Caller must own `net_id`; the server validates again on receive
 *     and drops + bumps rejected_authority otherwise.
 *
 * ClientRpc rules (any other target):
 *   - Local role must be SERVER and have authority over `net_id`
 *     (server has authority over everything).
 *
 * Returns true if the packet was enqueued / dispatched, false on a
 * validation failure (bad role, missing handler-side authority,
 * unknown net_id, etc).
 */
JCE_API bool JCE_CALL
jce_rpc_send(JceNetObjectId    net_id,
             const char       *rpc_name,
             JceRpcTarget      target,
             JceClientId       specific_client,
             const void       *payload,
             uint32_t          payload_size);

/* ================================================================== */
/* Receive                                                             */
/* ================================================================== */

/* Apply a single inbound RPC packet (called by the replication
 * dispatcher when packet_type == 3).  Exposed for headless tests. */
JCE_API void JCE_CALL
jce_rpc_handle_packet(const void *data, uint32_t size);

/* ================================================================== */
/* Stats (atomic counters)                                             */
/* ================================================================== */

JCE_API uint64_t JCE_CALL jce_rpc_total_sent(void);
JCE_API uint64_t JCE_CALL jce_rpc_total_received(void);
JCE_API uint64_t JCE_CALL jce_rpc_rejected_authority(void);

JCE_EXTERN_C_END

#endif /* JCE_RPC_H */
