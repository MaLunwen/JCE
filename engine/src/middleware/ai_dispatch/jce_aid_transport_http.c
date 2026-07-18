/* jce_aid_transport_http.c -- minimal plaintext HTTP/1.0 transport over
 * jce_tcp (spec F.4 fallback path while libcurl awaits approval).
 *
 * Scope: http:// only, IPv4, Content-Length or connection-close bodies
 * (HTTP/1.0 request => servers do not chunk).  Intended for T2 localhost
 * inference and development-only T1; production T1 HTTPS arrives with
 * the curl transport. */
#include "jce_aid_transport.h"

#include <jce/os/platform/jce_tcp.h>

#include <stdio.h>

#define HTTP_MAX_RESPONSE (1u << 20) /* 1 MiB cap */

typedef struct UrlParts {
    char     host[128];
    uint16_t port;
    char     path[256];
} UrlParts;

static int parse_http_url(const char* url, UrlParts* out)
{
    const char* p;
    const char* host_end;
    size_t      n;

    if (!url || strncmp(url, "http://", 7) != 0) return 0;
    p = url + 7;
    host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    n = (size_t)(host_end - p);
    if (n == 0 || n >= sizeof(out->host)) return 0;
    memcpy(out->host, p, n);
    out->host[n] = 0;

    out->port = 80;
    if (*host_end == ':') {
        unsigned v = 0;
        host_end++;
        while (*host_end >= '0' && *host_end <= '9') {
            v = v * 10u + (unsigned)(*host_end - '0');
            host_end++;
        }
        if (!v || v > 65535u) return 0;
        out->port = (uint16_t)v;
    }
    if (*host_end == '/')
        snprintf(out->path, sizeof(out->path), "%s", host_end);
    else
        snprintf(out->path, sizeof(out->path), "/");
    return 1;
}

static int http_post(void* self, const char* url, const char* bearer,
                     const char* body, uint32_t timeout_ms,
                     int* out_status, char** out_body, size_t* out_len)
{
    UrlParts u;
    JceTcp*  c;
    char     header[640];
    int      hlen;
    char*    rsp = NULL;
    size_t   rsp_len = 0, rsp_cap = 0;
    int      ok = 1;
    (void)self;

    if (!parse_http_url(url, &u) || !body) return 1;
    if (timeout_ms == 0) timeout_ms = 1500;

    c = jce_tcp_connect(u.host, u.port, timeout_ms);
    if (!c) return 1;

    hlen = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.0\r\n"
                    "Host: %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %u\r\n"
                    "%s%s%s"
                    "Connection: close\r\n\r\n",
                    u.path, u.host, (unsigned)strlen(body),
                    bearer ? "Authorization: Bearer " : "",
                    bearer ? bearer : "",
                    bearer ? "\r\n" : "");
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) { jce_tcp_close(c); return 1; }

    if (!jce_tcp_send_all(c, header, (size_t)hlen, timeout_ms) ||
        !jce_tcp_send_all(c, body, strlen(body), timeout_ms)) {
        jce_tcp_close(c);
        return 1;
    }

    /* read until close (HTTP/1.0) */
    for (;;) {
        char chunk[4096];
        int  n = jce_tcp_recv(c, chunk, sizeof(chunk), timeout_ms);
        if (n < 0) { ok = 0; break; }  /* timeout/error */
        if (n == 0) break;             /* closed: done */
        if (rsp_len + (size_t)n + 1 > HTTP_MAX_RESPONSE) { ok = 0; break; }
        if (rsp_len + (size_t)n + 1 > rsp_cap) {
            size_t nc = rsp_cap ? rsp_cap * 2 : 8192;
            char*  nb;
            while (nc < rsp_len + (size_t)n + 1) nc *= 2;
            nb = (char*)jce_aid_malloc(nc);
            if (!nb) { ok = 0; break; }
            if (rsp) { memcpy(nb, rsp, rsp_len); jce_aid_free(rsp); }
            rsp = nb;
            rsp_cap = nc;
        }
        memcpy(rsp + rsp_len, chunk, (size_t)n);
        rsp_len += (size_t)n;
    }
    jce_tcp_close(c);
    if (!ok || !rsp) {
        if (rsp) jce_aid_free(rsp);
        return 1;
    }
    rsp[rsp_len] = 0;

    /* status line: HTTP/1.x NNN ... */
    {
        int status = 0;
        if (rsp_len > 12 && strncmp(rsp, "HTTP/1.", 7) == 0)
            status = (rsp[9] - '0') * 100 + (rsp[10] - '0') * 10 +
                     (rsp[11] - '0');
        if (status < 100 || status > 599) { jce_aid_free(rsp); return 1; }
        *out_status = status;
    }

    /* body = after the first \r\n\r\n */
    {
        const char* sep = strstr(rsp, "\r\n\r\n");
        if (!sep) { jce_aid_free(rsp); return 1; }
        sep += 4;
        {
            size_t blen = rsp_len - (size_t)(sep - rsp);
            char*  bcopy = (char*)jce_aid_malloc(blen + 1);
            if (!bcopy) { jce_aid_free(rsp); return 1; }
            memcpy(bcopy, sep, blen);
            bcopy[blen] = 0;
            *out_body = bcopy;
            *out_len  = blen;
        }
        jce_aid_free(rsp);
    }
    return 0;
}

static JceAidTransport g_http = { http_post, NULL };

void jce_aid_http_install(void)
{
    jce_aid_set_transport(&g_http);
}
