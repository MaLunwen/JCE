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

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_ibl.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL_iostream.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_ibl"

/* MSVC C mode cannot `return { ... }` directly; use a helper. */
static bgfx_texture_handle_t ibl_invalid_tex_handle(void)
{
    bgfx_texture_handle_t h = BGFX_INVALID_HANDLE;
    return h;
}

/* ================================================================== */
/* IBL data structure                                                  */
/* ================================================================== */

struct JceIblData {
    bgfx_texture_handle_t irradiance;   /* cubemap */
    bgfx_texture_handle_t prefilter;    /* cubemap with mips */
    bgfx_texture_handle_t brdf_lut;     /* 2D (unused: renderer shares one LUT) */
    uint32_t              prefilter_mips; /* actual prefilter mip count */
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
    float phi = 2.0f * JCE_PI * xi1;
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

/* BRDF LUT parallel-for job descriptor (file-scope so the worker and
   the dispatch site share an exact type). */
typedef struct {
    uint32_t  size;
    uint16_t *out;
} JceIblBrdfJob;

static void jce_ibl_brdf_worker(uint32_t begin, uint32_t end, void *arg)
{
    JceIblBrdfJob *j = (JceIblBrdfJob *)arg;
    for (uint32_t y = begin; y < end; y++) {
        float roughness = ((float)y + 0.5f) / (float)j->size;
        for (uint32_t x = 0; x < j->size; x++) {
            float n_dot_v = ((float)x + 0.5f) / (float)j->size;
            float s, b;
            integrate_brdf(n_dot_v, roughness, &s, &b);

            uint32_t idx = (y * j->size + x) * 4;
            j->out[idx + 0] = f32_to_f16(s);
            j->out[idx + 1] = f32_to_f16(b);
            j->out[idx + 2] = f32_to_f16(0.0f);
            j->out[idx + 3] = f32_to_f16(1.0f);
        }
    }
}

JceTexture jce_ibl_create_brdf_lut(uint32_t size)
{
    if (size == 0) size = 256;

    uint32_t pixel_count = size * size;
    size_t   bytes       = (size_t)pixel_count * 4 * sizeof(uint16_t);
    uint16_t *data = (uint16_t *)JCE_MALLOC(bytes);
    if (!data) return JCE_TEXTURE_INVALID;

    /* ── Try disk cache ──────────────────────────────────────────────
       The BRDF LUT is a deterministic function of pixel coordinates
       (importance-sampled split-sum GGX), so we can cache the raw
       RGBA16F bytes on disk and reload instantly on subsequent runs.
       Raw host IO on purpose: this is our own regenerable artefact, not an
       asset, so it must NOT go through the VFS-aware reader — an ISOLATED
       active-VFS would null the read while the write below still lands on
       the host, silently disabling the cache for good. */
    bool from_cache = false;
    char cache_path[256];
    snprintf(cache_path, sizeof(cache_path),
             ".jce/cache/brdf_lut_%u.f16", size);
    {
        SDL_IOStream *io = SDL_IOFromFile(cache_path, "rb");
        if (io) {
            Sint64 fsz = SDL_GetIOSize(io);
            if (fsz == (Sint64)bytes &&
                SDL_ReadIO(io, data, bytes) == bytes) {
                from_cache = true;
            }
            SDL_CloseIO(io);
        }
    }

    if (!from_cache) {
        /* Pure fork/join CPU work belongs on the shared frame pool. Fixed row
         * chunks keep writes disjoint; a missing pool executes serially. */
        JceIblBrdfJob job = { size, data };
        uint32_t chunk = size >= 32u ? 16u : 1u;
        LOG_INFO(LOG_TAG, "BRDF LUT: parallel CPU integration");
        jce_thread_pool_parallel_for_named(
            jce_thread_pool_shared(), "ibl.brdf-lut", size, chunk,
            jce_ibl_brdf_worker, &job);
        LOG_INFO(LOG_TAG, "BRDF LUT: computation done");

        /* ── Write cache for next run (best-effort) ─────────────────── */
        jce_fs_host_create_directory(".jce");
        jce_fs_host_create_directory(".jce/cache");
        SDL_IOStream *io = SDL_IOFromFile(cache_path, "wb");
        if (io) {
            SDL_WriteIO(io, data, bytes);
            SDL_CloseIO(io);
        }
    }

    LOG_INFO(LOG_TAG, "BRDF LUT: uploading to GPU");
    const bgfx_memory_t *mem = bgfx_copy(data, (uint32_t)bytes);
    JCE_FREE(data);

    bgfx_texture_handle_t tex = bgfx_create_texture_2d(
        (uint16_t)size, (uint16_t)size, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem, 0);

    LOG_INFO(LOG_TAG, "BRDF LUT: texture idx=%u valid=%d",
             (unsigned)tex.idx, BGFX_HANDLE_IS_VALID(tex) ? 1 : 0);

    return (JceTexture){ tex.idx };
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
    float u = theta / (2.0f * JCE_PI) + 0.5f;
    float v = phi / JCE_PI + 0.5f;

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

/* Computes the irradiance cubemap face data (RGBA16F, 6 faces, no mips) on the
 * CPU and returns it; the caller uploads to bgfx and/or caches it to disk.
 * Returns NULL on OOM. Buffer length = face_size*face_size*6*4 halfwords. */
static uint16_t *ibl_compute_irradiance(const float *equirect,
                                        uint32_t ew, uint32_t eh,
                                        uint32_t face_size)
{
    const uint32_t SAMPLE_DELTA_STEPS = 64;

    uint32_t face_pixels = face_size * face_size;
    uint32_t total_pixels = face_pixels * 6;
    uint16_t *data = (uint16_t *)JCE_MALLOC(total_pixels * 4 * sizeof(uint16_t));
    if (!data) return NULL;

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
                    float phi = 2.0f * JCE_PI * ((float)p + 0.5f) / (float)SAMPLE_DELTA_STEPS;
                    for (uint32_t t = 0; t < SAMPLE_DELTA_STEPS / 4; t++) {
                        float theta = 0.5f * JCE_PI * ((float)t + 0.5f) / (float)(SAMPLE_DELTA_STEPS / 4);

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
                    irr_r = irr_r * JCE_PI / total_weight;
                    irr_g = irr_g * JCE_PI / total_weight;
                    irr_b = irr_b * JCE_PI / total_weight;
                }

                uint32_t idx = ((uint32_t)face * face_pixels + y * face_size + x) * 4;
                data[idx + 0] = f32_to_f16(irr_r);
                data[idx + 1] = f32_to_f16(irr_g);
                data[idx + 2] = f32_to_f16(irr_b);
                data[idx + 3] = f32_to_f16(1.0f);
            }
        }
    }

    /* Return the CPU buffer; the caller uploads (and may cache) it. */
    return data;
}

/* ================================================================== */
/* Specular prefilter cubemap                                          */
/* ================================================================== */

/* Computes the specular prefilter cubemap face data (RGBA16F, 6 faces, all
 * mips packed sequentially) on the CPU and returns it; caller uploads/caches.
 * Sets *out_mips and *out_words (total halfword count). Returns NULL on OOM. */
static uint16_t *ibl_compute_prefilter(const float *equirect,
                                       uint32_t ew, uint32_t eh,
                                       uint32_t face_size,
                                       uint32_t *out_mips,
                                       uint32_t *out_words)
{
    /* Number of mip levels. */
    uint32_t max_mip = 1;
    {
        uint32_t s = face_size;
        while (s > 1) { s >>= 1; max_mip++; }
    }
    if (max_mip > 8) max_mip = 8;
    if (out_mips) *out_mips = max_mip;

    /* Compute total memory needed for all mips of all 6 faces. */
    uint32_t total_half_words = 0;
    for (uint32_t mip = 0; mip < max_mip; mip++) {
        uint32_t ms = face_size >> mip;
        if (ms < 1) ms = 1;
        total_half_words += ms * ms * 6 * 4;
    }

    uint16_t *data = (uint16_t *)JCE_MALLOC(total_half_words * sizeof(uint16_t));
    if (!data) return NULL;

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

    if (out_words) *out_words = total_half_words;
    return data;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

JceIblData *jce_ibl_generate(JceTexture equirect_tex,
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
             "use jce_ibl_generate_from_pixels() instead");
    return NULL;
}

/* ================================================================== */
/* Disk cache (precompute once, reload thereafter)                      */
/* ================================================================== */
/* Generating the irradiance + specular-prefilter cubemaps is a CPU
 * convolution that costs several seconds (the prefilter dominates). It is a
 * pure function of the source HDR pixels + the requested sizes, so — like a
 * professional engine baking its sky/reflection IBL — we cache the baked
 * cubemap face data on disk keyed by a content hash and reload it instantly on
 * subsequent loads (of this scene or any scene using the same HDR).
 * As with the BRDF LUT cache above, load/store deliberately use raw host IO:
 * a regenerable cache must not be intercepted by an active VFS, and the
 * header + two payload blocks are read at offsets into caller-owned buffers,
 * which a whole-file reader cannot express without an extra full copy. */
#define JCE_IBL_CACHE_MAGIC   0x434C4249u  /* 'IBLC' */
#define JCE_IBL_CACHE_VERSION 1u

/* FNV-1a-style 64-bit hash over the source pixel words + bake parameters. */
static uint64_t ibl_content_hash(const float *px, uint32_t n_floats,
                                 uint32_t irr, uint32_t pf)
{
    uint64_t h = 1469598103934665603ULL;
    const uint32_t *w = (const uint32_t *)px;
    for (uint32_t i = 0; i < n_floats; i++)
        h = (h ^ (uint64_t)w[i]) * 1099511628211ULL;
    h = (h ^ (uint64_t)irr) * 1099511628211ULL;
    h = (h ^ (uint64_t)pf)  * 1099511628211ULL;
    h = (h ^ (uint64_t)JCE_IBL_CACHE_VERSION) * 1099511628211ULL;
    return h;
}

/* On a cache hit, allocates + fills *irr / *pf (caller frees) and returns true. */
static bool ibl_cache_load(uint64_t key,
                           uint16_t **irr, uint32_t *irr_face, uint32_t *irr_words,
                           uint16_t **pf,  uint32_t *pf_face,  uint32_t *pf_mips,
                           uint32_t *pf_words)
{
    char path[256];
    snprintf(path, sizeof(path), ".jce/cache/ibl_%016llx.bin",
             (unsigned long long)key);
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return false;

    uint32_t hdr[7] = {0};
    bool ok = (SDL_ReadIO(io, hdr, sizeof(hdr)) == sizeof(hdr) &&
               hdr[0] == JCE_IBL_CACHE_MAGIC && hdr[1] == JCE_IBL_CACHE_VERSION);
    uint16_t *id = NULL, *pd = NULL;
    if (ok) {
        *irr_face = hdr[2]; *irr_words = hdr[3];
        *pf_face  = hdr[4]; *pf_mips   = hdr[5]; *pf_words = hdr[6];
        size_t ib = (size_t)*irr_words * sizeof(uint16_t);
        size_t pb = (size_t)*pf_words  * sizeof(uint16_t);
        id = (uint16_t *)JCE_MALLOC(ib);
        pd = (uint16_t *)JCE_MALLOC(pb);
        ok = (id && pd &&
              SDL_ReadIO(io, id, ib) == ib &&
              SDL_ReadIO(io, pd, pb) == pb);
    }
    SDL_CloseIO(io);
    if (!ok) { JCE_FREE(id); JCE_FREE(pd); return false; }
    *irr = id; *pf = pd;
    return true;
}

static void ibl_cache_store(uint64_t key,
                            const uint16_t *irr, uint32_t irr_face, uint32_t irr_words,
                            const uint16_t *pf,  uint32_t pf_face,  uint32_t pf_mips,
                            uint32_t pf_words)
{
    jce_fs_host_create_directory(".jce");
    jce_fs_host_create_directory(".jce/cache");
    char path[256];
    snprintf(path, sizeof(path), ".jce/cache/ibl_%016llx.bin",
             (unsigned long long)key);
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return;
    uint32_t hdr[7] = { JCE_IBL_CACHE_MAGIC, JCE_IBL_CACHE_VERSION,
                        irr_face, irr_words, pf_face, pf_mips, pf_words };
    SDL_WriteIO(io, hdr, sizeof(hdr));
    SDL_WriteIO(io, irr, (size_t)irr_words * sizeof(uint16_t));
    SDL_WriteIO(io, pf,  (size_t)pf_words  * sizeof(uint16_t));
    SDL_CloseIO(io);
}

static bgfx_texture_handle_t ibl_upload_cube(const uint16_t *data,
                                             uint32_t face_size, uint32_t words,
                                             bool has_mips)
{
    if (!data || words == 0) return ibl_invalid_tex_handle();
    const bgfx_memory_t *mem = bgfx_copy(data, words * sizeof(uint16_t));
    return bgfx_create_texture_cube(
        (uint16_t)face_size, has_mips, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        mem, 0);
}

/* CPU-side baked cubemap face data (no GPU handles) — produced by
 * jce_ibl_bake_cpu() on a worker, consumed by jce_ibl_upload_cpu() on main. */
struct JceIblCpuData {
    uint16_t *irr; uint32_t irr_face, irr_words;
    uint16_t *pf;  uint32_t pf_face, pf_mips, pf_words;
};

JceIblCpuData *jce_ibl_bake_cpu(const float *pixels,
                                uint32_t width, uint32_t height,
                                uint32_t irradiance_size,
                                uint32_t prefilter_size)
{
    if (!pixels || width == 0 || height == 0) {
        LOG_WARN(LOG_TAG, "jce_ibl_bake_cpu: invalid pixel data");
        return NULL;
    }
    if (irradiance_size == 0) irradiance_size = 32;
    if (prefilter_size  == 0) prefilter_size  = 128;

    JceIblCpuData *cpu = (JceIblCpuData *)JCE_CALLOC(1, sizeof(*cpu));
    if (!cpu) return NULL;
    cpu->irr_face  = irradiance_size;
    cpu->irr_words = irradiance_size * irradiance_size * 6u * 4u;
    cpu->pf_face   = prefilter_size;

    uint64_t key = ibl_content_hash(pixels, width * height * 4u,
                                    irradiance_size, prefilter_size);

    if (ibl_cache_load(key, &cpu->irr, &cpu->irr_face, &cpu->irr_words,
                       &cpu->pf, &cpu->pf_face, &cpu->pf_mips, &cpu->pf_words)) {
        LOG_INFO(LOG_TAG, "IBL: reloaded baked cubemaps from cache "
                 "(.jce/cache/ibl_%016llx.bin)", (unsigned long long)key);
    } else {
        LOG_INFO(LOG_TAG, "IBL bake: equirect %ux%u -> irradiance %u, "
                 "prefilter %u (one-time; cached for next load)",
                 width, height, irradiance_size, prefilter_size);
        cpu->irr = ibl_compute_irradiance(pixels, width, height, cpu->irr_face);
        cpu->pf  = ibl_compute_prefilter(pixels, width, height, cpu->pf_face,
                                         &cpu->pf_mips, &cpu->pf_words);
        if (cpu->irr && cpu->pf)
            ibl_cache_store(key, cpu->irr, cpu->irr_face, cpu->irr_words,
                            cpu->pf, cpu->pf_face, cpu->pf_mips, cpu->pf_words);
    }

    if (!cpu->irr || !cpu->pf) {
        LOG_WARN(LOG_TAG, "jce_ibl_bake_cpu: convolution/cache failed");
        jce_ibl_cpu_free(cpu);
        return NULL;
    }
    return cpu;
}

JceIblData *jce_ibl_upload_cpu(JceIblCpuData *cpu)
{
    if (!cpu) return NULL;
    JceIblData *ibl = (JceIblData *)JCE_CALLOC(1, sizeof(*ibl));
    if (!ibl) { jce_ibl_cpu_free(cpu); return NULL; }
    ibl->brdf_lut       = ibl_invalid_tex_handle();
    ibl->prefilter_mips = cpu->pf_mips;
    ibl->irradiance = ibl_upload_cube(cpu->irr, cpu->irr_face, cpu->irr_words,
                                      false);
    ibl->prefilter  = ibl_upload_cube(cpu->pf,  cpu->pf_face,  cpu->pf_words,
                                      true);
    jce_ibl_cpu_free(cpu);

    if (!BGFX_HANDLE_IS_VALID(ibl->irradiance) ||
        !BGFX_HANDLE_IS_VALID(ibl->prefilter)) {
        LOG_WARN(LOG_TAG, "IBL upload failed (irradiance/prefilter)");
        jce_ibl_destroy(ibl);
        return NULL;
    }
    return ibl;
}

void jce_ibl_cpu_free(JceIblCpuData *cpu)
{
    if (!cpu) return;
    JCE_FREE(cpu->irr);
    JCE_FREE(cpu->pf);
    JCE_FREE(cpu);
}

/* Synchronous convenience wrapper (bake + upload on the calling thread). */
JceIblData *jce_ibl_generate_from_pixels(const float *pixels,
                                         uint32_t width, uint32_t height,
                                         uint32_t irradiance_size,
                                         uint32_t prefilter_size)
{
    JceIblCpuData *cpu = jce_ibl_bake_cpu(pixels, width, height,
                                          irradiance_size, prefilter_size);
    if (!cpu) return NULL;
    return jce_ibl_upload_cpu(cpu);
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

JceTexture jce_ibl_get_irradiance(const JceIblData *ibl)
{
    if (!ibl) return JCE_TEXTURE_INVALID;
    return (JceTexture){ ibl->irradiance.idx };
}

JceTexture jce_ibl_get_prefilter(const JceIblData *ibl)
{
    if (!ibl) return JCE_TEXTURE_INVALID;
    return (JceTexture){ ibl->prefilter.idx };
}

JceTexture jce_ibl_get_brdf_lut(const JceIblData *ibl)
{
    if (!ibl) return JCE_TEXTURE_INVALID;
    return (JceTexture){ ibl->brdf_lut.idx };
}

uint32_t jce_ibl_get_prefilter_mips(const JceIblData *ibl)
{
    if (!ibl) return 0;
    return ibl->prefilter_mips;
}
