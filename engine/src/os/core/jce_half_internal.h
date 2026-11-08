#ifndef JCE_HALF_INTERNAL_H
#define JCE_HALF_INTERNAL_H
#include <stdint.h>
#include <string.h>

/* Shared IEEE binary16 encoder, moved without arithmetic changes from the
 * network quantizer. Round-to-nearest-even, including subnormals/Inf/NaN. */
static inline uint16_t jce_float_to_half(float v)
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

#endif
