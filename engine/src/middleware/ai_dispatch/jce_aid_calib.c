/* jce_aid_calib.c -- calibration: reservoir storage + integer fit +
 * versioned table persistence (spec F.6).
 *
 * ★确定性说明(规范 F.6,原样保留):记录自包含 payload;回放与联机只重放
 * 记录 + 求解,从不重新生成——因此校准表的任何漂移都不影响既有记录的
 * 重放结果,表只影响"未来的新生成"。
 *
 * Everything here is integer + PCG32: fit computes Q16.16-unit means,
 * integer stddev (Newton isqrt) and seen-range truncation for numeric
 * fields, frequency tables for ENUM/TAGSET; sampling uses a 12-draw
 * Irwin-Hall approximate normal, frequency roulettes, per-bit
 * thresholds and empirical row picks (COLOR8).  File format JCAL v1
 * with a trailing 64-bit checksum; corrupt or schema-mismatched files
 * are rejected and T3 falls back to the frozen uniform path. */
#include "jce_aid_internal.h"

#include <stdio.h>

#define AID_CALIB_CAP     256u
#define AID_CALIB_FILE_V  1u

typedef struct CalibNumeric {
    int32_t mean, stddev, lo, hi;  /* value units (i32 raw or Q16.16) */
} CalibNumeric;

typedef struct CalibField {
    JceAidFieldType type;
    CalibNumeric    num;         /* I32/F32Q */
    uint32_t*       freq;        /* ENUM/TAGSET: enum_count entries */
    uint64_t        freq_total;  /* ENUM: sum(freq); TAGSET: sample count */
} CalibField;

struct JceAidCalib {
    struct JceAidCalib* next;
    JceAidSchemaId      schema_id;
    uint16_t            calib_ver;   /* 0 = no fitted table yet */
    uint32_t            payload_size;
    uint16_t            field_count;
    CalibField*         fields;      /* NULL until fit/load */
    /* reservoir */
    uint8_t*            reservoir;   /* AID_CALIB_CAP x payload_size */
    uint16_t            count;
    uint64_t            seen;
    JceRng              rng;
};

/* ---- helpers ------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t* p)
{
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}
static void wr_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr_u64(uint8_t* p, uint64_t v)
{
    wr_u32(p, (uint32_t)v);
    wr_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t isqrt64(uint64_t v)
{
    uint64_t x = v, y = 0;
    if (v == 0) return 0;
    /* initial guess: 2^(ceil(bits/2)) */
    {
        int bits = 0;
        uint64_t t = v;
        while (t) { bits++; t >>= 1; }
        x = 1ull << ((bits + 1) / 2);
    }
    for (;;) {
        y = (x + v / x) >> 1;
        if (y >= x) break;
        x = y;
    }
    return x;
}

JceAidCalib* jce_aid_calib_get(JceAidSchemaId id)
{
    JceAidState* st = jce_aid_state();
    JceAidCalib* c;
    if (!st) return NULL;
    for (c = (JceAidCalib*)st->calib_head; c; c = c->next)
        if (c->schema_id == id) return c;
    return NULL;
}

uint16_t jce_aid_calib_table_version(const JceAidCalib* c)
{
    return c ? c->calib_ver : 0;
}

static void calib_free_fields(JceAidCalib* c)
{
    if (!c->fields) return;
    {
        uint16_t i;
        for (i = 0; i < c->field_count; ++i)
            if (c->fields[i].freq) jce_aid_free(c->fields[i].freq);
    }
    jce_aid_free(c->fields);
    c->fields = NULL;
}

static void calib_destroy(JceAidCalib* c)
{
    calib_free_fields(c);
    if (c->reservoir) jce_aid_free(c->reservoir);
    jce_aid_free(c);
}

void jce_aid_calib_free_all(void)
{
    JceAidState* st = jce_aid_state();
    JceAidCalib* c;
    if (!st) return;
    c = (JceAidCalib*)st->calib_head;
    while (c) {
        JceAidCalib* n = c->next;
        calib_destroy(c);
        c = n;
    }
    st->calib_head = NULL;
    if (st->calib_dir) { jce_aid_free(st->calib_dir); st->calib_dir = NULL; }
}

static JceAidCalib* calib_ensure(JceAidSchemaId id, const JceAidSchema* s)
{
    JceAidState* st = jce_aid_state();
    JceAidCalib* c  = jce_aid_calib_get(id);
    if (c) return c;
    c = (JceAidCalib*)jce_aid_malloc(sizeof(JceAidCalib));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->schema_id    = id;
    c->payload_size = jce_aid_schema_payload_size(s);
    c->field_count  = s->field_count;
    jce_rng_seed(&c->rng, 0xCA11Bull, id); /* reservoir replacement stream */
    c->next        = (JceAidCalib*)st->calib_head;
    st->calib_head = c;
    return c;
}

/* ---- public: reservoir --------------------------------------------------- */

JceAidResult JCE_CALL jce_aid_calib_note_accepted(const JceAidRecord* rec)
{
    JceAidState*        st = jce_aid_state();
    const JceAidSchema* s;
    JceAidCalib*        c;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!rec) return JCE_AID_ERR_INVALID_ARG;
    if (rec->tier != 1) return JCE_AID_ERR_INVALID_ARG; /* T1 only (spec F.6) */
    s = jce_aid_schema_get(rec->schema_id);
    if (!s) return JCE_AID_ERR_NOT_FOUND;
    if (rec->payload_len != jce_aid_schema_payload_size(s))
        return JCE_AID_ERR_BAD_FORMAT;

    c = calib_ensure(rec->schema_id, s);
    if (!c) return JCE_AID_ERR_LIMIT;
    if (!c->reservoir) {
        c->reservoir = (uint8_t*)jce_aid_malloc(
            (size_t)AID_CALIB_CAP * c->payload_size);
        if (!c->reservoir) return JCE_AID_ERR_LIMIT;
        c->count = 0;
        c->seen  = 0;
    }

    if (c->count < AID_CALIB_CAP) {
        memcpy(c->reservoir + (size_t)c->count * c->payload_size,
               rec->payload, c->payload_size);
        c->count++;
    } else {
        /* classic reservoir replacement */
        uint64_t j = jce_rng_u64(&c->rng) % (c->seen + 1u);
        if (j < AID_CALIB_CAP)
            memcpy(c->reservoir + (size_t)j * c->payload_size,
                   rec->payload, c->payload_size);
    }
    c->seen++;
    return JCE_AID_OK;
}

/* ---- fit ----------------------------------------------------------------- */

static uint32_t field_offset(const JceAidSchema* s, uint16_t field)
{
    uint32_t off = 0;
    uint16_t i;
    for (i = 0; i < field; ++i)
        off += jce_aid_field_wire_size(s->fields[i].type);
    return off;
}

JceAidResult JCE_CALL jce_aid_calib_fit(JceAidSchemaId id)
{
    JceAidState*        st = jce_aid_state();
    const JceAidSchema* s;
    JceAidCalib*        c;
    uint16_t            f;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    s = jce_aid_schema_get(id);
    if (!s) return JCE_AID_ERR_NOT_FOUND;
    c = jce_aid_calib_get(id);
    if (!c || !c->reservoir || c->count == 0) return JCE_AID_ERR_NOT_FOUND;

    calib_free_fields(c);
    c->fields = (CalibField*)jce_aid_malloc(sizeof(CalibField) * s->field_count);
    if (!c->fields) return JCE_AID_ERR_LIMIT;
    memset(c->fields, 0, sizeof(CalibField) * s->field_count);

    for (f = 0; f < s->field_count; ++f) {
        const JceAidField* fld = &s->fields[f];
        CalibField*        cf  = &c->fields[f];
        const uint32_t     off = field_offset(s, f);
        uint16_t           r;
        cf->type = fld->type;

        switch (fld->type) {
        case JCE_AID_F_I32:
        case JCE_AID_F_F32Q: {
            int64_t sum = 0;
            int32_t lo = 0, hi = 0;
            for (r = 0; r < c->count; ++r) {
                const int32_t v = (int32_t)rd_u32(
                    c->reservoir + (size_t)r * c->payload_size + off);
                if (r == 0) { lo = hi = v; }
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                sum += v;
            }
            cf->num.mean = (int32_t)(sum / c->count);
            {
                uint64_t acc = 0;
                for (r = 0; r < c->count; ++r) {
                    const int32_t v = (int32_t)rd_u32(
                        c->reservoir + (size_t)r * c->payload_size + off);
                    const int64_t d = (int64_t)v - cf->num.mean;
                    acc += (uint64_t)(d * d);
                }
                cf->num.stddev = (int32_t)isqrt64(acc / c->count);
            }
            cf->num.lo = lo; /* seen-range truncation (quantile stand-in) */
            cf->num.hi = hi;
            break;
        }
        case JCE_AID_F_ENUM: {
            cf->freq = (uint32_t*)jce_aid_malloc(sizeof(uint32_t) *
                                                 fld->enum_count);
            if (!cf->freq) return JCE_AID_ERR_LIMIT;
            memset(cf->freq, 0, sizeof(uint32_t) * fld->enum_count);
            for (r = 0; r < c->count; ++r) {
                uint16_t idx = rd_u16(
                    c->reservoir + (size_t)r * c->payload_size + off);
                if (idx < fld->enum_count) cf->freq[idx]++;
            }
            {
                uint16_t e;
                cf->freq_total = 0;
                for (e = 0; e < fld->enum_count; ++e)
                    cf->freq_total += cf->freq[e];
            }
            break;
        }
        case JCE_AID_F_TAGSET: {
            cf->freq = (uint32_t*)jce_aid_malloc(sizeof(uint32_t) *
                                                 fld->enum_count);
            if (!cf->freq) return JCE_AID_ERR_LIMIT;
            memset(cf->freq, 0, sizeof(uint32_t) * fld->enum_count);
            for (r = 0; r < c->count; ++r) {
                uint64_t bits = rd_u64(
                    c->reservoir + (size_t)r * c->payload_size + off);
                uint16_t b;
                for (b = 0; b < fld->enum_count; ++b)
                    if (bits & (1ull << b)) cf->freq[b]++;
            }
            cf->freq_total = c->count; /* per-bit denominator */
            break;
        }
        case JCE_AID_F_COLOR8:
        default:
            /* empirical row pick at sampling time; nothing to fit */
            break;
        }
    }

    c->calib_ver++;
    if (c->calib_ver == 0) c->calib_ver = 1; /* wrap guard */

    /* ---- persist (JCAL v1) ---- */
    {
        char path[512];
        FILE* fp;
        snprintf(path, sizeof(path), "%s/aid_calib_%016llx.bin",
                 st->calib_dir ? st->calib_dir : ".",
                 (unsigned long long)id);
        fp = fopen(path, "wb");
        if (!fp) return JCE_AID_ERR_INVALID_ARG;
        {
            /* serialise into a memory blob first so the checksum covers
             * everything before the trailer */
            size_t   cap = 64 + (size_t)s->field_count * 32 +
                           (size_t)c->count * c->payload_size +
                           (size_t)s->field_count * 4 * 64;
            uint8_t* blob = (uint8_t*)jce_aid_malloc(cap + 8);
            size_t   n = 0;
            uint16_t e;
            if (!blob) { fclose(fp); return JCE_AID_ERR_LIMIT; }

            blob[n++] = 'J'; blob[n++] = 'C'; blob[n++] = 'A'; blob[n++] = 'L';
            wr_u32(blob + n, AID_CALIB_FILE_V);          n += 4;
            wr_u64(blob + n, id);                        n += 8;
            wr_u16(blob + n, c->calib_ver);              n += 2;
            wr_u16(blob + n, s->field_count);            n += 2;
            wr_u32(blob + n, c->payload_size);           n += 4;
            wr_u16(blob + n, c->count);                  n += 2;

            for (f = 0; f < s->field_count; ++f) {
                const CalibField* cf = &c->fields[f];
                blob[n++] = (uint8_t)cf->type;
                switch (cf->type) {
                case JCE_AID_F_I32:
                case JCE_AID_F_F32Q:
                    wr_u32(blob + n, (uint32_t)cf->num.mean);   n += 4;
                    wr_u32(blob + n, (uint32_t)cf->num.stddev); n += 4;
                    wr_u32(blob + n, (uint32_t)cf->num.lo);     n += 4;
                    wr_u32(blob + n, (uint32_t)cf->num.hi);     n += 4;
                    break;
                case JCE_AID_F_ENUM:
                case JCE_AID_F_TAGSET:
                    wr_u16(blob + n, s->fields[f].enum_count);  n += 2;
                    for (e = 0; e < s->fields[f].enum_count; ++e) {
                        wr_u32(blob + n, cf->freq[e]);          n += 4;
                    }
                    wr_u64(blob + n, cf->freq_total);           n += 8;
                    break;
                default:
                    break;
                }
            }
            memcpy(blob + n, c->reservoir, (size_t)c->count * c->payload_size);
            n += (size_t)c->count * c->payload_size;
            wr_u64(blob + n, jce_aid_hash64(blob, n));
            n += 8;

            if (fwrite(blob, 1, n, fp) != n) {
                jce_aid_free(blob);
                fclose(fp);
                return JCE_AID_ERR_INVALID_ARG;
            }
            jce_aid_free(blob);
        }
        fclose(fp);
    }
    return JCE_AID_OK;
}

/* ---- load ----------------------------------------------------------------- */

JceAidResult JCE_CALL jce_aid_calib_load(JceAidSchemaId id)
{
    JceAidState*        st = jce_aid_state();
    const JceAidSchema* s;
    JceAidCalib*        c = NULL;
    char                path[512];
    FILE*               fp;
    uint8_t*            blob = NULL;
    size_t              n = 0;
    JceAidResult        result = JCE_AID_ERR_BAD_FORMAT;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    s = jce_aid_schema_get(id);
    if (!s) return JCE_AID_ERR_NOT_FOUND;

    snprintf(path, sizeof(path), "%s/aid_calib_%016llx.bin",
             st->calib_dir ? st->calib_dir : ".", (unsigned long long)id);
    fp = fopen(path, "rb");
    if (!fp) return JCE_AID_ERR_NOT_FOUND;
    fseek(fp, 0, SEEK_END);
    {
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz < 32 || sz > (1 << 22)) { fclose(fp); return JCE_AID_ERR_BAD_FORMAT; }
        blob = (uint8_t*)jce_aid_malloc((size_t)sz);
        if (!blob) { fclose(fp); return JCE_AID_ERR_LIMIT; }
        if (fread(blob, 1, (size_t)sz, fp) != (size_t)sz) {
            jce_aid_free(blob);
            fclose(fp);
            return JCE_AID_ERR_BAD_FORMAT;
        }
        n = (size_t)sz;
    }
    fclose(fp);

    /* checksum + header validation; any failure = safe fallback */
    do {
        size_t   p = 0;
        uint16_t calib_ver, field_count, count;
        uint32_t payload_size;
        uint16_t f;

        if (rd_u64(blob + n - 8) != jce_aid_hash64(blob, n - 8)) break;
        if (blob[0] != 'J' || blob[1] != 'C' || blob[2] != 'A' || blob[3] != 'L')
            break;
        p = 4;
        if (rd_u32(blob + p) != AID_CALIB_FILE_V) break;
        p += 4;
        if (rd_u64(blob + p) != id) break;
        p += 8;
        calib_ver = rd_u16(blob + p); p += 2;
        field_count = rd_u16(blob + p); p += 2;
        payload_size = rd_u32(blob + p); p += 4;
        count = rd_u16(blob + p); p += 2;
        if (field_count != s->field_count) break;
        if (payload_size != jce_aid_schema_payload_size(s)) break;
        if (count > AID_CALIB_CAP) break;

        c = calib_ensure(id, s);
        if (!c) { result = JCE_AID_ERR_LIMIT; break; }
        calib_free_fields(c);
        c->fields = (CalibField*)jce_aid_malloc(sizeof(CalibField) *
                                                field_count);
        if (!c->fields) { result = JCE_AID_ERR_LIMIT; break; }
        memset(c->fields, 0, sizeof(CalibField) * field_count);

        for (f = 0; f < field_count; ++f) {
            CalibField* cf = &c->fields[f];
            if (p >= n - 8) goto malformed;
            cf->type = (JceAidFieldType)blob[p++];
            if (cf->type != s->fields[f].type) goto malformed;
            switch (cf->type) {
            case JCE_AID_F_I32:
            case JCE_AID_F_F32Q:
                if (p + 16 > n - 8) goto malformed;
                cf->num.mean   = (int32_t)rd_u32(blob + p); p += 4;
                cf->num.stddev = (int32_t)rd_u32(blob + p); p += 4;
                cf->num.lo     = (int32_t)rd_u32(blob + p); p += 4;
                cf->num.hi     = (int32_t)rd_u32(blob + p); p += 4;
                break;
            case JCE_AID_F_ENUM:
            case JCE_AID_F_TAGSET: {
                uint16_t cnt, e;
                if (p + 2 > n - 8) goto malformed;
                cnt = rd_u16(blob + p); p += 2;
                if (cnt != s->fields[f].enum_count) goto malformed;
                if (p + (size_t)cnt * 4 + 8 > n - 8) goto malformed;
                cf->freq = (uint32_t*)jce_aid_malloc(sizeof(uint32_t) * cnt);
                if (!cf->freq) goto malformed;
                for (e = 0; e < cnt; ++e) { cf->freq[e] = rd_u32(blob + p); p += 4; }
                cf->freq_total = rd_u64(blob + p); p += 8;
                break;
            }
            default:
                break;
            }
        }
        /* reservoir */
        if (p + (size_t)count * payload_size != n - 8) goto malformed;
        if (!c->reservoir) {
            c->reservoir = (uint8_t*)jce_aid_malloc(
                (size_t)AID_CALIB_CAP * payload_size);
            if (!c->reservoir) { result = JCE_AID_ERR_LIMIT; break; }
        }
        memcpy(c->reservoir, blob + p, (size_t)count * payload_size);
        c->count     = count;
        c->seen      = count;
        c->calib_ver = calib_ver;
        result = JCE_AID_OK;
        break;

    malformed:
        if (c) calib_free_fields(c); /* table rejected: uniform fallback */
        result = JCE_AID_ERR_BAD_FORMAT;
        break;
    } while (0);

    jce_aid_free(blob);
    return result;
}

uint16_t JCE_CALL jce_aid_calib_version(JceAidSchemaId id)
{
    JceAidCalib* c = jce_aid_calib_get(id);
    return (c && c->fields) ? c->calib_ver : 0;
}

void JCE_CALL jce_aid_calib_set_dir(const char* dir)
{
    JceAidState* st = jce_aid_state();
    if (!st) return;
    if (st->calib_dir) { jce_aid_free(st->calib_dir); st->calib_dir = NULL; }
    if (dir) st->calib_dir = jce_aid_strdup(dir);
}

/* ---- table sampling (called from tier3) ---------------------------------- */

int jce_aid_calib_sample(const JceAidSchema* s, const JceAidCalib* c,
                         JceRng* rng, uint8_t* payload, uint16_t* out_len)
{
    uint32_t off = 0;
    uint16_t f;

    if (!c || !c->fields || c->count == 0) return 0;

    for (f = 0; f < s->field_count; ++f) {
        const JceAidField* fld = &s->fields[f];
        const CalibField*  cf  = &c->fields[f];
        switch (fld->type) {
        case JCE_AID_F_I32:
        case JCE_AID_F_F32Q: {
            /* 12-draw Irwin-Hall approximate normal: z has stddev 1.0 in
             * Q16.16 units; v = mean + (z * stddev >> 16), truncated to
             * the seen range then the schema bounds.  Integer only. */
            int64_t z = 0;
            int     k;
            int32_t v;
            for (k = 0; k < 12; ++k)
                z += (int64_t)(jce_rng_u32(rng) & 0xFFFFu);
            z -= 6ll * 65535ll;
            v = (int32_t)((int64_t)cf->num.mean +
                          ((z * (int64_t)cf->num.stddev) >> 16));
            v = jce_aid_clamp_i32(v, cf->num.lo, cf->num.hi);
            v = jce_aid_clamp_i32(v, fld->min_q, fld->max_q);
            wr_u32(payload + off, (uint32_t)v);
            off += 4;
            break;
        }
        case JCE_AID_F_ENUM: {
            uint16_t pick = 0;
            if (cf->freq_total) {
                uint64_t d = jce_rng_u64(rng) % cf->freq_total;
                uint16_t e;
                uint64_t acc = 0;
                for (e = 0; e < fld->enum_count; ++e) {
                    acc += cf->freq[e];
                    if (d < acc) { pick = e; break; }
                }
            } else {
                pick = (uint16_t)(jce_rng_u64(rng) % fld->enum_count);
            }
            wr_u16(payload + off, pick);
            off += 2;
            break;
        }
        case JCE_AID_F_COLOR8: {
            /* empirical row pick */
            const uint32_t row = (uint32_t)(jce_rng_u64(rng) % c->count);
            const uint8_t* src =
                c->reservoir + (size_t)row * c->payload_size + off;
            payload[off + 0] = src[0]; payload[off + 1] = src[1];
            payload[off + 2] = src[2]; payload[off + 3] = src[3];
            off += 4;
            break;
        }
        case JCE_AID_F_TAGSET: {
            uint64_t bits = 0;
            uint16_t b;
            for (b = 0; b < fld->enum_count; ++b) {
                /* set bit with probability freq[b]/freq_total */
                if (cf->freq_total &&
                    (jce_rng_u64(rng) % cf->freq_total) < cf->freq[b])
                    bits |= 1ull << b;
            }
            wr_u64(payload + off, bits);
            off += 8;
            break;
        }
        default:
            return 0;
        }
    }
    *out_len = (uint16_t)off;
    return 1;
}
