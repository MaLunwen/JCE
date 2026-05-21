/*
 * jce_session_internal.h — L4-internal seam between session, replication
 * and RPC.  NOT a public header.  Lives next to jce_session.c.
 *
 * Two responsibilities:
 *   1. Provide jce_replication's packet dispatcher with a hook for the
 *      session packet type (4).  Replication forwards type==4 to
 *      jce__session_recv_packet(); session re-feeds anything else back
 *      via the normal dispatcher when it owns polling.
 *   2. Provide jce_rpc with peer resolution so it can switch
 *      TO_OWNER / TO_NOT_OWNER / TO_CLIENT_ID from "broadcast + receive-
 *      side filter" to direct jce_net_send.  Receive-side filter stays
 *      as defence in depth.
 *
 * All entry points are NO-OPs when no session is active so existing
 * (session-less) callers keep working.
 */

#ifndef JCE_SESSION_INTERNAL_H
#define JCE_SESSION_INTERNAL_H

#include <jce/middleware/net/jce_net_types.h>
#include <jce/middleware/net/jce_replication.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Replication dispatcher hands a packet whose first byte is
 * JCE_REPL_PKT_SESSION (= 4) here.  `peer_idx` is the JcePeerHandle.idx
 * of the sender (UINT32_MAX for "unknown / local-injected"). */
void jce__session_recv_packet(uint32_t peer_idx,
                              const void *data, uint32_t size);

/* Look up the peer handle for a known client id.  Returns false (and
 * leaves *out untouched) if there is no roster entry, or if the entry's
 * peer is the local loopback sentinel (use jce__session_is_local_client
 * to detect that case explicitly). */
bool jce__session_get_peer(JceClientId id, JcePeerHandle *out);

/* True iff `id` corresponds to HOST's own local seat. */
bool jce__session_is_local_client_internal(JceClientId id);

/* Owner of `id` matches local seat? */

/* Iterate every transport-traversing remote peer in the roster
 * (excludes the local loopback).  Used by RPC TO_NOT_OWNER fanout. */
typedef void (*jce__session_peer_iter_fn)(JceClientId   id,
                                          JcePeerHandle peer,
                                          void         *user);
void jce__session_iter_remote_peers(jce__session_peer_iter_fn fn, void *user);

/* True when the session subsystem owns the host poll loop, in which
 * case other subsystems (replication client tick) should NOT call
 * jce_net_poll() themselves to avoid stealing events. */
bool jce__session_owns_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SESSION_INTERNAL_H */
