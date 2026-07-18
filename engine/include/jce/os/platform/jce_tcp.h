/*
 * jce_tcp.h  Minimal blocking TCP client/server with millisecond
 * deadlines.
 *
 * Motivation: the engine had no TCP primitive (net middleware rides
 * enet/UDP).  First consumer is the ai_dispatch plaintext HTTP/1.0
 * transport (localhost/LAN inference endpoints); the listen/accept pair
 * exists mainly so tests can stand up loopback peers.
 *
 * All calls are blocking with explicit timeouts (select-based).  IPv4
 * only, numeric addresses or resolvable hostnames.  Not thread-safe per
 * connection; distinct connections may live on distinct threads.
 *
 * Layer: OS / Platform (platform symbols live in the .c only).
 */
#ifndef JCE_TCP_H
#define JCE_TCP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTcp JceTcp;

/* Connect to host:port within timeout_ms.  NULL on failure/timeout. */
JCE_API JceTcp* JCE_CALL
jce_tcp_connect(const char* host, uint16_t port, uint32_t timeout_ms);

/* Send the whole buffer (looping) within timeout_ms. */
JCE_API bool JCE_CALL
jce_tcp_send_all(JceTcp* t, const void* data, size_t n, uint32_t timeout_ms);

/* Receive up to cap bytes.  >0 = bytes read, 0 = peer closed,
 * <0 = error or timeout. */
JCE_API int JCE_CALL
jce_tcp_recv(JceTcp* t, void* buf, size_t cap, uint32_t timeout_ms);

JCE_API void JCE_CALL jce_tcp_close(JceTcp* t);

/* Test/loopback helpers: bind+listen on 127.0.0.1:port (port 0 = ephemeral;
 * query with jce_tcp_bound_port), accept one connection. */
JCE_API JceTcp*  JCE_CALL jce_tcp_listen(uint16_t port);
JCE_API uint16_t JCE_CALL jce_tcp_bound_port(JceTcp* server);
JCE_API JceTcp*  JCE_CALL jce_tcp_accept(JceTcp* server, uint32_t timeout_ms);

JCE_EXTERN_C_END

#endif /* JCE_TCP_H */
