/* jce_aid_chain.c -- tier selection and request-level cascade (spec F.5).
 *
 * T1 cloud gateway -> T2 local inference -> T3 deterministic generator.
 * All three produce ISOMORPHIC records: same schema, same wire format,
 * same solver downstream -- game logic never learns the source.  The
 * terminal tier cannot fail, so acquisition as a whole cannot fail.
 *
 * Stamping: record_id/tick are pre-assigned on the MAIN thread at
 * request time (JceAidStamp) so the worker never touches shared
 * counters; the seed is derived engine-side as hash64(record_id ^ tick)
 * and NEVER comes from the network (spec F.1). */
#include "jce_aid_transport.h"

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_timer.h>

#define AID_DEFAULT_LOCAL_URL "http://127.0.0.1:11434/v1/chat/completions"

void jce_aid_set_transport(const JceAidTransport* t)
{
    JceAidState* st = jce_aid_state();
    if (st) st->transport = t;
}

const JceAidTransport* jce_aid_get_transport(void)
{
    JceAidState* st = jce_aid_state();
    return st ? (const JceAidTransport*)st->transport : NULL;
}

uint32_t jce_aid_debug_tier_calls(uint8_t tier)
{
    JceAidState* st = jce_aid_state();
    return (st && tier >= 1 && tier <= 3) ? st->tier_calls[tier] : 0;
}

static uint64_t chain_now_ms(JceAidState* st)
{
    return st->now_ms ? st->now_ms() : jce_time_ticks_ms();
}

/* circuit cache: a tier that just failed is skipped for health_ttl_ms */
static int tier_open(JceAidState* st, int tier)
{
    uint64_t last = st->tier_fail_ms[tier];
    if (!last) return 1;
    return chain_now_ms(st) - last >= st->health_ttl_ms;
}

static void tier_mark_fail(JceAidState* st, int tier)
{
    uint64_t now = chain_now_ms(st);
    st->tier_fail_ms[tier] = now ? now : 1;
}

static void tier_mark_ok(JceAidState* st, int tier)
{
    st->tier_fail_ms[tier] = 0;
}

/* spec F.5 mem floor: approximate available memory as total RAM minus
 * this process's RSS (the engine has no OS free-memory primitive) and
 * drop straight to T3 below the configured floor.  dbg_* fields let
 * tests inject both terms. */
static int chain_mem_low(JceAidState* st)
{
    uint32_t total_mb, rss_mb;
    if (st->cfg.mem_floor_mb == 0) return 0;

    if (st->dbg_mem_total_mb) {
        total_mb = st->dbg_mem_total_mb;
        rss_mb   = st->dbg_mem_rss_mb;
    } else {
        JceMemStats ms;
        if (!st->mem_total_mb_cache) {
            JceSysInfo si;
            jce_sysinfo_init(&si);
            st->mem_total_mb_cache =
                si.ram_total_mb > 0 ? (uint32_t)si.ram_total_mb : 1u;
        }
        total_mb = st->mem_total_mb_cache;
        rss_mb   = jce_mem_stats(&ms)
                       ? (uint32_t)(ms.current_rss / (1024u * 1024u))
                       : 0u;
    }
    {
        uint32_t avail_mb = total_mb > rss_mb ? total_mb - rss_mb : 0u;
        return avail_mb < st->cfg.mem_floor_mb;
    }
}

/* ---- network tiers ----------------------------------------------------- */

static JceAidResult t1_acquire(JceAidState* st, const JceAidSchema* s,
                               const JceAidContextKV* kv, uint32_t kv_count,
                               uint8_t* payload, uint16_t* out_len)
{
    const JceAidTransport* tr = (const JceAidTransport*)st->transport;
    char*        body;
    char*        rsp = NULL;
    size_t       rsp_len = 0;
    int          status = 0;
    JceAidResult r = JCE_AID_ERR_BAD_FORMAT;

    st->tier_calls[1]++;
    if (!tr || !st->cfg_gateway_url) return JCE_AID_ERR_NOT_FOUND;

    body = jce_aid_json_build_request(s, kv, kv_count,
                                      st->cfg.t1_timeout_ms);
    if (!body) return JCE_AID_ERR_LIMIT;

    {
        /* url = {base}/v1/constraint-requests */
        size_t n = strlen(st->cfg_gateway_url);
        char*  url = (char*)jce_aid_malloc(n + 32);
        if (!url) { jce_aid_free(body); return JCE_AID_ERR_LIMIT; }
        memcpy(url, st->cfg_gateway_url, n);
        memcpy(url + n, "/v1/constraint-requests", 24);
        if (tr->post_json(tr->self, url, st->cfg_gateway_token, body,
                          st->cfg.t1_timeout_ms, &status, &rsp,
                          &rsp_len) == 0 && status == 200 && rsp) {
            r = jce_aid_json_parse_fields(s, rsp, rsp_len, payload, out_len);
        }
        jce_aid_free(url);
    }
    if (rsp) jce_aid_free(rsp);
    jce_aid_free(body);
    return r;
}

static JceAidResult t2_acquire(JceAidState* st, const JceAidSchema* s,
                               const JceAidContextKV* kv, uint32_t kv_count,
                               uint8_t* payload, uint16_t* out_len)
{
    const JceAidTransport* tr = (const JceAidTransport*)st->transport;
    const char* url =
        st->cfg_local_url ? st->cfg_local_url : AID_DEFAULT_LOCAL_URL;
    char*        body;
    int          attempt;
    JceAidResult r = JCE_AID_ERR_BAD_FORMAT;

    st->tier_calls[2]++;
    if (!tr) return JCE_AID_ERR_NOT_FOUND;

    body = jce_aid_json_build_chat_request(s, kv, kv_count,
                                           st->cfg_local_model);
    if (!body) return JCE_AID_ERR_LIMIT;

    /* parse failure retries once (spec F.3) */
    for (attempt = 0; attempt < 2; ++attempt) {
        char*  rsp = NULL;
        size_t rsp_len = 0;
        int    status = 0;
        if (tr->post_json(tr->self, url, NULL, body, st->cfg.t2_timeout_ms,
                          &status, &rsp, &rsp_len) != 0) {
            if (rsp) jce_aid_free(rsp);
            break; /* transport failure: no retry, cascade */
        }
        if (status == 200 && rsp)
            r = jce_aid_json_parse_chat_fields(s, rsp, rsp_len, payload,
                                               out_len);
        if (rsp) jce_aid_free(rsp);
        if (r == JCE_AID_OK) break;
    }
    jce_aid_free(body);
    return r;
}

/* ---- cascade ------------------------------------------------------------ */

JceAidResult jce_aid_chain_acquire(const JceAidSchema* s,
                                   const JceAidContextKV* kv,
                                   uint32_t kv_count,
                                   const JceAidStamp* stamp,
                                   JceAidRecord* out)
{
    JceAidState* st = jce_aid_state();
    JceAidMode   mode;
    uint8_t      tier = 3;
    JceAidResult r = JCE_AID_ERR_BAD_FORMAT;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!s || !stamp || !out) return JCE_AID_ERR_INVALID_ARG;
    JCE_AID_ASSERT(!st->replay_active);
    if (st->replay_active) return JCE_AID_ERR_INVALID_ARG;

    mode = st->cfg.mode;
    memset(out, 0, sizeof(*out));

    /* low memory: skip the network tiers entirely (spec F.5) */
    if (mode == JCE_AID_MODE_AUTO && chain_mem_low(st))
        mode = JCE_AID_MODE_FORCE_T3;

    if (mode == JCE_AID_MODE_AUTO || mode == JCE_AID_MODE_FORCE_T1) {
        if (mode == JCE_AID_MODE_FORCE_T1 || tier_open(st, 1)) {
            r = t1_acquire(st, s, kv, kv_count, out->payload,
                           &out->payload_len);
            if (r == JCE_AID_OK) { tier = 1; tier_mark_ok(st, 1); }
            else tier_mark_fail(st, 1);
        }
    }
    if (r != JCE_AID_OK &&
        (mode == JCE_AID_MODE_AUTO || mode == JCE_AID_MODE_FORCE_T2)) {
        if (mode == JCE_AID_MODE_FORCE_T2 || tier_open(st, 2)) {
            r = t2_acquire(st, s, kv, kv_count, out->payload,
                           &out->payload_len);
            if (r == JCE_AID_OK) { tier = 2; tier_mark_ok(st, 2); }
            else tier_mark_fail(st, 2);
        }
    }
    if (r != JCE_AID_OK) {
        /* terminal tier: deterministic generator, cannot fail.  Its
         * sampling seed IS the record seed (spec E.2). */
        uint64_t mix  = stamp->record_id ^ stamp->tick;
        uint64_t seed = jce_aid_hash64(&mix, 8); /* spec F.1 formula */
        uint16_t cv   = 0;
        st->tier_calls[3]++;
        r = jce_aid_tier3_fill(s, seed, out->payload, &out->payload_len, &cv);
        if (r != JCE_AID_OK) return r; /* only on invalid schema */
        tier = 3;
        out->seed = seed;
        out->calib_ver = cv;
    } else {
        /* engine-side seed stamping for network tiers (spec F.1): derived
         * from engine state, never from the network. */
        uint64_t mix = stamp->record_id ^ stamp->tick;
        out->seed = jce_aid_hash64(&mix, 8);
        out->calib_ver = 0; /* network payloads owe nothing to any table */
    }

    out->schema_id  = jce_aid_schema_id(s->name, s->version);
    out->schema_ver = s->version;
    out->record_id  = stamp->record_id;
    out->tick       = stamp->tick;
    out->tier       = tier;
    out->solver_ver = (uint16_t)JCE_AID_SOLVER_VERSION;
    jce_aid_stat_inc(&st->acquire_count);
    return JCE_AID_OK;
}
