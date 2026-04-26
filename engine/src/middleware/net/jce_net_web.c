/*
 * jce_net_web.c  Networking implementation (WebSocket backend for Emscripten).
 *
 * On WASM, native UDP sockets are unavailable.  This backend wraps the
 * Emscripten WebSocket API behind the same jce_net.h interface so that
 * game code can use networking without platform-specific changes.
 *
 * Limitations vs. ENet:
 *   - All delivery is TCP-reliable (WebSocket is TCP-based).
 *   - Broadcast is implemented by iterating connected peers.
 *   - RTT measurement is approximate (ping/pong timestamps).
 *
 * Compiled ONLY when __EMSCRIPTEN__ is defined; on other platforms
 * jce_net.c (ENet backend) is used instead.
 */

#ifdef __EMSCRIPTEN__

#include <jce/middleware/net/jce_net.h>
#include <jce/os/core/jce_log.h>

#include <emscripten/websocket.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_TAG "net_web"

#define MAX_PEERS_DEFAULT   32
#define MAX_RECV_QUEUE      64
#define MAX_RECV_DATA_SIZE  (64 * 1024)  /* 64 KiB per message */

/* ── Received message queue (ring buffer) ──────────────────────────── */

typedef struct {
    uint32_t peer_idx;
    uint8_t  channel;
    uint32_t size;
    uint8_t  data[MAX_RECV_DATA_SIZE];
} RecvEntry;

/* ── Peer state ────────────────────────────────────────────────────── */

typedef enum {
    PEER_STATE_DISCONNECTED = 0,
    PEER_STATE_CONNECTING,
    PEER_STATE_CONNECTED
} PeerState;

typedef struct {
    EMSCRIPTEN_WEBSOCKET_T ws;
    PeerState              state;
    uint64_t               last_pong_ms;   /* for approximate RTT */
    uint32_t               rtt_ms;
} WebPeer;

/* ── Host struct ──────────────────────────────────────────────────── */

struct JceNetHost {
    jce_allocator_t  alloc;
    bool             is_server;
    uint32_t         max_peers;
    WebPeer         *peers;

    /* Receive ring buffer. */
    RecvEntry        recv_ring[MAX_RECV_QUEUE];
    uint32_t         recv_head;
    uint32_t         recv_tail;

    /* Pending event queue for connect/disconnect. */
    JceNetEvent      event_queue[MAX_RECV_QUEUE];
    uint32_t         evt_head;
    uint32_t         evt_tail;
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static void push_event(JceNetHost *host, JceNetEventType type,
                       uint32_t peer_idx)
{
    uint32_t next = (host->evt_head + 1) % MAX_RECV_QUEUE;
    if (next == host->evt_tail) return;  /* queue full, drop */

    JceNetEvent *e = &host->event_queue[host->evt_head];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->peer = (JcePeerHandle){ peer_idx };
    host->evt_head = next;
}

static void push_recv(JceNetHost *host, uint32_t peer_idx,
                      const void *data, uint32_t size)
{
    uint32_t next = (host->recv_head + 1) % MAX_RECV_QUEUE;
    if (next == host->recv_tail) return;  /* queue full, drop */

    RecvEntry *entry = &host->recv_ring[host->recv_head];
    entry->peer_idx = peer_idx;
    entry->channel  = 0;
    entry->size     = size < MAX_RECV_DATA_SIZE ? size : MAX_RECV_DATA_SIZE;
    memcpy(entry->data, data, entry->size);
    host->recv_head = next;
}

/* ── WebSocket callbacks ──────────────────────────────────────────── */

static EM_BOOL ws_on_open(int event_type,
                          const EmscriptenWebSocketOpenEvent *event,
                          void *user_data)
{
    (void)event_type;
    JceNetHost *host = (JceNetHost *)user_data;

    /* Find the peer with this socket. */
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].ws == event->socket &&
            host->peers[i].state == PEER_STATE_CONNECTING) {
            host->peers[i].state = PEER_STATE_CONNECTED;
            push_event(host, JCE_NET_EVENT_CONNECT, i);
            LOG_INFO(LOG_TAG, "peer %u connected (WebSocket)", i);
            break;
        }
    }
    return EM_TRUE;
}

static EM_BOOL ws_on_message(int event_type,
                             const EmscriptenWebSocketMessageEvent *event,
                             void *user_data)
{
    (void)event_type;
    JceNetHost *host = (JceNetHost *)user_data;

    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].ws == event->socket &&
            host->peers[i].state == PEER_STATE_CONNECTED) {
            push_recv(host, i, event->data, (uint32_t)event->numBytes);
            break;
        }
    }
    return EM_TRUE;
}

static EM_BOOL ws_on_error(int event_type,
                           const EmscriptenWebSocketErrorEvent *event,
                           void *user_data)
{
    (void)event_type;
    JceNetHost *host = (JceNetHost *)user_data;

    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].ws == event->socket) {
            LOG_ERROR(LOG_TAG, "peer %u WebSocket error", i);
            break;
        }
    }
    return EM_TRUE;
}

static EM_BOOL ws_on_close(int event_type,
                           const EmscriptenWebSocketCloseEvent *event,
                           void *user_data)
{
    (void)event_type;
    JceNetHost *host = (JceNetHost *)user_data;

    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].ws == event->socket) {
            PeerState prev = host->peers[i].state;
            host->peers[i].state = PEER_STATE_DISCONNECTED;
            host->peers[i].ws    = 0;

            if (prev == PEER_STATE_CONNECTED) {
                push_event(host, JCE_NET_EVENT_DISCONNECT, i);
                LOG_INFO(LOG_TAG, "peer %u disconnected (WebSocket)", i);
            } else if (prev == PEER_STATE_CONNECTING) {
                push_event(host, JCE_NET_EVENT_TIMEOUT, i);
                LOG_WARN(LOG_TAG, "peer %u connection failed (WebSocket)", i);
            }
            break;
        }
    }
    return EM_TRUE;
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceNetHost *jce_net_host_create(const JceNetHostDesc *desc, jce_allocator_t alloc)
{
    if (!desc) return NULL;

    if (desc->port > 0) {
        LOG_WARN(LOG_TAG, "WebSocket backend cannot act as a server "
                 "in the browser — creating client-only host");
    }

    JceNetHost *host = (JceNetHost *)alloc.alloc(sizeof(JceNetHost), alloc.ctx);
    if (!host) return NULL;

    memset(host, 0, sizeof(*host));
    host->alloc     = alloc;
    host->max_peers = desc->max_peers > 0 ? desc->max_peers : MAX_PEERS_DEFAULT;
    host->is_server = false;  /* server not supported in WASM */

    host->peers = (WebPeer *)alloc.alloc(
        sizeof(WebPeer) * host->max_peers, alloc.ctx);
    if (!host->peers) {
        alloc.free(host, alloc.ctx);
        return NULL;
    }
    memset(host->peers, 0, sizeof(WebPeer) * host->max_peers);

    LOG_SUCCESS(LOG_TAG, "WebSocket host created (max_peers=%u)", host->max_peers);
    return host;
}

void jce_net_host_destroy(JceNetHost *host)
{
    if (!host) return;
    jce_allocator_t a = host->alloc;

    /* Close all WebSocket connections. */
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].ws) {
            emscripten_websocket_close(host->peers[i].ws, 1000, "host destroyed");
            emscripten_websocket_delete(host->peers[i].ws);
            host->peers[i].ws = 0;
        }
    }

    a.free(host->peers, a.ctx);
    a.free(host, a.ctx);
}

/* ── Connection ───────────────────────────────────────────────────── */

JcePeerHandle jce_net_connect(JceNetHost *host,
                              const char *address, uint16_t port,
                              uint8_t channel_count)
{
    (void)channel_count;
    if (!host || !address) return JCE_PEER_INVALID;

    /* Find a free peer slot. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].state == PEER_STATE_DISCONNECTED) {
            idx = i;
            break;
        }
    }
    if (idx == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "no free peer slots");
        return JCE_PEER_INVALID;
    }

    /* Build ws:// or wss:// URL. */
    char url[512];
    snprintf(url, sizeof(url), "ws://%s:%u", address, (unsigned)port);

    EmscriptenWebSocketCreateAttributes ws_attrs = {
        .url      = url,
        .protocols = NULL,
        .createOnMainThread = EM_TRUE
    };

    EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&ws_attrs);
    if (ws <= 0) {
        LOG_ERROR(LOG_TAG, "emscripten_websocket_new() failed for %s", url);
        return JCE_PEER_INVALID;
    }

    host->peers[idx].ws    = ws;
    host->peers[idx].state = PEER_STATE_CONNECTING;

    emscripten_websocket_set_onopen_callback(ws, host, ws_on_open);
    emscripten_websocket_set_onmessage_callback(ws, host, ws_on_message);
    emscripten_websocket_set_onerror_callback(ws, host, ws_on_error);
    emscripten_websocket_set_onclose_callback(ws, host, ws_on_close);

    LOG_INFO(LOG_TAG, "connecting to %s (peer %u)", url, idx);
    return (JcePeerHandle){ idx };
}

void jce_net_disconnect(JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !jce_peer_valid(peer) || peer.idx >= host->max_peers) return;

    WebPeer *wp = &host->peers[peer.idx];
    if (wp->state == PEER_STATE_DISCONNECTED) return;

    LOG_INFO(LOG_TAG, "disconnecting peer %u (graceful)", peer.idx);
    if (wp->ws) {
        emscripten_websocket_close(wp->ws, 1000, "disconnect");
        emscripten_websocket_delete(wp->ws);
        wp->ws = 0;
    }
    wp->state = PEER_STATE_DISCONNECTED;
    push_event(host, JCE_NET_EVENT_DISCONNECT, peer.idx);
}

void jce_net_disconnect_now(JceNetHost *host, JcePeerHandle peer)
{
    /* WebSocket close is always graceful; same implementation. */
    jce_net_disconnect(host, peer);
}

/* ── Send / Broadcast ─────────────────────────────────────────────── */

bool jce_net_send(JceNetHost *host, JcePeerHandle peer,
                  uint8_t channel, const void *data, uint32_t size,
                  JceNetDelivery delivery)
{
    (void)channel;
    (void)delivery;  /* WebSocket is always reliable-ordered */
    if (!host || !data || size == 0) return false;
    if (!jce_peer_valid(peer) || peer.idx >= host->max_peers) return false;

    WebPeer *wp = &host->peers[peer.idx];
    if (wp->state != PEER_STATE_CONNECTED || !wp->ws) return false;

    EMSCRIPTEN_RESULT res = emscripten_websocket_send_binary(
        wp->ws, (void *)data, size);

    return res == EMSCRIPTEN_RESULT_SUCCESS;
}

void jce_net_broadcast(JceNetHost *host, uint8_t channel,
                       const void *data, uint32_t size,
                       JceNetDelivery delivery)
{
    if (!host || !data || size == 0) return;

    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].state == PEER_STATE_CONNECTED) {
            JcePeerHandle p = { i };
            jce_net_send(host, p, channel, data, size, delivery);
        }
    }
}

/* ── Polling ──────────────────────────────────────────────────────── */

bool jce_net_poll(JceNetHost *host, JceNetEvent *out_event,
                  uint32_t timeout_ms)
{
    (void)timeout_ms;  /* non-blocking in WASM (single-threaded) */
    if (!host || !out_event) return false;

    memset(out_event, 0, sizeof(*out_event));
    out_event->peer = JCE_PEER_INVALID;

    /* Check control events first (connect/disconnect/timeout). */
    if (host->evt_head != host->evt_tail) {
        *out_event = host->event_queue[host->evt_tail];
        host->evt_tail = (host->evt_tail + 1) % MAX_RECV_QUEUE;
        return true;
    }

    /* Check received data. */
    if (host->recv_head != host->recv_tail) {
        RecvEntry *entry = &host->recv_ring[host->recv_tail];
        out_event->type      = JCE_NET_EVENT_RECEIVE;
        out_event->peer      = (JcePeerHandle){ entry->peer_idx };
        out_event->channel   = entry->channel;
        out_event->data      = entry->data;
        out_event->data_size = entry->size;
        host->recv_tail = (host->recv_tail + 1) % MAX_RECV_QUEUE;
        return true;
    }

    return false;
}

void jce_net_service(JceNetHost *host)
{
    /* WebSocket I/O is handled by the browser event loop;
       nothing to flush explicitly. */
    (void)host;
}

/* ── Peer info ────────────────────────────────────────────────────── */

uint32_t jce_net_peer_rtt(const JceNetHost *host, JcePeerHandle peer)
{
    if (!host || !jce_peer_valid(peer) || peer.idx >= host->max_peers)
        return 0;
    return host->peers[peer.idx].rtt_ms;
}

uint32_t jce_net_peer_count(const JceNetHost *host)
{
    if (!host) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < host->max_peers; i++) {
        if (host->peers[i].state == PEER_STATE_CONNECTED)
            count++;
    }
    return count;
}

JceNetTransport jce_net_get_transport(void)
{
    return JCE_NET_TRANSPORT_WEBSOCKET;
}

#endif /* __EMSCRIPTEN__ */
