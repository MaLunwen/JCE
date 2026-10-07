/* Shared pure IBL sampling and half conversion; no renderer/device state. */
#ifndef JCE_IBL_SAMPLING_H
#define JCE_IBL_SAMPLING_H
#include <stdint.h>
#include <math.h>
#include <jce/os/core/jce_math.h>
static uint16_t f32_to_f16(float f)
{
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t b = conv.u;
    uint32_t sign = (b >> 16) & 0x8000;
    int32_t expo  = ((int32_t)((b >> 23) & 0xFF)) - 127 + 15;
    uint32_t mant = (b >> 13) & 0x03FF;
    if (expo <= 0)  return (uint16_t)sign;
    if (expo >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | ((uint32_t)expo << 10) | mant);
}

/* The inverse of the above.  Denormals decode as zero, which is what the
 * encoder produces for them anyway (expo <= 0 returns the sign bit alone), so
 * the pair round-trips every value the encoder can emit. */
static float f16_to_f32(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t expo = (uint32_t)(h >> 10) & 0x1Fu;
    const uint32_t mant = (uint32_t)(h & 0x03FFu);
    union { float f; uint32_t u; } conv;
    if (expo == 0u)  { conv.u = sign; return conv.f; }          /* +-0/denorm */
    if (expo == 31u) { conv.u = sign | 0x7F800000u | (mant << 13); return conv.f; }
    conv.u = sign | ((expo - 15u + 127u) << 23) | (mant << 13);
    return conv.f;
}

/* Radical inverse using Van der Corput sequence (base 2). */
static float radical_inverse_vdc(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

/* Low-discrepancy 2D sample via Hammersley sequence. */
static void hammersley(uint32_t i, uint32_t n, float *xi1, float *xi2)
{
    *xi1 = (float)i / (float)n;
    *xi2 = radical_inverse_vdc(i);
}

/* GGX importance sampling: returns a half-vector in tangent space. */
static void importance_sample_ggx(float xi1, float xi2, float roughness,
                                  float *hx, float *hy, float *hz)
{
    float a = roughness * roughness;
    float phi = 2.0f * JCE_PI * xi1;
    float cos_theta = sqrtf((1.0f - xi2) / (1.0f + (a * a - 1.0f) * xi2));
    float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);

    *hx = cosf(phi) * sin_theta;
    *hy = sinf(phi) * sin_theta;
    *hz = cos_theta;
}


#endif
