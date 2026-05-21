/*
 * jce_net_quant.c — P4-D.3 quantization primitives.
 *
 * Pure-C bit math; no allocations, no platform deps.  See header for
 * the wire-format contract.
 */

#include <jce/middleware/net/jce_net_quant.h>

#include <math.h>
#include <string.h>

/* ================================================================== */
/* fp16 (IEEE-754 half) — round-to-nearest-even, with Inf / NaN /     */
/* denormal handling.  Branchy but deterministic and well-tested.     */
/* ================================================================== */

uint16_t jce_quant_f32_to_f16(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);

    uint32_t sign = (u >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;

    if (exp >= 31) {
        /* Inf or NaN. */
        if (((u >> 23) & 0xFFu) == 0xFFu && mant != 0u) {
            return (uint16_t)(sign | 0x7E00u);    /* qNaN */
        }
        return (uint16_t)(sign | 0x7C00u);        /* +/- Inf */
    }
    if (exp <= 0) {
        if (exp < -10) {
            /* Underflow to signed zero. */
            return (uint16_t)sign;
        }
        /* Subnormal: shift mantissa with implicit leading 1. */
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half  = mant >> shift;
        /* Round-to-nearest-even. */
        uint32_t rem   = mant & ((1u << shift) - 1u);
        uint32_t mid   = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (half & 1u))) half += 1u;
        return (uint16_t)(sign | half);
    }

    /* Normal: round mantissa to 10 bits. */
    uint32_t half_mant = mant >> 13;
    uint32_t rem       = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half_mant & 1u))) {
        half_mant += 1u;
        if (half_mant == 0x400u) {
            half_mant = 0u;
            exp      += 1;
            if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
        }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | half_mant);
}

float jce_quant_f16_to_f32(uint16_t v)
{
    uint32_t sign = ((uint32_t)v & 0x8000u) << 16;
    uint32_t exp  = ((uint32_t)v >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)v & 0x3FFu;
    uint32_t out;

    if (exp == 0u) {
        if (mant == 0u) {
            out = sign;                         /* +/- 0 */
        } else {
            /* Subnormal: normalise. */
            int32_t e = -1;
            do { mant <<= 1; e += 1; } while ((mant & 0x400u) == 0u);
            mant &= 0x3FFu;
            out = sign | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        out = sign | 0x7F800000u | (mant << 13); /* Inf / NaN */
    } else {
        out = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &out, 4);
    return f;
}

/* ================================================================== */
/* vec3 in fp16                                                        */
/* ================================================================== */

void jce_quant_pack_vec3_f16(jce_vec3 v, uint16_t out[3])
{
    out[0] = jce_quant_f32_to_f16(v.x);
    out[1] = jce_quant_f32_to_f16(v.y);
    out[2] = jce_quant_f32_to_f16(v.z);
}

jce_vec3 jce_quant_unpack_vec3_f16(const uint16_t in[3])
{
    jce_vec3 v;
    v.x = jce_quant_f16_to_f32(in[0]);
    v.y = jce_quant_f16_to_f32(in[1]);
    v.z = jce_quant_f16_to_f32(in[2]);
    return v;
}

/* ================================================================== */
/* Smallest-three quaternion                                           */
/*                                                                     */
/*   For a unit quaternion q, the component with the largest |value|  */
/*   is dropped on the wire (it can be reconstructed from the other   */
/*   three).  Range of any non-largest component is [-1/sqrt(2),      */
/*   +1/sqrt(2)] so we map to 10-bit signed integers with full        */
/*   dynamic range.                                                    */
/*                                                                     */
/*   Bit layout (LSB on right):                                        */
/*     [31:30] largest-index (0=x, 1=y, 2=z, 3=w)                     */
/*     [29:20] comp_a as signed 10-bit (lowest non-largest index)     */
/*     [19:10] comp_b                                                  */
/*     [9:0]   comp_c                                                  */
/* ================================================================== */

#define ST3_RANGE   0.70710678118654752440f      /* 1 / sqrt(2)         */
#define ST3_SCALE   511.0f                       /* (2^10 / 2) - 1      */

static uint32_t st3_encode_comp(float v)
{
    /* Clamp into representable range then scale. */
    if (v >  ST3_RANGE) v =  ST3_RANGE;
    if (v < -ST3_RANGE) v = -ST3_RANGE;
    float scaled = v * (ST3_SCALE / ST3_RANGE);
    int32_t i = (int32_t)floorf(scaled + 0.5f);
    if (i >  511) i =  511;
    if (i < -512) i = -512;
    return (uint32_t)(i & 0x3FFu);               /* two's-complement 10 */
}

static float st3_decode_comp(uint32_t bits)
{
    bits &= 0x3FFu;
    /* Sign-extend from 10 bits. */
    int32_t s = (bits & 0x200u) ? (int32_t)(bits | 0xFFFFFC00u)
                                : (int32_t)bits;
    return (float)s * (ST3_RANGE / ST3_SCALE);
}

uint32_t jce_quant_pack_quat_st3(jce_quat q)
{
    float c[4] = { q.x, q.y, q.z, q.w };

    /* Find largest |component|. */
    uint32_t largest = 0u;
    float    abs_max = fabsf(c[0]);
    for (uint32_t i = 1u; i < 4u; ++i) {
        float a = fabsf(c[i]);
        if (a > abs_max) { abs_max = a; largest = i; }
    }

    /* Flip sign so the dropped component is non-negative; this is
     * lossless because q and -q represent the same rotation. */
    if (c[largest] < 0.0f) {
        c[0] = -c[0]; c[1] = -c[1]; c[2] = -c[2]; c[3] = -c[3];
    }

    /* Pack the three non-largest components in ascending index order. */
    uint32_t out  = (largest & 0x3u) << 30;
    uint32_t slot = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (i == largest) continue;
        uint32_t bits = st3_encode_comp(c[i]);
        out |= bits << (20u - slot * 10u);
        slot += 1u;
    }
    return out;
}

jce_quat jce_quant_unpack_quat_st3(uint32_t packed)
{
    uint32_t largest = (packed >> 30) & 0x3u;
    float    comp_a  = st3_decode_comp((packed >> 20) & 0x3FFu);
    float    comp_b  = st3_decode_comp((packed >> 10) & 0x3FFu);
    float    comp_c  = st3_decode_comp((packed >>  0) & 0x3FFu);

    float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t slot_vals[3] = { 0u, 0u, 0u };
    (void)slot_vals;

    float vals[3] = { comp_a, comp_b, comp_c };
    uint32_t slot = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (i == largest) continue;
        c[i] = vals[slot++];
    }

    float sum_sq = c[0]*c[0] + c[1]*c[1] + c[2]*c[2] + c[3]*c[3];
    float rem    = 1.0f - sum_sq;
    if (rem < 0.0f) rem = 0.0f;
    c[largest] = sqrtf(rem);

    jce_quat q;
    q.x = c[0]; q.y = c[1]; q.z = c[2]; q.w = c[3];
    return q;
}
