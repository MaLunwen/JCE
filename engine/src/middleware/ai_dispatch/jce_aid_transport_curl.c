/* jce_aid_transport_curl.c -- production transport over libcurl
 * (spec D.1/F.4 primary path; owner-approved 2026-07-17).
 *
 * Covers T1 HTTPS (schannel on Windows, recipe default elsewhere) and
 * T2 localhost alike.  Compiles to an empty TU unless the build found
 * libcurl (JCE_AID_HAVE_CURL) so stale conan trees keep configuring and
 * fall back to the plaintext socket transport. */
#include "jce_aid_transport.h"

#if defined(JCE_AID_HAVE_CURL) && JCE_AID_HAVE_CURL

#include <curl/curl.h>

typedef struct CurlBody {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;
} CurlBody;

static size_t curl_sink(const void* ptr, size_t size, size_t nmemb,
                        void* user)
{
    CurlBody*    b = (CurlBody*)user;
    const size_t n = size * nmemb;
    if (b->oom) return 0;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        char*  nb;
        while (nc < b->len + n + 1) nc *= 2;
        if (nc > (1u << 20)) { b->oom = 1; return 0; } /* 1 MiB cap */
        nb = (char*)jce_aid_malloc(nc);
        if (!nb) { b->oom = 1; return 0; }
        if (b->data) { memcpy(nb, b->data, b->len); jce_aid_free(b->data); }
        b->data = nb;
        b->cap  = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

static int curl_post(void* self, const char* url, const char* bearer,
                     const char* body, uint32_t timeout_ms,
                     int* out_status, char** out_body, size_t* out_len)
{
    CURL*              h;
    struct curl_slist* headers = NULL;
    CurlBody           sink;
    long               status = 0;
    CURLcode           rc;
    (void)self;

    if (!url || !body) return 1;
    memset(&sink, 0, sizeof(sink));

    h = curl_easy_init();
    if (!h) return 1;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (bearer) {
        char auth[576];
        int  n = snprintf(auth, sizeof(auth), "Authorization: Bearer %s",
                          bearer);
        if (n > 0 && (size_t)n < sizeof(auth))
            headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS,
                     (long)(timeout_ms ? timeout_ms : 1500));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS,
                     (long)(timeout_ms ? timeout_ms : 1500));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L); /* no surprises */
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); /* builtin decoders */

    rc = curl_easy_perform(h);
    if (rc == CURLE_OK)
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK || sink.oom || status < 100 || status > 599) {
        if (sink.data) jce_aid_free(sink.data);
        return 1;
    }
    *out_status = (int)status;
    *out_body   = sink.data; /* may be NULL for empty bodies */
    *out_len    = sink.len;
    if (!sink.data) {
        /* normalise: hand back an owned empty string */
        char* empty = (char*)jce_aid_malloc(1);
        if (!empty) return 1;
        empty[0]  = 0;
        *out_body = empty;
        *out_len  = 0;
    }
    return 0;
}

static JceAidTransport g_curl = { curl_post, NULL };

int jce_aid_curl_install(void)
{
    static int global_ready = 0;
    if (!global_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return 0;
        global_ready = 1;
    }
    jce_aid_set_transport(&g_curl);
    return 1;
}

#else /* !JCE_AID_HAVE_CURL */

int jce_aid_curl_install(void)
{
    return 0; /* not built with libcurl: caller falls back */
}

#endif /* JCE_AID_HAVE_CURL */
