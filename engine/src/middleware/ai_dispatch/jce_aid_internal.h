/* jce_aid_internal.h -- shared internals of the ai_dispatch module.
 * Not installed; include only from jce_aid_*.c (and whitebox tests). */
#ifndef JCE_AID_INTERNAL_H
#define JCE_AID_INTERNAL_H

#include <jce/middleware/ai_dispatch/jce_ai_dispatch.h>
#include <jce/os/core/jce_allocator.h>

#include <assert.h>
#include <string.h>

#define JCE_AID_ASSERT(x) assert(x)

/* ---- Q16.16 fixed point (engine has no shared fixed-point util) ----- */
typedef int32_t jce_aid_q16;

#define JCE_AID_Q16_ONE 0x00010000

JCE_INLINE jce_aid_q16 jce_aid_q16_from_int(int32_t i)
{
    return (jce_aid_q16)((uint32_t)i << 16);
}

/* Exact and unambiguous under IEEE754: int32 -> double is exact, the
 * multiply by 2^-16 is exact, double -> float is one correctly-rounded
 * step.  This is the ONLY float producer in the module (spec C.3/G). */
JCE_INLINE float jce_aid_q16_to_f32(jce_aid_q16 q)
{
    return (float)((double)q * (1.0 / 65536.0));
}

JCE_INLINE jce_aid_q16 jce_aid_q16_mul(jce_aid_q16 a, jce_aid_q16 b)
{
    return (jce_aid_q16)(((int64_t)a * (int64_t)b) >> 16);
}

JCE_INLINE int32_t jce_aid_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- registry entry -------------------------------------------------- */
typedef struct JceAidSchemaSlot {
    JceAidSchemaId id;
    JceAidSchema   schema;   /* deep copy: name/fields/enum_names owned */
} JceAidSchemaSlot;

/* ---- module singleton ------------------------------------------------ */
typedef struct JceAidState {
    int               initialised;
    JceAidConfig      cfg;
    jce_allocator_t   alloc;
    uint64_t          live_allocs;   /* alloc minus free (leak probe) */
    JceAidSchemaSlot* slots;
    uint32_t          slot_count;
    uint32_t          slot_cap;
    uint16_t          record_counter;      /* lo 16 bits of record_id */
    uint64_t          record_counter_tick; /* tick the counter belongs to */

    /* P1: session record log (encoded records, drop-oldest ring) */
    uint8_t*     log_buf;   /* JCE_AID_LOG_CAP x JCE_AID_MAX_RECORD_SIZE */
    uint16_t*    log_len;   /* per-slot encoded length */
    uint32_t     log_head, log_count;
    uint64_t     log_dropped;
    int          log_warned;
    uint64_t     acquire_count;
    uint64_t     sanitize_clamps;   /* spec K: boundary clamps */
    int          replay_active;
    void*        stream_file;        /* FILE*, record mode */
    void*        replay_file;        /* FILE*, replay mode */
    JceAidRecord replay_pending;     /* decoded lookahead */
    uint16_t     replay_pending_len; /* encoded length of the lookahead */
    uint8_t      replay_pending_buf[JCE_AID_MAX_RECORD_SIZE];
    int          replay_has_pending;

    /* P2: transport + chain */
    const void*  transport;          /* JceAidTransport*, NULL = none */
    uint64_t     (*now_ms)(void);    /* injected time source (tests) */
    uint64_t     tier_fail_ms[4];    /* circuit cache: last failure, [1..3] */
    uint32_t     health_ttl_ms;      /* skip-tier window, default 30000 */
    uint32_t     tier_calls[4];      /* per-tier acquisition attempts */
    char*        cfg_gateway_url;    /* deep copies of config strings */
    char*        cfg_gateway_token;
    char*        cfg_local_url;
    char*        cfg_local_model;

    /* P2: async queue */
    void*        stats_mutex;        /* JceMutex*: cross-thread counters */
    uint64_t     generation;         /* world/scene token (main thread) */
    void*        bound_bus;          /* jce_event_bus* used by pump */

    /* P3: calibration */
    void*        calib_head;         /* JceAidCalib* linked list */
    char*        calib_dir;          /* owned; NULL = "." */

    /* mem-floor tier gate (spec F.5); dbg_* are test overrides (0=real) */
    uint32_t     mem_total_mb_cache;
    uint32_t     dbg_mem_total_mb;
    uint32_t     dbg_mem_rss_mb;
} JceAidState;

#define JCE_AID_LOG_CAP 256u

JceAidState* jce_aid_state(void);       /* NULL when not initialised */

void* jce_aid_malloc(size_t size);      /* counted wrappers */
void  jce_aid_free(void* p);
char* jce_aid_strdup(const char* s);

/* 64-bit hash used across the module (spec says "xxh64"; the engine
 * convention is XXH3_64bits -- locked forever once fmt=1 shipped). */
uint64_t jce_aid_hash64(const void* data, size_t len);
uint64_t jce_aid_hash_str(const char* s);

/* payload size for one field / a whole schema (0 = invalid schema) */
uint32_t jce_aid_field_wire_size(JceAidFieldType t);
uint32_t jce_aid_schema_payload_size(const JceAidSchema* s);

/* registry teardown (called from jce_aid_shutdown) */
void jce_aid_schema_free_all(void);

/* event/stream teardown (called from jce_aid_shutdown) */
void jce_aid_events_free_all(void);

/* session-log accessors (used by the save provider) */
const uint8_t* jce_aid_log_entry(uint32_t index, uint16_t* out_len);
void           jce_aid_log_reset(void);
int            jce_aid_log_append_encoded(const uint8_t* bytes, uint16_t len);

/* cross-thread stat counters (worker + main thread) */
void     jce_aid_stat_inc(uint64_t* counter);
uint64_t jce_aid_stat_get(const uint64_t* counter);

/* queue teardown (join worker; called from jce_aid_shutdown FIRST) */
void jce_aid_queue_shutdown(void);

/* internal: fill payload (tier3).  With a fitted calibration table the
 * table path samples truncated distributions / frequency roulettes;
 * without one the FROZEN uniform-bounds path runs and *out_calib_ver is
 * 0.  The P2 chain's T3 provider and jce_aid_generate_local share this
 * single implementation -- they must never diverge. */
JceAidResult jce_aid_tier3_fill(const JceAidSchema* s, uint64_t seed,
                                uint8_t* payload, uint16_t* out_len,
                                uint16_t* out_calib_ver);

/* ---- P3: calibration internals (jce_aid_calib.c) ---------------------- */
typedef struct JceAidCalib JceAidCalib;
JceAidCalib* jce_aid_calib_get(JceAidSchemaId id); /* NULL = none */
uint16_t     jce_aid_calib_table_version(const JceAidCalib* c);
/* Sample one payload from the fitted table (integer + PCG32 only).
 * Returns 0 when the calib has no fitted table (caller uses uniform). */
int jce_aid_calib_sample(const JceAidSchema* s, const JceAidCalib* c,
                         JceRng* rng, uint8_t* payload, uint16_t* out_len);
void jce_aid_calib_free_all(void);

/* ---- P2: JSON boundary (jce_aid_json.c, cJSON) ----------------------- */
/* Build the T1 constraint-request body / the T2 OpenAI-compatible chat
 * body.  Returned strings are jce_aid_malloc'd; caller jce_aid_free. */
char* jce_aid_json_build_request(const JceAidSchema* s,
                                 const JceAidContextKV* kv, uint32_t kv_count,
                                 uint32_t budget_ms);
char* jce_aid_json_build_chat_request(const JceAidSchema* s,
                                      const JceAidContextKV* kv,
                                      uint32_t kv_count, const char* model);
/* Parse a T1 response ({"fields":{...}}) into a payload under spec-K
 * sanitising (clamp+count / defaults / ignore extras).  BAD_FORMAT means
 * "cascade to the next tier". */
JceAidResult jce_aid_json_parse_fields(const JceAidSchema* s,
                                       const char* json, size_t len,
                                       uint8_t* payload, uint16_t* out_len);
/* Parse a T2 chat response: choices[0].message.content is itself a JSON
 * object of field values (bare or wrapped in "fields"). */
JceAidResult jce_aid_json_parse_chat_fields(const JceAidSchema* s,
                                            const char* json, size_t len,
                                            uint8_t* payload,
                                            uint16_t* out_len);

#endif /* JCE_AID_INTERNAL_H */
