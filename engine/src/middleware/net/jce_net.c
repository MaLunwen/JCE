/*
 * jce_net.c  Networking implementation (ENet backend).
 *
 * Wraps ENet host/peer management in a C99 engine API.
 * Peers are tracked by index into ENet's internal peer array.
 *
 * On Emscripten, native UDP sockets are unavailable; the WebSocket
 * backend (jce_net_web.c) is used instead and this file is skipped.
 */

#include <jce/os/core/jce_defs.h>

#if JCE_PLATFORM_WEB
/* WebSocket backend (jce_net_web.c) provides the implementation. */
#else

#include <jce/middleware/net/jce_net.h>
#include <jce/os/core/jce_log.h>

#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "net"

#define MAX_PEERS_DEFAULT 32

/* ── One-time ENet initialisation ─────────────────────────────────── */

static bool g_enet_initialised = false;

static void enet_shutdown(void)
{
    if (g_enet_initialised) {
        enet_deinitialize();
        g_enet_initialised = false;
    }
}

static bool enet_ensure_init(void)
{
    if (g_enet_initialised) return true;

    if (enet_initialize() != 0) {
        LOG_ERROR(LOG_TAG, "enet_initialize() failed");
        return false;
    }
    g_enet_initialised = true;
    atexit(enet_shutdown);
    return true;
}

/* ── Host struct ──────────────────────────────────────────────────── */

struct JceNetHost {
    jce_allocator_t  alloc;
    ENetHost        *enet_host;
    bool             is_server;
    uint32_t         max_peers;
    ENetPacket      *last_recv_packet;  /* deferred destroy on next poll */
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static enet_uint32 delivery_to_flags(JceNetDelivery delivery)
{
    switch (delivery) {
    case JCE_NET_RELIABLE:
        return ENET_PACKET_FLAG_RELIABLE;
    case JCE_NET_RELIABLE_UNORDERED:
        return ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED;
    case JCE_NET_UNRELIABLE: /* fall through */
    default:
        return 0;
    }
}

static bool peer_idx_valid(const JceNetHost *host, JcePeerHandle peer)
{
    return jce_peer_valid(peer) && peer.idx < host->max_peers;
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceNetHost *jce_net_host_create(const JceNetHostDesc *desc, jce_allocator_t alloc)
{
    if (!desc) return NULL;
    if (!enet_ensure_init()) return NULL;

    JceNetHost *host = (JceNetHost *)alloc.alloc(sizeof(JceNetHost), alloc.ctx);
    if (!host) return NULL;

    memset(host, 0, sizeof(*host));
    host->alloc     = alloc;
    host->max_peers = desc->max_peers > 0 ? desc->max_peers : MAX_PEERS_DEFAULT;
    host->is_server = (desc->port > 0);

    uint8_t channel_count = desc->channel_count > 0 ? desc->channel_count : 2;

    if (host->is_server) {
        ENetAddress address;
        address.host = ENET_HOST_ANY;
        address.port = desc->port;
        host->enet_host = enet_host_create(
            &address,
            host->max_peers,
            channel_count,
            desc->incoming_bandwidth,
            desc->outgoing_bandwidth);
    } else {
        host->enet_host = enet_host_create(
            NULL,
            host->max_peers,
            channel_count,
            desc->incoming_bandwidth,
            desc->outgoing_bandwidth);
    }

    if (!host->enet_host) {
        LOG_ERROR(LOG_TAG, "enet_host_create() failed (port=%u)",
                  (unsigned)desc->port);
        alloc.free(host, alloc.ctx);
        return NULL;
    }

    LOG_SUCCESS(LOG_TAG, "%s created (port=%u, max_peers=%u, channels=%u)",
                host->is_server ? "server" : "client",
                (unsigned)desc->port, host->max_peers,
                (unsigned)channel_count);
    return host;
}

void jce_net_host_destroy(JceNetHost *host)
{
    if (!host) return;
    jce_allocator_t a = host->alloc;

    if (host->last_recv_packet) {
        enet_packet_destroy(host->last_recv_packet);
        host->last_recv_packet = NULL;
    }
    if (host->enet_host) {
        enet_host_destroy(host->enet_host);
        host->enet_host = NULL;
    }

    a.free(host, a.ctx);
}

/* ── Connection ───────────────────────────────────────────────────── */

JcePeerHandle jce_net_connect(JceNetHost *host,
                              const char *address, uint16_t port,
                              uint8_t channel_count)
{
    if (!host || !address || !host->enet_host) return JCE_PEER_INVALID;

    ENetAddress addr;
    addr.port = port;
    if (enet_address_set_host(&addr, address) != 0) {
        LOG_ERROR(LOG_TAG, "failed to resolve host: %s", address);
        return JCE_PEER_INVALID;
    }

    uint8_t ch = channel_count > 0 ? channel_count : 2;
    ENetPeer *peer = enet_host_connect(host->enet_host, &addr, ch, 0);
    if (!peer) {
        LOG_ERROR(LOG_TAG, "enet_host_connect() failed (no available peers)");
        return JCE_PEER_INVALID;
    }

    uint32_t idx = (uint32_t)(peer - host->enet_host->peers);
    LOG_INFO(LOG_TAG, "connecting to %s:%u (peer %u)",
             address, (unsigned)port, idx);
    return (JcePeerHandle){ idx };
}

void jce_net_disconnect(JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !host->enet_host || !peer_idx_valid(host, peer)) return;

    ENetPeer *ep = &host->enet_host->peers[peer.idx];
    if (ep->state == ENET_PEER_STATE_CONNECTED ||
        ep->state == ENET_PEER_STATE_CONNECTING) {
        LOG_INFO(LOG_TAG, "disconnecting peer %u (graceful)", peer.idx);
        enet_peer_disconnect(ep, 0);
    }
}

void jce_net_disconnect_now(JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !host->enet_host || !peer_idx_valid(host, peer)) return;

    ENetPeer *ep = &host->enet_host->peers[peer.idx];
    LOG_INFO(LOG_TAG, "disconnecting peer %u (immediate)", peer.idx);
    enet_peer_disconnect_now(ep, 0);
}

/* ── Send / Broadcast ─────────────────────────────────────────────── */

bool jce_net_send(JceNetHost *host, JcePeerHandle peer,
                  uint8_t channel, const void *data, uint32_t size,
                  JceNetDelivery delivery)
{
    if (!host || !host->enet_host || !data || size == 0) return false;
    if (!peer_idx_valid(host, peer)) return false;

    ENetPeer *ep = &host->enet_host->peers[peer.idx];
    if (ep->state != ENET_PEER_STATE_CONNECTED) return false;

    enet_uint32 flags = delivery_to_flags(delivery);
    ENetPacket *pkt = enet_packet_create(data, size, flags);
    if (!pkt) {
        LOG_ERROR(LOG_TAG, "enet_packet_create() failed (%u bytes)", size);
        return false;
    }

    if (enet_peer_send(ep, channel, pkt) != 0) {
        LOG_ERROR(LOG_TAG, "enet_peer_send() failed (peer %u, ch %u)",
                  peer.idx, (unsigned)channel);
        enet_packet_destroy(pkt);
        return false;
    }

    return true;
}

void jce_net_broadcast(JceNetHost *host, uint8_t channel,
                       const void *data, uint32_t size,
                       JceNetDelivery delivery)
{
    if (!host || !host->enet_host || !data || size == 0) return;

    enet_uint32 flags = delivery_to_flags(delivery);
    ENetPacket *pkt = enet_packet_create(data, size, flags);
    if (!pkt) {
        LOG_ERROR(LOG_TAG, "enet_packet_create() failed (%u bytes)", size);
        return;
    }

    enet_host_broadcast(host->enet_host, channel, pkt);
}

/* ── Polling ──────────────────────────────────────────────────────── */

bool jce_net_poll(JceNetHost *host, JceNetEvent *out_event,
                  uint32_t timeout_ms)
{
    if (!host || !host->enet_host || !out_event) return false;

    /* Destroy the packet from the previous RECEIVE event. */
    if (host->last_recv_packet) {
        enet_packet_destroy(host->last_recv_packet);
        host->last_recv_packet = NULL;
    }

    memset(out_event, 0, sizeof(*out_event));
    out_event->peer = JCE_PEER_INVALID;

    ENetEvent event;
    int result = enet_host_service(host->enet_host, &event, timeout_ms);
    if (result <= 0) return false;   /* 0 = no event, <0 = error */

    uint32_t peer_idx = (uint32_t)(event.peer - host->enet_host->peers);
    out_event->peer = (JcePeerHandle){ peer_idx };
    out_event->channel = event.channelID;

    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT:
        out_event->type = JCE_NET_EVENT_CONNECT;
        LOG_INFO(LOG_TAG, "peer %u connected", peer_idx);
        break;

    case ENET_EVENT_TYPE_DISCONNECT:
        out_event->type = JCE_NET_EVENT_DISCONNECT;
        LOG_INFO(LOG_TAG, "peer %u disconnected", peer_idx);
        break;

#ifdef ENET_EVENT_TYPE_DISCONNECT_TIMEOUT
    case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
        out_event->type = JCE_NET_EVENT_TIMEOUT;
        LOG_WARN(LOG_TAG, "peer %u timed out", peer_idx);
        break;
#endif

    case ENET_EVENT_TYPE_RECEIVE:
        out_event->type      = JCE_NET_EVENT_RECEIVE;
        out_event->data      = event.packet->data;
        out_event->data_size = (uint32_t)event.packet->dataLength;
        host->last_recv_packet = event.packet;  /* destroy on next poll */
        break;

    default:
        return false;
    }

    return true;
}

void jce_net_service(JceNetHost *host)
{
    if (!host || !host->enet_host) return;
    enet_host_flush(host->enet_host);
}

/* ── Peer info ────────────────────────────────────────────────────── */

uint32_t jce_net_peer_rtt(const JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !host->enet_host || !peer_idx_valid(host, peer)) return 0;
    return (uint32_t)host->enet_host->peers[peer.idx].roundTripTime;
}

uint32_t jce_net_peer_count(const JceNetHost *host)
{
    if (!host || !host->enet_host) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->enet_host->peers[i].state == ENET_PEER_STATE_CONNECTED)
            count++;
    }
    return count;
}

JceNetTransport jce_net_get_transport(void)
{
    return JCE_NET_TRANSPORT_ENET;
}

bool jce_net_peer_stats(const JceNetHost *host, JcePeerHandle peer,
                        JceNetPeerStats *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!host || !host->enet_host || !peer_idx_valid(host, peer))
        return false;
    const ENetPeer *p = &host->enet_host->peers[peer.idx];
    if (p->state != ENET_PEER_STATE_CONNECTED) return false;
    out->rtt_ms       = (uint32_t)p->roundTripTime;
    out->packets_sent = (uint32_t)p->packetsSent;
    out->packets_lost = (uint32_t)p->packetsLost;
    out->bytes_in     = (uint64_t)p->incomingDataTotal;
    out->bytes_out    = (uint64_t)p->outgoingDataTotal;
    return true;
}

bool jce_net_peer_address_str(const JceNetHost *host, JcePeerHandle peer,
                              char *buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0) return false;
    buf[0] = '\0';
    if (!host || !host->enet_host || !peer_idx_valid(host, peer))
        return false;
    const ENetPeer *p = &host->enet_host->peers[peer.idx];
    if (p->state != ENET_PEER_STATE_CONNECTED) return false;
    char ip[64] = {0};
    if (enet_address_get_host_ip(&p->address, ip, sizeof ip) < 0)
        return false;
    snprintf(buf, buf_size, "%s:%u", ip, (unsigned)p->address.port);
    return true;
}

#endif /* !__EMSCRIPTEN__ */
