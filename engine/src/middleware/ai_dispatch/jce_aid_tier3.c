/* jce_aid_tier3.c -- tier-3 deterministic generator: uniform in-bounds
 * sampling from the schema, integer/PCG32 only.  NEVER fails once the
 * schema exists (spec C.4/J).  Calibration-table sampling arrives in P3
 * as an additive path keyed by calib_ver; this no-table path is frozen. */
#include "jce_aid_internal.h"

#include <jce/os/core/jce_fixed_clock.h>

static void put_le16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t* p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

JceAidResult jce_aid_tier3_fill(const JceAidSchema* s, uint64_t seed,
                                uint8_t* payload, uint16_t* out_len,
                                uint16_t* out_calib_ver)
{
    JceRng   rng;
    uint32_t off = 0;

    /* stream = schema id so two schemas never share a sequence */
    jce_rng_seed(&rng, seed, jce_aid_schema_id(s->name, s->version));

    /* With a fitted calibration table, sample from it (spec F.6).  The
     * table only shapes NEW generations: records are self-contained, so
     * replay of existing records never touches this path. */
    {
        const JceAidCalib* calib =
            jce_aid_calib_get(jce_aid_schema_id(s->name, s->version));
        if (calib &&
            jce_aid_calib_sample(s, calib, &rng, payload, out_len)) {
            if (out_calib_ver)
                *out_calib_ver = jce_aid_calib_table_version(calib);
            return JCE_AID_OK;
        }
    }
    if (out_calib_ver) *out_calib_ver = 0;

    for (uint16_t i = 0; i < s->field_count; ++i) {
        const JceAidField* f = &s->fields[i];
        switch (f->type) {
        case JCE_AID_F_I32:
        case JCE_AID_F_F32Q: {
            /* inclusive span <= 2^32; u64 draw keeps modulo bias < 2^-32 */
            const uint64_t span =
                (uint64_t)((int64_t)f->max_q - (int64_t)f->min_q) + 1u;
            const int32_t v =
                (int32_t)((int64_t)f->min_q + (int64_t)(jce_rng_u64(&rng) % span));
            put_le32(payload + off, (uint32_t)v);
            off += 4;
            break;
        }
        case JCE_AID_F_ENUM: {
            const uint16_t v = (uint16_t)(jce_rng_u64(&rng) % f->enum_count);
            put_le16(payload + off, v);
            off += 2;
            break;
        }
        case JCE_AID_F_COLOR8: {
            put_le32(payload + off, jce_rng_u32(&rng));
            off += 4;
            break;
        }
        case JCE_AID_F_TAGSET: {
            const uint64_t mask = (f->enum_count >= 64)
                ? ~0ull : ((1ull << f->enum_count) - 1ull);
            put_le64(payload + off, jce_rng_u64(&rng) & mask);
            off += 8;
            break;
        }
        default:
            return JCE_AID_ERR_INVALID_ARG;
        }
    }
    *out_len = (uint16_t)off;
    return JCE_AID_OK;
}

JceAidResult JCE_CALL
jce_aid_generate_local(JceAidSchemaId id, uint64_t seed, JceAidRecord* out)
{
    JceAidState*        st = jce_aid_state();
    const JceAidSchema* s;
    JceAidResult        r;
    uint64_t            tick;
    uint16_t            calib_ver = 0;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!out) return JCE_AID_ERR_INVALID_ARG;

    /* Replay never acquires: records are re-played and re-solved only
     * (spec H).  Assert in dev builds, hard-refuse everywhere. */
    JCE_AID_ASSERT(!st->replay_active);
    if (st->replay_active) return JCE_AID_ERR_INVALID_ARG;

    s = jce_aid_schema_get(id);
    if (!s) return JCE_AID_ERR_NOT_FOUND;

    memset(out, 0, sizeof(*out));
    r = jce_aid_tier3_fill(s, seed, out->payload, &out->payload_len,
                           &calib_ver);
    if (r != JCE_AID_OK) return r;

    tick = jce_fixed_clock_default()->tick_count;
    if (st->record_counter_tick != tick) {
        st->record_counter_tick = tick;
        st->record_counter      = 0;
    }
    out->record_id  = (tick << 16) | (uint64_t)st->record_counter++;
    out->tick       = tick;
    out->schema_id  = id;
    out->schema_ver = s->version;
    out->tier       = 3;
    out->solver_ver = (uint16_t)JCE_AID_SOLVER_VERSION;
    out->calib_ver  = calib_ver; /* provenance only -- solve never reads it */
    out->seed       = seed;
    jce_aid_stat_inc(&st->acquire_count);
    return JCE_AID_OK;
}
