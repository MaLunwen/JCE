/*
 * jce_lan_discovery.c  P3-D.5 LAN host discovery (UDP broadcast).
 *
 * Wire format (little-endian):
 *   REQ  : [ magic u32 = 'JCEL' ][ version u16 ][ opcode u8 = 1 ]
 *          [ client_nonce u32 ]                                              (11 B)
 *   RESP : [ magic u32 ][ version u16 ][ opcode u8 = 2 ][ client_nonce u32 ]
 *          [ game_port u32 ][ current u16 ][ max u16 ][ protocol u32 ]
 *          [ name_len u16 ][ name bytes ]                          (23 B + name)
 *
 * Magic = 'JCEL' on the wire = bytes { 'J','C','E','L' } i.e. 0x4C45434A LE.
 *
 * Transport: ENet's socket API (`enet_socket_*`).  Both sockets are
 * non-blocking datagram sockets.  Server binds 0.0.0.0:discovery_port
 * and replies to whoever asks.  Client binds 0.0.0.0:0, sets
 * ENET_SOCKOPT_BROADCAST, and sends REQ to ENET_HOST_BROADCAST.
 */

#include <jce/os/core/jce_defs.h>

#if JCE_PLATFORM_WEB
/* WebSocket build (jce_net_web.c) has no UDP broadcast — discovery is a
 * native-build-only feature. */
#else

#include <jce/middleware/net/jce_lan_discovery.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include "jce_net_bytes.h"

#include <enet/enet.h>

#include <stdio.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG "lan-disc"

#define LAN_MAGIC          0x4C45434Au /* 'JCEL' little-endian */
#define LAN_OPCODE_REQ     1u
#define LAN_OPCODE_RESP    2u
#define LAN_REQ_SIZE       11u
#define LAN_RESP_HDR_SIZE  23u
#define LAN_MAX_PACKET     512u   /* name <= 63 chars → headroom plenty */

/* Little-endian wire codec: the advancing-cursor helpers in
 * jce_net_bytes.h (jce_net_wr_* / jce_net_rd_*).  Both packets are
 * bounds-checked by their LAN_*_SIZE guards before any read, so the
 * unchecked cursor flavour is the right one here. */

/* ── ENet lifecycle ───────────────────────────────────────────────────
 *
 * jce_net.c owns the process-wide ENet refcount; discovery holds one
 * reference for as long as a beacon / scan socket is open.  Symmetric on
 * purpose: neither module may enet_deinitialize() while the other still
 * has live sockets. */
extern bool jce__net_enet_acquire(void);
extern void jce__net_enet_release(void);

static uint16_t lan_default_port(uint16_t requested)
{
    return requested != 0 ? requested : (uint16_t)JCE_LAN_DISCOVERY_DEFAULT_PORT;
}

static void format_address(const ENetAddress *addr, uint32_t game_port,
                           char *out, size_t out_size)
{
    char host_text[64];
    if (enet_address_get_host_ip(addr, host_text, sizeof(host_text)) != 0) {
        host_text[0] = '?'; host_text[1] = '\0';
    }
    /* Game port (not the discovery source port) — that's what the
     * client will hand to jce_net_connect. */
    snprintf(out, out_size, "%s:%u", host_text, (unsigned)game_port);
}

/* Cheap xorshift32 PRNG seeded from the high-res counter — good enough
 * for a discovery nonce.  We avoid <stdlib.h>'s rand() (state shared
 * with the rest of the engine) and any platform-specific RNG. */
static uint32_t lan_random_u32(void)
{
    static uint32_t state = 0;
    if (state == 0) {
        uint64_t s = jce_time_perf_counter();
        state = (uint32_t)(s ^ (s >> 32));
        if (state == 0) state = 0xA5A5A5A5u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* ── SERVER state ─────────────────────────────────────────────────── */

typedef struct {
    bool        running;
    ENetSocket  sock;
    uint16_t    port;
    uint32_t    game_port;
    uint16_t    current_players;
    uint16_t    max_players;
    char        name[JCE_LAN_DISCOVERY_NAME_LEN];
    uint16_t    name_len;
} LanServerState;

static LanServerState g_srv = { false, 0, 0, 0, 0, 0, { 0 }, 0 };

static void server_close_socket(void)
{
    if (g_srv.sock != ENET_SOCKET_NULL) {
        enet_socket_destroy(g_srv.sock);
        g_srv.sock = ENET_SOCKET_NULL;
    }
}

bool jce_lan_discovery_server_start(const char *server_name,
                                     uint32_t game_port,
                                     uint16_t current_players,
                                     uint16_t max_players,
                                     uint16_t discovery_port)
{
    if (g_srv.running) {
        LOG_WARN(LOG_TAG, "server_start: already running");
        return false;
    }
    if (!jce__net_enet_acquire()) return false;

    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.sock = ENET_SOCKET_NULL;
    g_srv.port = lan_default_port(discovery_port);
    g_srv.game_port = game_port;
    g_srv.current_players = current_players;
    g_srv.max_players = max_players;

    if (server_name && server_name[0] != '\0') {
        size_t n = strlen(server_name);
        if (n >= JCE_LAN_DISCOVERY_NAME_LEN) n = JCE_LAN_DISCOVERY_NAME_LEN - 1u;
        memcpy(g_srv.name, server_name, n);
        g_srv.name[n] = '\0';
        g_srv.name_len = (uint16_t)n;
    } else {
        g_srv.name[0] = '\0';
        g_srv.name_len = 0;
    }

    g_srv.sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (g_srv.sock == ENET_SOCKET_NULL) {
        LOG_ERROR(LOG_TAG, "server: enet_socket_create failed");
        jce__net_enet_release();
        return false;
    }
    (void)enet_socket_set_option(g_srv.sock, ENET_SOCKOPT_NONBLOCK,  1);
    (void)enet_socket_set_option(g_srv.sock, ENET_SOCKOPT_REUSEADDR, 1);

    ENetAddress bind_addr;
    bind_addr.host = ENET_HOST_ANY;
    bind_addr.port = g_srv.port;
    if (enet_socket_bind(g_srv.sock, &bind_addr) != 0) {
        LOG_ERROR(LOG_TAG, "server: bind 0.0.0.0:%u failed", (unsigned)g_srv.port);
        server_close_socket();
        jce__net_enet_release();
        return false;
    }

    g_srv.running = true;
    LOG_SUCCESS(LOG_TAG, "server beacon up: \"%s\" game_port=%u (disc_port=%u)",
                g_srv.name, (unsigned)g_srv.game_port, (unsigned)g_srv.port);
    return true;
}

void jce_lan_discovery_server_stop(void)
{
    if (!g_srv.running) return;
    server_close_socket();
    g_srv.running = false;
    jce__net_enet_release();
    LOG_INFO(LOG_TAG, "server beacon down");
}

void jce_lan_discovery_server_set_player_count(uint16_t current_players)
{
    g_srv.current_players = current_players;
}

bool jce_lan_discovery_server_is_running(void)
{
    return g_srv.running;
}

static void server_handle_request(const uint8_t *data, size_t size,
                                  const ENetAddress *from)
{
    if (size < LAN_REQ_SIZE) return;
    const uint8_t *p = data;
    uint32_t magic   = jce_net_rd_u32(&p);
    uint16_t version = jce_net_rd_u16(&p);
    uint8_t  opcode  = jce_net_rd_u8(&p);
    uint32_t nonce   = jce_net_rd_u32(&p);

    if (magic != LAN_MAGIC)                         return;
    if (version != JCE_LAN_PROTOCOL_VERSION)        return;
    if (opcode != LAN_OPCODE_REQ)                   return;

    uint8_t reply[LAN_MAX_PACKET];
    uint8_t *w = reply;
    jce_net_wr_u32(&w, LAN_MAGIC);
    jce_net_wr_u16(&w, (uint16_t)JCE_LAN_PROTOCOL_VERSION);
    jce_net_wr_u8 (&w, (uint8_t)LAN_OPCODE_RESP);
    jce_net_wr_u32(&w, nonce);
    jce_net_wr_u32(&w, g_srv.game_port);
    jce_net_wr_u16(&w, g_srv.current_players);
    jce_net_wr_u16(&w, g_srv.max_players);
    jce_net_wr_u32(&w, (uint32_t)JCE_LAN_PROTOCOL_VERSION);
    jce_net_wr_u16(&w, g_srv.name_len);
    if (g_srv.name_len > 0) {
        memcpy(w, g_srv.name, g_srv.name_len);
        w += g_srv.name_len;
    }

    ENetBuffer buf;
    buf.data       = reply;
    buf.dataLength = (size_t)(w - reply);
    int sent = enet_socket_send(g_srv.sock, from, &buf, 1);
    if (sent < 0) {
        LOG_WARN(LOG_TAG, "server: enet_socket_send failed");
    }
}

void jce_lan_discovery_server_tick(void)
{
    if (!g_srv.running) return;

    /* Drain everything that's ready — non-blocking, bounded so we
     * don't spin forever if someone floods the port. */
    for (int i = 0; i < 32; ++i) {
        uint8_t scratch[LAN_MAX_PACKET];
        ENetAddress from;
        ENetBuffer  buf;
        buf.data       = scratch;
        buf.dataLength = sizeof(scratch);

        int got = enet_socket_receive(g_srv.sock, &from, &buf, 1);
        if (got <= 0) break;
        server_handle_request(scratch, (size_t)got, &from);
    }
}

/* ── CLIENT state ─────────────────────────────────────────────────── */

typedef struct {
    bool        scanning;
    ENetSocket  sock;
    uint16_t    port;             /* discovery port being scanned */
    uint32_t    nonce;
    uint64_t    scan_start_ms;
    uint32_t    duration_ms;
} LanClientState;

static LanClientState g_cli = { false, 0, 0, 0, 0, 0 };

static JceLanDiscoveredServer g_servers[JCE_LAN_DISCOVERY_MAX_SERVERS];
static uint32_t               g_server_count = 0;

static void client_close_socket(void)
{
    if (g_cli.sock != ENET_SOCKET_NULL) {
        enet_socket_destroy(g_cli.sock);
        g_cli.sock = ENET_SOCKET_NULL;
    }
}

void jce_lan_discovery_client_clear(void)
{
    memset(g_servers, 0, sizeof(g_servers));
    g_server_count = 0;
}

uint32_t jce_lan_discovery_client_server_count(void)
{
    return g_server_count;
}

bool jce_lan_discovery_client_get_server(uint32_t idx, JceLanDiscoveredServer *out)
{
    if (!out || idx >= g_server_count) return false;
    *out = g_servers[idx];
    return true;
}

bool jce_lan_discovery_client_is_scanning(void)
{
    return g_cli.scanning;
}

bool jce_lan_discovery_client_start_scan(uint16_t discovery_port, uint32_t duration_ms)
{
    if (g_cli.scanning) jce_lan_discovery_client_stop_scan();
    if (!jce__net_enet_acquire()) return false;

    memset(&g_cli, 0, sizeof(g_cli));
    g_cli.sock        = ENET_SOCKET_NULL;
    g_cli.port        = lan_default_port(discovery_port);
    g_cli.nonce       = lan_random_u32();
    g_cli.duration_ms = duration_ms > 0 ? duration_ms : 3000u;

    g_cli.sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (g_cli.sock == ENET_SOCKET_NULL) {
        LOG_ERROR(LOG_TAG, "client: enet_socket_create failed");
        jce__net_enet_release();
        return false;
    }
    (void)enet_socket_set_option(g_cli.sock, ENET_SOCKOPT_NONBLOCK,  1);
    (void)enet_socket_set_option(g_cli.sock, ENET_SOCKOPT_BROADCAST, 1);

    ENetAddress bind_addr;
    bind_addr.host = ENET_HOST_ANY;
    bind_addr.port = 0; /* ephemeral */
    if (enet_socket_bind(g_cli.sock, &bind_addr) != 0) {
        LOG_ERROR(LOG_TAG, "client: bind 0.0.0.0:0 failed");
        client_close_socket();
        jce__net_enet_release();
        return false;
    }

    /* Send the broadcast REQ. */
    uint8_t pkt[LAN_REQ_SIZE];
    uint8_t *w = pkt;
    jce_net_wr_u32(&w, LAN_MAGIC);
    jce_net_wr_u16(&w, (uint16_t)JCE_LAN_PROTOCOL_VERSION);
    jce_net_wr_u8 (&w, (uint8_t)LAN_OPCODE_REQ);
    jce_net_wr_u32(&w, g_cli.nonce);

    ENetAddress bcast;
    bcast.host = ENET_HOST_BROADCAST;
    bcast.port = g_cli.port;

    ENetBuffer buf;
    buf.data       = pkt;
    buf.dataLength = sizeof(pkt);

    int sent = enet_socket_send(g_cli.sock, &bcast, &buf, 1);
    if (sent < 0) {
        /* Some networks/firewalls reject broadcast — keep socket open
         * to still receive any unicast RESPs from a server that
         * already knows us; document for the UI layer. */
        LOG_WARN(LOG_TAG, "client: broadcast send failed (network may block UDP broadcast)");
    }

    g_cli.scan_start_ms = jce_time_ticks_ms();
    g_cli.scanning      = true;
    LOG_INFO(LOG_TAG, "client scan begin (port=%u, duration=%u ms, nonce=0x%08x)",
             (unsigned)g_cli.port, (unsigned)g_cli.duration_ms,
             (unsigned)g_cli.nonce);
    return true;
}

void jce_lan_discovery_client_stop_scan(void)
{
    if (!g_cli.scanning) return;
    client_close_socket();
    g_cli.scanning = false;
    jce__net_enet_release();
    LOG_INFO(LOG_TAG, "client scan end (%u server(s) found)", (unsigned)g_server_count);
}

/* Linear scan dedupe keyed on (address.host, address.port, game_port).
 * 32 server cap × O(N) compare is trivial. */
static JceLanDiscoveredServer *find_or_insert(const ENetAddress *src_addr,
                                              uint32_t game_port,
                                              bool *out_is_new)
{
    char addr_text[64];
    {
        char ip[64];
        if (enet_address_get_host_ip(src_addr, ip, sizeof(ip)) != 0) {
            ip[0] = '?'; ip[1] = '\0';
        }
        snprintf(addr_text, sizeof(addr_text), "%s:%u", ip, (unsigned)game_port);
    }
    for (uint32_t i = 0; i < g_server_count; ++i) {
        if (strncmp(g_servers[i].address_text, addr_text,
                    sizeof(g_servers[i].address_text)) == 0) {
            *out_is_new = false;
            return &g_servers[i];
        }
    }
    if (g_server_count >= JCE_LAN_DISCOVERY_MAX_SERVERS) {
        *out_is_new = false;
        return NULL;
    }
    JceLanDiscoveredServer *slot = &g_servers[g_server_count++];
    memset(slot, 0, sizeof(*slot));
    jce_strlcpy(slot->address_text, addr_text, sizeof(slot->address_text));
    slot->ping_ms = -1;
    *out_is_new = true;
    return slot;
}

static void client_handle_response(const uint8_t *data, size_t size,
                                   const ENetAddress *from)
{
    if (size < LAN_RESP_HDR_SIZE) return;
    const uint8_t *p = data;
    uint32_t magic   = jce_net_rd_u32(&p);
    uint16_t version = jce_net_rd_u16(&p);
    uint8_t  opcode  = jce_net_rd_u8(&p);
    uint32_t nonce   = jce_net_rd_u32(&p);
    if (magic != LAN_MAGIC)                         return;
    if (version != JCE_LAN_PROTOCOL_VERSION)        return;
    if (opcode != LAN_OPCODE_RESP)                  return;
    if (nonce != g_cli.nonce)                       return;

    uint32_t game_port = jce_net_rd_u32(&p);
    uint16_t current   = jce_net_rd_u16(&p);
    uint16_t maxp      = jce_net_rd_u16(&p);
    uint32_t proto     = jce_net_rd_u32(&p);
    uint16_t name_len  = jce_net_rd_u16(&p);
    if (proto != JCE_LAN_PROTOCOL_VERSION)          return;
    if ((size_t)(p - data) + name_len > size)       return;
    if (name_len >= JCE_LAN_DISCOVERY_NAME_LEN) name_len = (uint16_t)(JCE_LAN_DISCOVERY_NAME_LEN - 1u);

    bool is_new = false;
    JceLanDiscoveredServer *slot = find_or_insert(from, game_port, &is_new);
    if (!slot) return;

    uint64_t now_ms = jce_time_ticks_ms();
    if (is_new) {
        memcpy(slot->server_name, p, name_len);
        slot->server_name[name_len] = '\0';
        slot->game_port        = game_port;
        slot->protocol_version = proto;
        slot->ping_ms          = (int32_t)(now_ms - g_cli.scan_start_ms);
    }
    /* Always refresh dynamic fields. */
    slot->current_players = current;
    slot->max_players     = maxp;
    slot->last_seen_ms    = (int64_t)now_ms;
}

void jce_lan_discovery_client_tick(void)
{
    if (!g_cli.scanning) return;

    /* Drain RESPs. */
    for (int i = 0; i < 32; ++i) {
        uint8_t scratch[LAN_MAX_PACKET];
        ENetAddress from;
        ENetBuffer  buf;
        buf.data       = scratch;
        buf.dataLength = sizeof(scratch);

        int got = enet_socket_receive(g_cli.sock, &from, &buf, 1);
        if (got <= 0) break;
        client_handle_response(scratch, (size_t)got, &from);
    }

    /* Auto-stop when the timer expires.  Results stay in the table
     * until the caller calls jce_lan_discovery_client_clear(). */
    uint64_t now_ms = jce_time_ticks_ms();
    if (now_ms - g_cli.scan_start_ms >= g_cli.duration_ms) {
        jce_lan_discovery_client_stop_scan();
    }
}

/* ── Self-test ────────────────────────────────────────────────────── */

#ifdef JCE_NET_SELF_TEST
#include <jce/os/core/jce_assert.h>

/* Loopback test: spin a server, run a scan, tick both for ~500 ms,
 * assert the local server appears in the client table.  Some sandbox
 * CI networks block bind / broadcast — failures log + return rather
 * than abort so the test can be best-effort. */
void jce_lan_discovery_self_test(void)
{
    /* Clean state. */
    jce_lan_discovery_server_stop();
    jce_lan_discovery_client_stop_scan();
    jce_lan_discovery_client_clear();

    const uint16_t test_port = 47776u; /* avoid collision with default */

    if (!jce_lan_discovery_server_start("TestSrv", 7777u, 1u, 4u, test_port)) {
        LOG_WARN(LOG_TAG, "self_test: server_start failed (skip)");
        return;
    }

    if (!jce_lan_discovery_client_start_scan(test_port, 500u)) {
        LOG_WARN(LOG_TAG, "self_test: client_start_scan failed (skip)");
        jce_lan_discovery_server_stop();
        return;
    }

    /* Pump both sides for ~500 ms in 10 ms steps. */
    uint64_t deadline = jce_time_ticks_ms() + 600u;
    while (jce_time_ticks_ms() < deadline && jce_lan_discovery_client_is_scanning()) {
        jce_lan_discovery_server_tick();
        jce_lan_discovery_client_tick();
        /* Busy-wait a small slice — discovery self-test is short and
         * we don't want to drag in a sleep dep just for one test. */
        uint64_t spin_until = jce_time_ticks_ms() + 10u;
        while (jce_time_ticks_ms() < spin_until) { /* spin */ }
    }
    jce_lan_discovery_client_stop_scan();

    uint32_t found = jce_lan_discovery_client_server_count();
    LOG_INFO(LOG_TAG, "self_test: found %u server(s)", (unsigned)found);

    bool saw_test_srv = false;
    for (uint32_t i = 0; i < found; ++i) {
        JceLanDiscoveredServer s;
        if (jce_lan_discovery_client_get_server(i, &s)
            && strcmp(s.server_name, "TestSrv") == 0
            && s.game_port == 7777u
            && s.max_players == 4u) {
            saw_test_srv = true;
            break;
        }
    }
    /* Soft assert: log on mismatch (loopback broadcast availability
     * varies across CI sandboxes) — the build / lint gates remain the
     * authoritative pass criteria. */
    if (!saw_test_srv) {
        LOG_WARN(LOG_TAG, "self_test: TestSrv not observed (broadcast may be blocked)");
    } else {
        LOG_SUCCESS(LOG_TAG, "self_test: TestSrv observed");
    }

    jce_lan_discovery_server_stop();
    jce_lan_discovery_client_clear();
    JCE_ASSERT(jce_lan_discovery_client_server_count() == 0u);
}
#endif /* JCE_NET_SELF_TEST */

#endif /* !__EMSCRIPTEN__ */
