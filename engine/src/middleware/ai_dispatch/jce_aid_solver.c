/* jce_aid_solver.c -- deterministic solver (spec G): the final hop from
 * semantic constraints to engine parameter values.
 *
 * ALLOWED:   integers, Q16.16, PCG32, 64-bit hash.
 * FORBIDDEN: libm transcendentals, time(), rand(), order-dependent float
 *            reductions, any external state.
 *
 * v1 behaviour is FROZEN.  Any change bumps JCE_AID_SOLVER_VERSION and
 * adds a new switch case; old records keep the old path (spec E.3 --
 * never silently approximate). */
#include "jce_aid_internal.h"

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t* p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static JceAidResult solve_v1(const JceAidSchema* s, const JceAidRecord* rec,
                             JceAidSolved* out)
{
    uint32_t off = 0;
    out->field_count = s->field_count;
    for (uint16_t i = 0; i < s->field_count; ++i) {
        const JceAidField* f = &s->fields[i];
        JceAidSolvedField* o = &out->fields[i];
        o->type = f->type;
        switch (f->type) {
        case JCE_AID_F_I32: {
            const int32_t v = (int32_t)get_u32(rec->payload + off);
            o->v.i = jce_aid_clamp_i32(v, f->min_q, f->max_q);
            off += 4;
            break;
        }
        case JCE_AID_F_F32Q: {
            const int32_t q = (int32_t)get_u32(rec->payload + off);
            o->v.f = jce_aid_q16_to_f32(jce_aid_clamp_i32(q, f->min_q, f->max_q));
            off += 4;
            break;
        }
        case JCE_AID_F_ENUM: {
            const uint16_t idx = get_u16(rec->payload + off);
            o->v.enum_index = (uint16_t)(idx % f->enum_count);
            off += 2;
            break;
        }
        case JCE_AID_F_COLOR8: {
            o->v.color = get_u32(rec->payload + off);
            off += 4;
            break;
        }
        case JCE_AID_F_TAGSET: {
            const uint64_t mask = (f->enum_count >= 64)
                ? ~0ull : ((1ull << f->enum_count) - 1ull);
            o->v.tags = get_u64(rec->payload + off) & mask;
            off += 8;
            break;
        }
        default:
            return JCE_AID_ERR_BAD_FORMAT;
        }
    }
    return JCE_AID_OK;
}

JceAidResult JCE_CALL
jce_aid_solve(const JceAidRecord* rec, JceAidSolved* out)
{
    JceAidState*        st = jce_aid_state();
    const JceAidSchema* s;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!rec || !out) return JCE_AID_ERR_INVALID_ARG;

    s = jce_aid_schema_get(rec->schema_id);
    if (!s) return JCE_AID_ERR_NOT_FOUND;
    if (rec->payload_len != jce_aid_schema_payload_size(s))
        return JCE_AID_ERR_BAD_FORMAT;

    memset(out, 0, sizeof(*out));
    switch (rec->solver_ver) {
    case 1:  return solve_v1(s, rec, out);
    default: return JCE_AID_ERR_VERSION; /* never silently approximate */
    }
}

JceAidResult JCE_CALL
jce_aid_field_rng(const JceAidRecord* rec, const char* field_name, JceRng* out_rng)
{
    if (!rec || !field_name || !out_rng) return JCE_AID_ERR_INVALID_ARG;
    /* Per-instance expansion sub-stream (spec G): seed mixes the record
     * seed with the field name; stream selector is the schema id. */
    jce_rng_seed(out_rng, rec->seed ^ jce_aid_hash_str(field_name), rec->schema_id);
    return JCE_AID_OK;
}
