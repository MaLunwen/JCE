/*
 * jce_net.h  Multiplayer networking (ENet backend).
 *
 * Provides client/server UDP networking with reliable and unreliable
 * channels.  Built on top of ENet for cross-platform support.
 *
 * Thread safety: NOT thread-safe.  All calls must happen on the
 * main thread.  For background I/O, queue messages and flush.
 *
 * Layer: Network (Layer 3 — optional subsystem, priority 200).
 */

#ifndef JCE_NET_H
#define JCE_NET_H


#include <jce/middleware/net/jce_net_types.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Host lifecycle                                                      */
/* ================================================================== */

typedef struct JceNetHost JceNetHost;

typedef struct {
    /* Server: port to listen on.  Client: 0 (auto). */
    uint16_t port;

    /* Maximum number of simultaneous connections. */
    uint32_t max_peers;      /* default: 32 */

    /* Number of ENet channels. */
    uint8_t  channel_count;  /* default: 2 */

    /* Bandwidth limits (bytes/sec), 0 = unlimited. */
    uint32_t incoming_bandwidth;
    uint32_t outgoing_bandwidth;
} JceNetHostDesc;

/* Create a host.  For a server, set port > 0.
   For a client, set port = 0 (ephemeral). */
JCE_API JceNetHost *jce_net_host_create(const JceNetHostDesc *desc, jce_allocator_t alloc);
JCE_API void        jce_net_host_destroy(JceNetHost *host);

/* ================================================================== */
/* Connection                                                          */
/* ================================================================== */

/* Connect to a remote address.  Returns peer handle (valid before
   JCE_NET_EVENT_CONNECT fires — do not send until connected). */
JCE_API JcePeerHandle jce_net_connect(JceNetHost *host,
                              const char *address, uint16_t port,
                              uint8_t channel_count);

/* Graceful disconnect.  Triggers JCE_NET_EVENT_DISCONNECT. */
JCE_API void jce_net_disconnect(JceNetHost *host, JcePeerHandle peer);

/* Forceful disconnect (no notification to remote). */
JCE_API void jce_net_disconnect_now(JceNetHost *host, JcePeerHandle peer);

/* ================================================================== */
/* Send / receive                                                      */
/* ================================================================== */

/* Send a message to a specific peer on a given channel. */
JCE_API bool JCE_CALL jce_net_send(JceNetHost *host, JcePeerHandle peer,
                                   uint8_t channel, const void *data, uint32_t size,
                                   JceNetDelivery delivery);

/* Broadcast a message to all connected peers. */
JCE_API void JCE_CALL jce_net_broadcast(JceNetHost *host, uint8_t channel,
                                        const void *data, uint32_t size,
                                        JceNetDelivery delivery);

/* ================================================================== */
/* Polling                                                             */
/* ================================================================== */

/* Poll for network events.  timeout_ms = 0 for non-blocking.
   Returns true if an event was received. */
JCE_API bool JCE_CALL jce_net_poll(JceNetHost *host, JceNetEvent *out_event,
                                   uint32_t timeout_ms);

/* Service the host: send queued packets, receive events.
   Call once per frame. */
JCE_API void jce_net_service(JceNetHost *host);

/* ================================================================== */
/* Transport info                                                      */
/* ================================================================== */

typedef enum {
    JCE_NET_TRANSPORT_ENET      = 0,   /* UDP via ENet (desktop/mobile) */
    JCE_NET_TRANSPORT_WEBSOCKET = 1    /* WebSocket (Emscripten/WASM)   */
} JceNetTransport;

/* Return the active network transport for this build. */
JCE_API JceNetTransport jce_net_get_transport(void);

/* ================================================================== */
/* Peer info                                                           */
/* ================================================================== */

/* Round-trip time in milliseconds. */
JCE_API uint32_t jce_net_peer_rtt(const JceNetHost *host, JcePeerHandle peer);

/* Number of currently connected peers. */
JCE_API uint32_t jce_net_peer_count(const JceNetHost *host);

typedef struct JceNetPeerStats {
    uint32_t rtt_ms;
    uint32_t packets_sent;
    uint32_t packets_lost;
    uint64_t bytes_in;
    uint64_t bytes_out;
} JceNetPeerStats;

/* Fill `out` with current transport stats for `peer`.  Returns false
 * if the peer is not connected. */
JCE_API bool jce_net_peer_stats(const JceNetHost *host, JcePeerHandle peer,
                                JceNetPeerStats *out);

/* Write a human-readable "ip:port" string for `peer` into `buf` (NUL-
 * terminated).  Returns false on bad args / unconnected peer. */
JCE_API bool jce_net_peer_address_str(const JceNetHost *host, JcePeerHandle peer,
                                      char *buf, uint32_t buf_size);

JCE_EXTERN_C_END

#endif /* JCE_NET_H */
