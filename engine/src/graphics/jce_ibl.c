/*
 * jce_ibl.c  CPU-side Image-Based Lighting generation.
 *
 * Generates:
 * - BRDF integration LUT (importance-sampled GGX, split-sum approximation)
 * - Diffuse irradiance cubemap (cosine-weighted hemisphere sampling)
 * - Specular prefiltered environment cubemap (GGX importance sampling per roughness mip)
 *
 * All computation is CPU-side to avoid compute-shader dependency.
 * The output textures are uploaded to bgfx as RGBA16F cubemaps / 2D textures.
 */

#include <jce/graphics/jce_ibl.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <math.h>
#include <string.h>

#define LOG_TAG "jce_ibl"

/* MSVC C mode cannot `return { ... }` directly; use a helper. */
static bgfx_texture_handle_t ibl_invalid_tex_handle(void)
{
    bgfx_texture_handle_t h = BGFX_INVALID_HANDLE;
    return h;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================== */
/* IBL data structure                                                  */
/* ================================================================== */

struct JceIblData {
    bgfx_texture_handle_t irradiance;   /* cubemap */
    bgfx_texture_handle_t prefilter;    /* cubemap with mips */
    bgfx_texture_handle_t brdf_lut;     /* 2D */
};

/* ================================================================== */
/* Math helpers                                                        */
/* ================================================================== */

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
    float phi = 2.0f * (float)M_PI * xi1;
    float cos_theta = sqrtf((1.0f - xi2) / (1.0f + (a * a - 1.0f) * xi2));
    float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);

    *hx = cosf(phi) * sin_theta;
    *hy = sinf(phi) * sin_theta;
    *hz = cos_theta;
}

/* ================================================================== */
/* BRDF Integration LUT                                                */
/* ================================================================== */

/* Geometry term for IBL (Schlick-GGX with k = roughness^2 / 2). */
static float geometry_schlick_ggx_ibl(float n_dot_v, float roughness)
{
    float k = (roughness * roughness) / 2.0f;
    return n_dot_v / (n_dot_v * (1.0f - k) + k);
}

static float geometry_smith_ibl(float n_dot_v, float n_dot_l, float roughness)
{
    return geometry_schlick_ggx_ibl(n_dot_v, roughness)
         * geometry_schlick_ggx_ibl(n_dot_l, roughness);
}

/* Integrate the BRDF for a given (NdotV, roughness) pair.
 * Returns (scale, bias) for the split-sum approximation. */
static void integrate_brdf(float n_dot_v, float roughness,
                           float *out_scale, float *out_bias)
{
    /* Clamp to avoid degenerate cases. */
    if (n_dot_v < 0.001f) n_dot_v = 0.001f;

    float vx = sqrtf(1.0f - n_dot_v * n_dot_v);
    float vy = 0.0f;
    float vz = n_dot_v;

    float scale = 0.0f;
    float bias  = 0.0f;

    const uint32_t SAMPLE_COUNT = 1024;

    for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
        float xi1, xi2;
        hammersley(i, SAMPLE_COUNT, &xi1, &xi2);

        float hx, hy, hz;
        importance_sample_ggx(xi1, xi2, roughness, &hx, &hy, &hz);

        /* Reflect V around H to get L. */
        float v_dot_h = vx * hx + vy * hy + vz * hz;
        float lx = 2.0f * v_dot_h * hx - vx;
        float ly = 2.0f * v_dot_h * hy - vy;
        float lz = 2.0f * v_dot_h * hz - vz;

        float n_dot_l = lz;  /* N = (0,0,1) in tangent space */
        float n_dot_h = hz;
        float vdh     = v_dot_h;

        if (n_dot_l > 0.0f) {
            float G = geometry_smith_ibl(n_dot_v, n_dot_l, roughness);
            float G_vis = (G * vdh) / (n_dot_h * n_dot_v);
            float Fc = powf(1.0f - vdh, 5.0f);

            scale += (1.0f - Fc) * G_vis;
            bias  += Fc * G_vis;
        }
    }

    *out_scale = scale / (float)SAMPLE_COUNT;
    *out_bias  = bias  / (float)SAMPLE_COUNT;
}

bgfx_texture_handle_t jce_ibl_create_brdf_lut(uint32_t size)
{
    if (size == 0) size = 256;

    uint32_t pixel_count = size * size;
    uint16_t *data = (uint16_t *)JCE_MALLOC(pixel_count * 4 * sizeof(uint16_t));
    if (!data) return ibl_invalid_tex_handle();

    for (uint32_t y = 0; y < size; y++) {
        float roughness = ((float)y + 0.5f) / (float)size;
        for (uint32_t x = 0; x < size; x++) {
            float n_dot_v = ((float)x + 0.5f) / (float)size;
            float s, b;
            integrate_brdf(n_dot_v, roughness, &s, &b);

            uint32_t idx = (y * size + x) * 4;
            data[idx + 0] = f32_to_f16(s);
            data[idx + 1] = f32_to_f16(b);
            data[idx + 2] = f32_to_f16(0.0f);
            data[idx + 3] = f32_to_f16(1.0f);
        }
    }

    const bgfx_memory_t *mem = bgfx_copy(data, pixel_count * 4 * sizeof(uint16_t));
    JCE_FREE(data);

    bgfx_texture_handle_t tex = bgfx_create_texture_2d(
        (uint16_t)size, (uint16_t)size, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem);

    if (BGFX_HANDLE_IS_VALID(tex))
        LOG_INFO(LOG_TAG, "BRDF LUT created: %ux%u", size, size);

    return tex;
}

/* ================================================================== */
/* Cubemap direction helpers                                           */
/* ================================================================== */

/* Compute direction vector from cubemap face + UV coordinates.
 * face: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z */
static void cube_dir(int face, float u, float v,
                     float *dx, float *dy, float *dz)
{
    /* Map [0,1] to [-1,1] */
    float su = u * 2.0f - 1.0f;
    float sv = v * 2.0f - 1.0f;

    switch (face) {
    case 0: *dx =  1.0f; *dy = -sv;   *dz = -su;   break; /* +X */
    case 1: *dx = -1.0f; *dy = -sv;   *dz =  su;   break; /* -X */
    case 2: *dx =  su;   *dy =  1.0f; *dz =  sv;   break; /* +Y */
    case 3: *dx =  su;   *dy = -1.0f; *dz = -sv;   break; /* -Y */
    case 4: *dx =  su;   *dy = -sv;   *dz =  1.0f; break; /* +Z */
    case 5: *dx = -su;   *dy = -sv;   *dz = -1.0f; break; /* -Z */
    default: *dx = 0; *dy = 0; *dz = 1; break;
    }

    float len = sqrtf(*dx * *dx + *dy * *dy + *dz * *dz);
    if (len > 1e-8f) { *dx /= len; *dy /= len; *dz /= len; }
}

/* Sample equirectangular map from a direction vector.
 * src: float RGBA pixel data, w x h resolution. */
static void sample_equirect(const float *src, uint32_t w, uint32_t h,
                            float dx, float dy, float dz,
                            float *r, float *g, float *b)
{
    float theta = atan2f(dz, dx);
    float phi   = asinf(dy);
    float u = theta / (2.0f * (float)M_PI) + 0.5f;
    float v = phi / (float)M_PI + 0.5f;

    /* Bilinear sample. */
    float fx = u * (float)(w - 1);
    float fy = (1.0f - v) * (float)(h - 1);  /* flip V */
    int ix = (int)fx;
    int iy = (int)fy;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix >= (int)w - 1) ix = (int)w - 2;
    if (iy >= (int)h - 1) iy = (int)h - 2;

    float tx = fx - (float)ix;
    float ty = fy - (float)iy;

    const float *p00 = &src[((uint32_t)iy * w + (uint32_t)ix) * 4];
    const float *p10 = &src[((uint32_t)iy * w + (uint32_t)ix + 1) * 4];
    const float *p01 = &src[((uint32_t)(iy + 1) * w + (uint32_t)ix) * 4];
    const float *p11 = &src[((uint32_t)(iy + 1) * w + (uint32_t)ix + 1) * 4];

    for (int c = 0; c < 3; c++) {
        float top    = p00[c] * (1.0f - tx) + p10[c] * tx;
        float bottom = p01[c] * (1.0f - tx) + p11[c] * tx;
        float val    = top * (1.0f - ty) + bottom * ty;
        if (c == 0) *r = val;
        else if (c == 1) *g = val;
        else *b = val;
    }
}

/* ================================================================== */
/* Irradiance cubemap (cosine-weighted hemisphere convolution)          */
/* ================================================================== */

static bgfx_texture_handle_t generate_irradiance(const float *equirect,
                                                  uint32_t ew, uint32_t eh,
                                                  uint32_t face_size)
{
    const uint32_t SAMPLE_DELTA_STEPS = 64;

    uint32_t face_pixels = face_size * face_size;
    uint32_t total_pixels = face_pixels * 6;
    uint16_t *data = (uint16_t *)JCE_MALLOC(total_pixels * 4 * sizeof(uint16_t));
    if (!data) return ibl_invalid_tex_handle();

    for (int face = 0; face < 6; face++) {
        for (uint32_t y = 0; y < face_size; y++) {
            for (uint32_t x = 0; x < face_size; x++) {
                float u = ((float)x + 0.5f) / (float)face_size;
                float v = ((float)y + 0.5f) / (float)face_size;

                float nx, ny, nz;
                cube_dir(face, u, v, &nx, &ny, &nz);

                /* Build tangent frame around N. */
                float upx = 0.0f, upy = 1.0f, upz = 0.0f;
                if (fabsf(ny) > 0.999f) { upx = 1.0f; upy = 0.0f; }

                float rx = upy * nz - upz * ny;
                float ry = upz * nx - upx * nz;
                float rz = upx * ny - upy * nx;
                float rl = sqrtf(rx*rx + ry*ry + rz*rz);
                if (rl > 1e-8f) { rx /= rl; ry /= rl; rz /= rl; }

                float tx = ry * nz - rz * ny;
                float ty = rz * nx - rx * nz;
                float tz = rx * ny - ry * nx;

                /* Cosine-weighted hemisphere sampling. */
                float irr_r = 0.0f, irr_g = 0.0f, irr_b = 0.0f;
                float total_weight = 0.0f;

                for (uint32_t p = 0; p < SAMPLE_DELTA_STEPS; p++) {
                    float phi = 2.0f * (float)M_PI * ((float)p + 0.5f) / (float)SAMPLE_DELTA_STEPS;
                    for (uint32_t t = 0; t < SAMPLE_DELTA_STEPS / 4; t++) {
                        float theta = 0.5f * (float)M_PI * ((float)t + 0.5f) / (float)(SAMPLE_DELTA_STEPS / 4);

                        float sin_t = sinf(theta);
                        float cos_t = cosf(theta);
                        float sin_p = sinf(phi);
                        float cos_p = cosf(phi);

                        /* Tangent-space direction. */
                        float sx = sin_t * cos_p;
                        float sy = sin_t * sin_p;
                        float sz = cos_t;

                        /* To world space. */
                        float wx = sx * rx + sy * tx + sz * nx;
                        float wy = sx * ry + sy * ty + sz * ny;
                        float wz = sx * rz + sy * tz + sz * nz;

                        float sr, sg, sb;
                        sample_equirect(equirect, ew, eh, wx, wy, wz,
                                        &sr, &sg, &sb);

                        float weight = cos_t * sin_t;
                        irr_r += sr * weight;
                        irr_g += sg * weight;
                        irr_b += sb * weight;
                        total_weight += weight;
                    }
                }

                if (total_weight > 0.0f) {
                    irr_r = irr_r * (float)M_PI / total_weight;
                    irr_g = irr_g * (float)M_PI / total_weight;
                    irr_b = irr_b * (float)M_PI / total_weight;
                }

                uint32_t idx = ((uint32_t)face * face_pixels + y * face_size + x) * 4;
                data[idx + 0] = f32_to_f16(irr_r);
                data[idx + 1] = f32_to_f16(irr_g);
                data[idx + 2] = f32_to_f16(irr_b);
                data[idx + 3] = f32_to_f16(1.0f);
            }
        }
    }

    const bgfx_memory_t *mem = bgfx_copy(data, total_pixels * 4 * sizeof(uint16_t));
    JCE_FREE(data);

    bgfx_texture_handle_t tex = bgfx_create_texture_cube(
        (uint16_t)face_size, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        mem);

    if (BGFX_HANDLE_IS_VALID(tex))
        LOG_INFO(LOG_TAG, "irradiance cubemap: %u per face", face_size);

    return tex;
}

/* ================================================================== */
/* Specular prefilter cubemap                                          */
/* ================================================================== */

static bgfx_texture_handle_t generate_prefilter(const float *equirect,
                                                 uint32_t ew, uint32_t eh,
                                                 uint32_t face_size)
{
    /* Number of mip levels. */
    uint32_t max_mip = 1;
    {
        uint32_t s = face_size;
        while (s > 1) { s >>= 1; max_mip++; }
    }
    if (max_mip > 8) max_mip = 8;

    /* Compute total memory needed for all mips of all 6 faces. */
    uint32_t total_half_words = 0;
    for (uint32_t mip = 0; mip < max_mip; mip++) {
        uint32_t ms = face_size >> mip;
        if (ms < 1) ms = 1;
        total_half_words += ms * ms * 6 * 4;
    }

    uint16_t *data = (uint16_t *)JCE_MALLOC(total_half_words * sizeof(uint16_t));
    if (!data) return ibl_invalid_tex_handle();

    uint32_t offset = 0;
    const uint32_t SAMPLE_COUNT = 512;

    for (uint32_t mip = 0; mip < max_mip; mip++) {
        uint32_t ms = face_size >> mip;
        if (ms < 1) ms = 1;
        float roughness = (float)mip / (float)(max_mip - 1);

        for (int face = 0; face < 6; face++) {
            for (uint32_t y = 0; y < ms; y++) {
                for (uint32_t x = 0; x < ms; x++) {
                    float u = ((float)x + 0.5f) / (float)ms;
                    float v = ((float)y + 0.5f) / (float)ms;

                    float nx, ny, nz;
                    cube_dir(face, u, v, &nx, &ny, &nz);

                    /* Use N = V = R for the split-sum approximation. */
                    float vx = nx, vy = ny, vz = nz;

                    /* Build tangent frame. */
                    float upx = 0.0f, upy = 1.0f, upz = 0.0f;
                    if (fabsf(ny) > 0.999f) { upx = 1.0f; upy = 0.0f; }

                    float rx = upy * nz - upz * ny;
                    float ry = upz * nx - upx * nz;
                    float rz = upx * ny - upy * nx;
                    float rl = sqrtf(rx*rx + ry*ry + rz*rz);
                    if (rl > 1e-8f) { rx /= rl; ry /= rl; rz /= rl; }

                    float tx = ry * nz - rz * ny;
                    float ty = rz * nx - rx * nz;
                    float tz = rx * ny - ry * nx;

                    float pf_r = 0.0f, pf_g = 0.0f, pf_b = 0.0f;
                    float total_weight = 0.0f;

                    for (uint32_t s = 0; s < SAMPLE_COUNT; s++) {
                        float xi1, xi2;
                        hammersley(s, SAMPLE_COUNT, &xi1, &xi2);

                        float hx_t, hy_t, hz_t;
                        importance_sample_ggx(xi1, xi2, roughness,
                                             &hx_t, &hy_t, &hz_t);

                        /* Transform H from tangent to world. */
                        float hx = hx_t * rx + hy_t * tx + hz_t * nx;
                        float hy2 = hx_t * ry + hy_t * ty + hz_t * ny;
                        float hz = hx_t * rz + hy_t * tz + hz_t * nz;

                        /* Reflect V around H. */
                        float v_dot_h = vx * hx + vy * hy2 + vz * hz;
                        float lx = 2.0f * v_dot_h * hx - vx;
                        float ly = 2.0f * v_dot_h * hy2 - vy;
                        float lz = 2.0f * v_dot_h * hz - vz;

                        float n_dot_l = nx * lx + ny * ly + nz * lz;
                        if (n_dot_l > 0.0f) {
                            float sr, sg, sb;
                            sample_equirect(equirect, ew, eh, lx, ly, lz,
                                            &sr, &sg, &sb);
                            pf_r += sr * n_dot_l;
                            pf_g += sg * n_dot_l;
                            pf_b += sb * n_dot_l;
                            total_weight += n_dot_l;
                        }
                    }

                    if (total_weight > 0.0f) {
                        pf_r /= total_weight;
                        pf_g /= total_weight;
                        pf_b /= total_weight;
                    }

                    data[offset++] = f32_to_f16(pf_r);
                    data[offset++] = f32_to_f16(pf_g);
                    data[offset++] = f32_to_f16(pf_b);
                    data[offset++] = f32_to_f16(1.0f);
                }
            }
        }
    }

    const bgfx_memory_t *mem = bgfx_copy(data, total_half_words * sizeof(uint16_t));
    JCE_FREE(data);

    bgfx_texture_handle_t tex = bgfx_create_texture_cube(
        (uint16_t)face_size, true, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        mem);

    if (BGFX_HANDLE_IS_VALID(tex))
        LOG_INFO(LOG_TAG, "prefilter cubemap: %u per face, %u mips",
                 face_size, max_mip);

    return tex;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

JceIblData *jce_ibl_generate(bgfx_texture_handle_t equirect_tex,
                              uint32_t irradiance_size,
                              uint32_t prefilter_size,
                              uint32_t brdf_lut_size)
{
    /*
     * NOTE: bgfx textures are GPU-resident; we cannot read them back.
     * The equirect_tex parameter is kept for API consistency, but the
     * actual HDR pixel data must be passed via an alternate path.
     *
     * For the editor integration, we read the HDR file on the CPU side
     * and call jce_ibl_generate_from_pixels() instead.
     * This function is a stub that returns NULL.
     */
    (void)equirect_tex;
    (void)irradiance_size;
    (void)prefilter_size;
    (void)brdf_lut_size;

    LOG_WARN(LOG_TAG, "jce_ibl_generate() requires CPU pixel data; "
             "use editor's IBL pipeline instead");
    return NULL;
}

void jce_ibl_destroy(JceIblData *ibl)
{
    if (!ibl) return;
    if (BGFX_HANDLE_IS_VALID(ibl->irradiance))
        bgfx_destroy_texture(ibl->irradiance);
    if (BGFX_HANDLE_IS_VALID(ibl->prefilter))
        bgfx_destroy_texture(ibl->prefilter);
    if (BGFX_HANDLE_IS_VALID(ibl->brdf_lut))
        bgfx_destroy_texture(ibl->brdf_lut);
    JCE_FREE(ibl);
}

bgfx_texture_handle_t jce_ibl_get_irradiance(const JceIblData *ibl)
{
    if (!ibl) return ibl_invalid_tex_handle();
    return ibl->irradiance;
}

bgfx_texture_handle_t jce_ibl_get_prefilter(const JceIblData *ibl)
{
    if (!ibl) return ibl_invalid_tex_handle();
    return ibl->prefilter;
}

bgfx_texture_handle_t jce_ibl_get_brdf_lut(const JceIblData *ibl)
{
    if (!ibl) return ibl_invalid_tex_handle();
    return ibl->brdf_lut;
}
