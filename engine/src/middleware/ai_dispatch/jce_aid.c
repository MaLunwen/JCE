/* jce_aid.c -- module facade: lifecycle, config, counted allocator. */
#include "jce_aid_internal.h"
#include "jce_aid_transport.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>

#include <stdlib.h>
#include <xxhash.h>

#define LOG_TAG "ai_dispatch"

static JceAidState g_aid; /* zero-init: .initialised == 0 */

JceAidState* jce_aid_state(void)
{
    return g_aid.initialised ? &g_aid : NULL;
}

void* jce_aid_malloc(size_t size)
{
    JceAidState* st = &g_aid;
    void* p = st->alloc.alloc(size, st->alloc.ctx);
    if (p) st->live_allocs++;
    return p;
}

void jce_aid_free(void* p)
{
    JceAidState* st = &g_aid;
    if (!p) return;
    st->alloc.free(p, st->alloc.ctx);
    JCE_AID_ASSERT(st->live_allocs > 0);
    st->live_allocs--;
}

char* jce_aid_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* d = (char*)jce_aid_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

uint64_t jce_aid_hash64(const void* data, size_t len)
{
    return (uint64_t)XXH3_64bits(data, len);
}

uint64_t jce_aid_hash_str(const char* s)
{
    return jce_aid_hash64(s, strlen(s));
}

JceAidResult JCE_CALL jce_aid_init(const JceAidConfig* cfg)
{
    if (g_aid.initialised) return JCE_AID_OK; /* idempotent */
    memset(&g_aid, 0, sizeof(g_aid));
    g_aid.alloc = jce_allocator_default();
    g_aid.initialised = 1; /* before strdup: the counted allocator needs it */
    if (cfg) g_aid.cfg = *cfg;
    if (g_aid.cfg.t1_timeout_ms == 0) g_aid.cfg.t1_timeout_ms = 1500;
    if (g_aid.cfg.t2_timeout_ms == 0) g_aid.cfg.t2_timeout_ms = 4000;
    g_aid.health_ttl_ms = 30000; /* spec F.5: probe cache TTL 30s */
    /* Deep-copy config strings: the caller's buffers may die while the
     * worker thread still reads them.  The token stays in memory only,
     * never persisted (spec K). */
    if (g_aid.cfg.gateway_url)
        g_aid.cfg_gateway_url = jce_aid_strdup(g_aid.cfg.gateway_url);
    if (g_aid.cfg.gateway_token)
        g_aid.cfg_gateway_token = jce_aid_strdup(g_aid.cfg.gateway_token);
    if (g_aid.cfg.local_url)
        g_aid.cfg_local_url = jce_aid_strdup(g_aid.cfg.local_url);
    if (g_aid.cfg.local_model)
        g_aid.cfg_local_model = jce_aid_strdup(g_aid.cfg.local_model);
    g_aid.stats_mutex = jce_mutex_create();
    /* Default transport preference: libcurl (HTTPS-capable) when built, else
     * the plaintext socket fallback; tests swap in the mock.
     *
     * The fallback is NARROWER, not equivalent: it speaks http:// only, so an
     * https:// endpoint stops working entirely rather than silently
     * downgrading to plaintext (parse_http_url rejects the scheme).  That is
     * the safe failure mode, but it presents to an operator as "every T1
     * request fails" with nothing pointing at the transport — so say so.
     * libcurl is an unconditional Conan requirement, which means reaching
     * here at all implies curl_global_init() failed at runtime. */
    if (!jce_aid_curl_install()) {
        jce_aid_http_install();
        LOG_WARN(LOG_TAG,
                 "libcurl transport unavailable (curl_global_init failed, or "
                 "built without JCE_AID_HAVE_CURL) — falling back to the "
                 "plaintext HTTP transport. http:// localhost/LAN inference "
                 "still works; every https:// endpoint will now fail.");
    }

    /* DEBUG TOGGLES mirroring JCE_INPUT_RECORD / JCE_INPUT_REPLAY */
    {
        const char* rec = getenv("JCE_AID_RECORD");
        const char* rep = getenv("JCE_AID_REPLAY");
        if (rep) {
            if (jce_aid_replay_open(rep) == JCE_AID_OK)
                LOG_INFO(LOG_TAG, "replaying records from %s", rep);
            else
                LOG_WARN(LOG_TAG, "JCE_AID_REPLAY: cannot open %s", rep);
        } else if (rec) {
            if (jce_aid_stream_record_open(rec) == JCE_AID_OK)
                LOG_INFO(LOG_TAG, "recording records to %s", rec);
            else
                LOG_WARN(LOG_TAG, "JCE_AID_RECORD: cannot open %s", rec);
        }
    }
    return JCE_AID_OK;
}

void JCE_CALL jce_aid_shutdown(void)
{
    if (!g_aid.initialised) return;
    jce_aid_queue_shutdown(); /* join the worker before anything dies */
    jce_aid_events_free_all();
    jce_aid_calib_free_all();
    jce_aid_schema_free_all();
    if (g_aid.stats_mutex) {
        jce_mutex_destroy((JceMutex*)g_aid.stats_mutex);
        g_aid.stats_mutex = NULL;
    }
    if (g_aid.cfg_gateway_url)   jce_aid_free(g_aid.cfg_gateway_url);
    if (g_aid.cfg_gateway_token) jce_aid_free(g_aid.cfg_gateway_token);
    if (g_aid.cfg_local_url)     jce_aid_free(g_aid.cfg_local_url);
    if (g_aid.cfg_local_model)   jce_aid_free(g_aid.cfg_local_model);
    JCE_AID_ASSERT(g_aid.live_allocs == 0);
    memset(&g_aid, 0, sizeof(g_aid));
}

uint64_t JCE_CALL jce_aid_debug_live_allocs(void)
{
    return g_aid.live_allocs;
}
