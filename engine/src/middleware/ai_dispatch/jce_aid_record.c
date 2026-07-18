/* jce_aid_record.c -- wire codec for constraint records (spec E.2).
 * fmt=1 layout is FROZEN: any behavioural change bumps fmt and keeps
 * the old decode path (spec E.3). */
#include "jce_aid_internal.h"

#define AID_HEADER_SIZE   47u /* bytes before payload */
#define AID_CHECKSUM_SIZE 8u

static void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t* p, uint64_t v)
{
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}
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

JceAidResult JCE_CALL
jce_aid_record_encode(const JceAidRecord* rec, void* buf, size_t* io_len)
{
    uint8_t* p;
    size_t   total;
    uint64_t sum;

    if (!rec || !buf || !io_len) return JCE_AID_ERR_INVALID_ARG;
    if (rec->payload_len > JCE_AID_MAX_PAYLOAD) return JCE_AID_ERR_INVALID_ARG;

    total = (size_t)AID_HEADER_SIZE + rec->payload_len + AID_CHECKSUM_SIZE;
    if (total > JCE_AID_MAX_RECORD_SIZE) return JCE_AID_ERR_INVALID_ARG;
    if (*io_len < total) return JCE_AID_ERR_BUFFER_SMALL;

    p = (uint8_t*)buf;
    p[0] = 'J'; p[1] = 'A'; p[2] = 'I'; p[3] = 'D';
    put_u16(p + 4,  (uint16_t)JCE_AID_FMT_VERSION);
    put_u64(p + 6,  rec->schema_id);
    put_u16(p + 14, rec->schema_ver);
    put_u64(p + 16, rec->record_id);
    put_u64(p + 24, rec->tick);
    p[32] = rec->tier;
    put_u16(p + 33, rec->solver_ver);
    put_u16(p + 35, rec->calib_ver);
    put_u64(p + 37, rec->seed);
    put_u16(p + 45, rec->payload_len);
    memcpy(p + AID_HEADER_SIZE, rec->payload, rec->payload_len);

    sum = jce_aid_hash64(p, AID_HEADER_SIZE + rec->payload_len);
    put_u64(p + AID_HEADER_SIZE + rec->payload_len, sum);

    *io_len = total;
    return JCE_AID_OK;
}

JceAidResult JCE_CALL
jce_aid_record_decode(const void* buf, size_t len, JceAidRecord* out)
{
    const uint8_t* p;
    uint16_t       payload_len;
    uint64_t       want, got;

    if (!buf || !out) return JCE_AID_ERR_INVALID_ARG;
    if (len < AID_HEADER_SIZE + AID_CHECKSUM_SIZE) return JCE_AID_ERR_BAD_FORMAT;

    p = (const uint8_t*)buf;
    if (p[0] != 'J' || p[1] != 'A' || p[2] != 'I' || p[3] != 'D')
        return JCE_AID_ERR_BAD_FORMAT;
    if (get_u16(p + 4) != JCE_AID_FMT_VERSION) return JCE_AID_ERR_BAD_FORMAT;

    payload_len = get_u16(p + 45);
    if (payload_len > JCE_AID_MAX_PAYLOAD) return JCE_AID_ERR_BAD_FORMAT;
    if (len != (size_t)AID_HEADER_SIZE + payload_len + AID_CHECKSUM_SIZE)
        return JCE_AID_ERR_BAD_FORMAT;

    want = get_u64(p + AID_HEADER_SIZE + payload_len);
    got  = jce_aid_hash64(p, AID_HEADER_SIZE + payload_len);
    if (want != got) return JCE_AID_ERR_CHECKSUM; /* tamper/corruption */

    memset(out, 0, sizeof(*out));
    out->schema_id   = get_u64(p + 6);
    out->schema_ver  = get_u16(p + 14);
    out->record_id   = get_u64(p + 16);
    out->tick        = get_u64(p + 24);
    out->tier        = p[32];
    out->solver_ver  = get_u16(p + 33);
    out->calib_ver   = get_u16(p + 35);
    out->seed        = get_u64(p + 37);
    out->payload_len = payload_len;
    memcpy(out->payload, p + AID_HEADER_SIZE, payload_len);
    return JCE_AID_OK;
}
