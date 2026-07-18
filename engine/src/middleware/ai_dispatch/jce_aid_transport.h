/* jce_aid_transport.h -- transport vtable for the network tiers.
 * Implementations: jce_aid_transport_mock.c (scripted, tests) and
 * jce_aid_transport_http.c (minimal plaintext HTTP/1.1 over os TCP;
 * production T1 HTTPS waits on the libcurl approval, spec F.4).
 * Internal header -- not installed. */
#ifndef JCE_AID_TRANSPORT_H
#define JCE_AID_TRANSPORT_H

#include "jce_aid_internal.h"

typedef struct JceAidTransport {
    /* POST `body` (JSON) to `url`.  Returns 0 when a response was
     * received (any HTTP status; *out_status carries it and *out_body a
     * jce_aid_malloc'd copy the CALLER frees), nonzero on
     * connect/timeout/transport failure. */
    int (*post_json)(void* self, const char* url, const char* bearer,
                     const char* body, uint32_t timeout_ms,
                     int* out_status, char** out_body, size_t* out_len);
    void* self;
} JceAidTransport;

/* Install a transport (NULL restores "none": every network tier reports
 * transport failure and the chain cascades to T3). */
void jce_aid_set_transport(const JceAidTransport* t);
const JceAidTransport* jce_aid_get_transport(void);

/* ---- libcurl transport (jce_aid_transport_curl.c) --------------------- */
/* Production path: T1 HTTPS + T2 localhost.  Returns 0 when the build
 * lacks libcurl -- callers fall back to the socket transport. */
int jce_aid_curl_install(void);

/* ---- plaintext HTTP transport (jce_aid_transport_http.c) -------------- */
/* Fallback (spec F.4): localhost/LAN plaintext only. */
void jce_aid_http_install(void);

/* ---- mock transport (jce_aid_transport_mock.c) ----------------------- */
void     jce_aid_mock_install(void);
void     jce_aid_mock_reset(void);
/* FIFO script: each post_json call consumes one entry. */
void     jce_aid_mock_push_response(int status, const char* body,
                                    uint32_t delay_ms);
void     jce_aid_mock_push_fail(uint32_t delay_ms);
uint32_t jce_aid_mock_post_calls(void);

/* ---- chain (jce_aid_chain.c) ------------------------------------------ */
typedef struct JceAidStamp {
    uint64_t record_id;  /* pre-assigned on the main thread (request time) */
    uint64_t tick;       /* fixed-clock tick at request time */
} JceAidStamp;

/* Synchronous acquisition through the tier chain (worker-thread body;
 * unit tests call it directly).  NEVER fails once the schema exists:
 * the terminal tier is the deterministic generator. */
JceAidResult jce_aid_chain_acquire(const JceAidSchema* s,
                                   const JceAidContextKV* kv,
                                   uint32_t kv_count,
                                   const JceAidStamp* stamp,
                                   JceAidRecord* out);

/* per-tier acquisition counters (probe for tests / stats) */
uint32_t jce_aid_debug_tier_calls(uint8_t tier);

#endif /* JCE_AID_TRANSPORT_H */
