/*
 * jce_lan_discovery.h  P3-D.5 LAN host discovery (UDP broadcast over ENet).
 *
 * Unity NetworkDiscovery parity: servers beacon-on-request on a well-known
 * UDP port (default 47775); clients broadcast a discovery REQ to
 * 255.255.255.255 and accumulate RESP packets for a few seconds.
 *
 * Layer: L4 (middleware/net).  Uses ENet's socket layer (`enet_socket_*`)
 * exclusively — no platform sockets, no <winsock2.h> / <sys/socket.h>.
 *
 * TODO: most LANs forward broadcasts only within a single subnet, and a
 * few corporate / VPN networks block UDP broadcast entirely.  A direct-IP
 * "Join by address" entry remains the recommended fallback in the UI.
 */

#ifndef JCE_LAN_DISCOVERY_H
#define JCE_LAN_DISCOVERY_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_LAN_DISCOVERY_DEFAULT_PORT 47775u
#define JCE_LAN_DISCOVERY_MAX_SERVERS  32u
#define JCE_LAN_DISCOVERY_NAME_LEN     64u
#define JCE_LAN_PROTOCOL_VERSION       1u

typedef struct JceLanDiscoveredServer {
    char     server_name[JCE_LAN_DISCOVERY_NAME_LEN];
    char     address_text[64];        /* "192.168.1.10:7777" */
    uint32_t game_port;
    uint16_t current_players;
    uint16_t max_players;
    uint32_t protocol_version;
    int64_t  last_seen_ms;            /* jce_time_ticks_ms() at recv */
    int32_t  ping_ms;                 /* round-trip estimate; -1 unknown */
} JceLanDiscoveredServer;

/* ================================================================== */
/* SERVER — start beaconing                                            */
/* ================================================================== */

/* Open a UDP socket on `discovery_port` (0 → default 47775) and reply to
 * incoming discovery requests with (server_name, game_port, player count,
 * max_players, JCE_LAN_PROTOCOL_VERSION).  `game_port` is the actual
 * `jce_net_host_create()` listen port — the value clients connect to.
 *
 * Non-blocking.  The caller must invoke `jce_lan_discovery_server_tick()`
 * once per frame (e.g. from an EarlyUpdate player_loop callback wired
 * up at L5) to drain incoming requests and send replies.  Returns
 * false if the socket cannot be created or bound (port in use,
 * firewall, etc.).
 *
 * `server_name` is truncated to JCE_LAN_DISCOVERY_NAME_LEN-1 bytes. */
JCE_API bool JCE_CALL jce_lan_discovery_server_start(const char *server_name,
                                                     uint32_t game_port,
                                                     uint16_t current_players,
                                                     uint16_t max_players,
                                                     uint16_t discovery_port);

JCE_API void JCE_CALL jce_lan_discovery_server_stop(void);
JCE_API void JCE_CALL jce_lan_discovery_server_set_player_count(uint16_t current_players);
JCE_API bool JCE_CALL jce_lan_discovery_server_is_running(void);

/* Drain pending REQs and reply.  Must be called once per frame by the
 * application while the server is running. */
JCE_API void JCE_CALL jce_lan_discovery_server_tick(void);

/* ================================================================== */
/* CLIENT — scan for servers                                           */
/* ================================================================== */

/* Send a broadcast REQ to 255.255.255.255:`discovery_port` (0 → default).
 * Listens for RESPs for `duration_ms`; results accumulate in a fixed-cap
 * (JCE_LAN_DISCOVERY_MAX_SERVERS) table, deduped by (address, game_port).
 * Call `jce_lan_discovery_client_tick()` each frame while scanning. */
JCE_API bool JCE_CALL jce_lan_discovery_client_start_scan(uint16_t discovery_port,
                                                          uint32_t duration_ms);
JCE_API void JCE_CALL jce_lan_discovery_client_tick(void);
JCE_API void JCE_CALL jce_lan_discovery_client_stop_scan(void);
JCE_API bool JCE_CALL jce_lan_discovery_client_is_scanning(void);

JCE_API uint32_t JCE_CALL jce_lan_discovery_client_server_count(void);
JCE_API bool     JCE_CALL jce_lan_discovery_client_get_server(uint32_t idx,
                                                              JceLanDiscoveredServer *out);
JCE_API void     JCE_CALL jce_lan_discovery_client_clear(void);

/* STATUS: NEVER COMPILED.  JCE_NET_SELF_TEST is not defined by any
 * CMakeLists, cmake module or build script in this repository, so this
 * routine — and the matching one in jce_session.c — is unconditionally
 * preprocessed away in every configuration we ship or test.
 *
 * This matters when reading the tree: the presence of a "self test" here
 * suggests LAN discovery has automated coverage.  It does not.  Even when
 * compiled in, the routine deliberately soft-asserts (it logs on mismatch
 * rather than failing) because loopback broadcast availability varies across
 * CI sandboxes, so its result was never authoritative either.
 *
 * Turning it on means accepting a ~600 ms busy-wait spin and real UDP
 * broadcast in the build — which is exactly why it was left off.  If LAN
 * discovery needs coverage, the honest place is tests/ with the broadcast
 * transport faked, not this macro. */
#ifdef JCE_NET_SELF_TEST
JCE_API void JCE_CALL jce_lan_discovery_self_test(void);
#endif

JCE_EXTERN_C_END

#endif /* JCE_LAN_DISCOVERY_H */
