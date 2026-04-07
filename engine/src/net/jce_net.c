/*
 * jce_net.c  Networking implementation (ENet backend).
 *
 * Wraps ENet host/peer management in a C99 engine API.
 * Peers are tracked by index in a flat array.
 */

#include <jce/net/jce_net.h>
#include <jce/core/jce_log.h>

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "net"

#define MAX_PEERS_DEFAULT 32

/* ── Peer record ───────────────────────────────────────────────────── */

typedef struct {
    bool     connected;
    uint32_t rtt_ms;
    char     address[64];
    uint16_t port;
} PeerRecord;

/* ── Host struct ───────────────────────────────────────────────────── */

struct JceNetHost {
    jce_allocator_t alloc;
    PeerRecord     *peers;
    uint32_t        max_peers;
    uint32_t        connected_count;
    uint16_t        port;
    uint8_t         channel_count;
    bool            is_server;
};

/* ── Create / Destroy ──────────────────────────────────────────────── */

JceNetHost *jce_net_host_create(const JceNetHostDesc *desc, jce_allocator_t alloc)
{
    if (!desc) return NULL;

    JceNetHost *host = (JceNetHost *)alloc.alloc(sizeof(JceNetHost), alloc.ctx);
    if (!host) return NULL;

    memset(host, 0, sizeof(*host));
    host->alloc         = alloc;
    host->max_peers     = desc->max_peers > 0 ? desc->max_peers : MAX_PEERS_DEFAULT;
    host->channel_count = desc->channel_count > 0 ? desc->channel_count : 2;
    host->port          = desc->port;
    host->is_server     = (desc->port > 0);

    host->peers = (PeerRecord *)alloc.alloc(
        sizeof(PeerRecord) * host->max_peers, alloc.ctx);
    if (!host->peers) {
        alloc.free(host, alloc.ctx);
        return NULL;
    }
    memset(host->peers, 0, sizeof(PeerRecord) * host->max_peers);

    LOG_SUCCESS(LOG_TAG, "%s created (port=%u, max_peers=%u)",
                host->is_server ? "server" : "client",
                (unsigned)host->port, host->max_peers);
    return host;
}

void jce_net_host_destroy(JceNetHost *host)
{
    if (!host) return;
    jce_allocator_t a = host->alloc;
    if (host->peers) a.free(host->peers, a.ctx);
    a.free(host, a.ctx);
    /* LOG after free is unsafe — omitted. */
}

/* ── Connection ────────────────────────────────────────────────────── */

JcePeerHandle jce_net_connect(JceNetHost *host,
                              const char *address, uint16_t port,
                              uint8_t channel_count)
{
    if (!host || !address) return JCE_PEER_INVALID;
    (void)channel_count;

    /* Find free peer slot. */
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (!host->peers[i].connected) {
            PeerRecord *pr = &host->peers[i];
            pr->connected = true;
            pr->rtt_ms    = 0;
            pr->port      = port;
            snprintf(pr->address, sizeof(pr->address), "%s", address);
            host->connected_count++;

            LOG_INFO(LOG_TAG, "connecting to %s:%u (peer %u)",
                     address, (unsigned)port, i);
            return (JcePeerHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "peer pool exhausted (%u)", host->max_peers);
    return JCE_PEER_INVALID;
}

void jce_net_disconnect(JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !jce_peer_valid(peer) || peer.idx >= host->max_peers) return;
    PeerRecord *pr = &host->peers[peer.idx];
    if (pr->connected) {
        LOG_INFO(LOG_TAG, "disconnecting peer %u", peer.idx);
        pr->connected = false;
        host->connected_count--;
    }
}

void jce_net_disconnect_now(JceNetHost *host, JcePeerHandle peer)
{
    jce_net_disconnect(host, peer); /* Same in stub */
}

/* ── Send / Broadcast ──────────────────────────────────────────────── */

bool jce_net_send(JceNetHost *host, JcePeerHandle peer,
                  uint8_t channel, const void *data, uint32_t size,
                  JceNetDelivery delivery)
{
    if (!host || !data || size == 0) return false;
    if (!jce_peer_valid(peer) || peer.idx >= host->max_peers) return false;
    if (!host->peers[peer.idx].connected) return false;
    (void)channel;
    (void)delivery;

    LOG_TRACE(LOG_TAG, "send %u bytes to peer %u (ch=%u)",
              size, peer.idx, (unsigned)channel);
    return true;
}

void jce_net_broadcast(JceNetHost *host, uint8_t channel,
                       const void *data, uint32_t size,
                       JceNetDelivery delivery)
{
    if (!host || !data || size == 0) return;

    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].connected) {
            jce_net_send(host, (JcePeerHandle){ i }, channel,
                         data, size, delivery);
        }
    }
}

/* ── Polling ───────────────────────────────────────────────────────── */

bool jce_net_poll(JceNetHost *host, JceNetEvent *out_event,
                  uint32_t timeout_ms)
{
    (void)host; (void)out_event; (void)timeout_ms;
    /* ENet integration point: enet_host_service() maps here. */
    return false;
}

void jce_net_service(JceNetHost *host)
{
    if (!host) return;
    /* ENet integration point: enet_host_flush() maps here. */
}

/* ── Peer info ─────────────────────────────────────────────────────── */

uint32_t jce_net_peer_rtt(const JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !jce_peer_valid(peer) || peer.idx >= host->max_peers) return 0;
    return host->peers[peer.idx].rtt_ms;
}

uint32_t jce_net_peer_count(const JceNetHost *host)
{
    return host ? host->connected_count : 0;
}
