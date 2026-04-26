/*
 * jce_yuv_convert.c  YUV420P → RGBA8 with SSE2 / NEON / scalar paths.
 *
 * BT.601 limited-range conversion (Y 16-235, UV 16-240):
 *   R = clip( (298*(Y-16) + 409*(V-128) + 128) >> 8 )
 *   G = clip( (298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8 )
 *   B = clip( (298*(Y-16) + 516*(U-128) + 128) >> 8 )
 *
 * Intermediates reach ~71k (298*239) — exceeding signed int16 range.
 * SSE2 uses _mm_madd_epi16 (16×16→32) and NEON uses vmull_s16 (widening)
 * to keep all arithmetic in 32-bit before packing back with saturation.
 */

#include "jce_yuv_convert.h"
#include <string.h>

/* ── SSE2 fast path (32-bit intermediates via madd) ──────────────── */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define JCE_YUV_HAS_SSE2 1
#include <emmintrin.h>

static void yuv420_to_rgba_sse2(const uint8_t *y_plane, int y_stride,
                                const uint8_t *u_plane, int u_stride,
                                const uint8_t *v_plane, int v_stride,
                                uint8_t *rgba, uint32_t width, uint32_t height)
{
    /*
     * _mm_madd_epi16([a0,b0,a1,b1,...], [c0,d0,c1,d1,...])
     *   = [a0*c0+b0*d0, a1*c1+b1*d1, ...] as int32
     *
     * We interleave [Y',V'] with coeff [298,409] for R,
     *               [Y',U'] with coeff [298,-100] for G (minus 208*V separately),
     *               [Y',U'] with coeff [298,516]  for B.
     */
    const __m128i coeff_rv = _mm_set_epi16(409, 298, 409, 298,
                                           409, 298, 409, 298);
    const __m128i coeff_gu = _mm_set_epi16(-100, 298, -100, 298,
                                           -100, 298, -100, 298);
    const __m128i coeff_gv = _mm_set_epi16(0, 208, 0, 208,
                                           0, 208, 0, 208);
    const __m128i coeff_bu = _mm_set_epi16(516, 298, 516, 298,
                                           516, 298, 516, 298);
    const __m128i c_16     = _mm_set1_epi16(16);
    const __m128i c_128w   = _mm_set1_epi16(128);
    const __m128i c_bias32 = _mm_set1_epi32(128);
    const __m128i c_zero   = _mm_setzero_si128();
    const __m128i c_ff     = _mm_set1_epi8('\xFF');

    const uint32_t simd_w = width & ~7u; /* 8 pixels at a time */

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp  = y_plane + row * y_stride;
        const uint8_t *up  = u_plane + (row / 2) * u_stride;
        const uint8_t *vp  = v_plane + (row / 2) * v_stride;
        uint8_t       *dst = rgba + row * width * 4;
        uint32_t col = 0;

        for (; col < simd_w; col += 8) {
            /* Load 8 Y bytes → 8 x u16. */
            __m128i y16 = _mm_unpacklo_epi8(
                _mm_loadl_epi64((const __m128i *)(yp + col)), c_zero);

            /* Load 4 U/V bytes, widen, duplicate for chroma subsampling. */
            __m128i u4 = _mm_unpacklo_epi8(
                _mm_cvtsi32_si128(*(const int *)(up + col / 2)), c_zero);
            __m128i v4 = _mm_unpacklo_epi8(
                _mm_cvtsi32_si128(*(const int *)(vp + col / 2)), c_zero);
            __m128i u16 = _mm_unpacklo_epi16(u4, u4); /* U0,U0,U1,U1,... */
            __m128i v16 = _mm_unpacklo_epi16(v4, v4);

            /* Subtract offsets → signed 16-bit. */
            __m128i yy = _mm_sub_epi16(y16, c_16);
            __m128i uu = _mm_sub_epi16(u16, c_128w);
            __m128i vv = _mm_sub_epi16(v16, c_128w);

            /* ── Low 4 pixels ── */
            __m128i yv_lo = _mm_unpacklo_epi16(yy, vv);
            __m128i yu_lo = _mm_unpacklo_epi16(yy, uu);
            __m128i vz_lo = _mm_unpacklo_epi16(vv, c_zero);

            __m128i r32_lo = _mm_srai_epi32(
                _mm_add_epi32(_mm_madd_epi16(yv_lo, coeff_rv), c_bias32), 8);
            __m128i g32_lo = _mm_srai_epi32(
                _mm_add_epi32(
                    _mm_sub_epi32(_mm_madd_epi16(yu_lo, coeff_gu),
                                  _mm_madd_epi16(vz_lo, coeff_gv)),
                    c_bias32), 8);
            __m128i b32_lo = _mm_srai_epi32(
                _mm_add_epi32(_mm_madd_epi16(yu_lo, coeff_bu), c_bias32), 8);

            /* ── High 4 pixels ── */
            __m128i yy_hi = _mm_srli_si128(yy, 8);
            __m128i uu_hi = _mm_srli_si128(uu, 8);
            __m128i vv_hi = _mm_srli_si128(vv, 8);

            __m128i yv_hi = _mm_unpacklo_epi16(yy_hi, vv_hi);
            __m128i yu_hi = _mm_unpacklo_epi16(yy_hi, uu_hi);
            __m128i vz_hi = _mm_unpacklo_epi16(vv_hi, c_zero);

            __m128i r32_hi = _mm_srai_epi32(
                _mm_add_epi32(_mm_madd_epi16(yv_hi, coeff_rv), c_bias32), 8);
            __m128i g32_hi = _mm_srai_epi32(
                _mm_add_epi32(
                    _mm_sub_epi32(_mm_madd_epi16(yu_hi, coeff_gu),
                                  _mm_madd_epi16(vz_hi, coeff_gv)),
                    c_bias32), 8);
            __m128i b32_hi = _mm_srai_epi32(
                _mm_add_epi32(_mm_madd_epi16(yu_hi, coeff_bu), c_bias32), 8);

            /* Pack 32→16 (signed sat) → 16→8 (unsigned sat). */
            __m128i r16 = _mm_packs_epi32(r32_lo, r32_hi);
            __m128i g16 = _mm_packs_epi32(g32_lo, g32_hi);
            __m128i b16 = _mm_packs_epi32(b32_lo, b32_hi);
            __m128i r8  = _mm_packus_epi16(r16, r16); /* low 8 bytes valid */
            __m128i g8  = _mm_packus_epi16(g16, g16);
            __m128i b8  = _mm_packus_epi16(b16, b16);

            /* Interleave R,G,B,A → RGBA8 for 8 pixels. */
            __m128i rg = _mm_unpacklo_epi8(r8, g8);
            __m128i ba = _mm_unpacklo_epi8(b8, c_ff);
            __m128i rgba_lo = _mm_unpacklo_epi16(rg, ba); /* pixels 0-3 */
            __m128i rgba_hi = _mm_unpackhi_epi16(rg, ba); /* pixels 4-7 */

            _mm_storeu_si128((__m128i *)(dst + col * 4 +  0), rgba_lo);
            _mm_storeu_si128((__m128i *)(dst + col * 4 + 16), rgba_hi);
        }

        /* Scalar tail for remaining pixels (0-7). */
        for (; col < width; ++col) {
            int y = (int)yp[col] - 16;
            int u = (int)up[col / 2] - 128;
            int v = (int)vp[col / 2] - 128;
            int c = y * 298;
            int r = (c + 409 * v + 128) >> 8;
            int g = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            dst[col * 4 + 0] = (uint8_t)r;
            dst[col * 4 + 1] = (uint8_t)g;
            dst[col * 4 + 2] = (uint8_t)b;
            dst[col * 4 + 3] = 255;
        }
    }
}
#endif /* SSE2 */

/* ── NEON fast path (32-bit intermediates via vmull) ─────────────── */

#if defined(__aarch64__) || defined(_M_ARM64)
#define JCE_YUV_HAS_NEON 1
#include <arm_neon.h>

static void yuv420_to_rgba_neon(const uint8_t *y_plane, int y_stride,
                                const uint8_t *u_plane, int u_stride,
                                const uint8_t *v_plane, int v_stride,
                                uint8_t *rgba, uint32_t width, uint32_t height)
{
    const int16x8_t c_16w  = vdupq_n_s16(16);
    const int16x8_t c_128w = vdupq_n_s16(128);
    const int16x4_t k_298  = vdup_n_s16(298);
    const int16x4_t k_409  = vdup_n_s16(409);
    const int16x4_t k_100  = vdup_n_s16(100);
    const int16x4_t k_208  = vdup_n_s16(208);
    const int16x4_t k_516  = vdup_n_s16(516);
    const int32x4_t c_bias = vdupq_n_s32(128);

    const uint32_t simd_w = width & ~7u; /* 8 pixels at a time */

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp  = y_plane + row * y_stride;
        const uint8_t *up  = u_plane + (row / 2) * u_stride;
        const uint8_t *vp  = v_plane + (row / 2) * v_stride;
        uint8_t       *dst = rgba + row * width * 4;
        uint32_t col = 0;

        for (; col < simd_w; col += 8) {
            uint8x8_t y_bytes = vld1_u8(yp + col);
            uint8x8_t u_raw   = vld1_u8(up + col / 2);
            uint8x8_t v_raw   = vld1_u8(vp + col / 2);

            /* Duplicate chroma: U0,U0,U1,U1,U2,U2,U3,U3. */
            uint8x8x2_t u_zip = vzip_u8(u_raw, u_raw);
            uint8x8x2_t v_zip = vzip_u8(v_raw, v_raw);

            int16x8_t yy = vsubq_s16(
                vreinterpretq_s16_u16(vmovl_u8(y_bytes)), c_16w);
            int16x8_t uu = vsubq_s16(
                vreinterpretq_s16_u16(vmovl_u8(u_zip.val[0])), c_128w);
            int16x8_t vv = vsubq_s16(
                vreinterpretq_s16_u16(vmovl_u8(v_zip.val[0])), c_128w);

            /* ── Low 4 pixels (widening 16×16→32) ── */
            int16x4_t yy_lo = vget_low_s16(yy);
            int16x4_t uu_lo = vget_low_s16(uu);
            int16x4_t vv_lo = vget_low_s16(vv);

            int32x4_t c_lo = vmull_s16(yy_lo, k_298);
            int32x4_t r_lo = vshrq_n_s32(
                vaddq_s32(vmlal_s16(c_lo, vv_lo, k_409), c_bias), 8);
            int32x4_t g_lo = vshrq_n_s32(
                vaddq_s32(vmlsl_s16(
                    vmlsl_s16(c_lo, uu_lo, k_100), vv_lo, k_208), c_bias), 8);
            int32x4_t b_lo = vshrq_n_s32(
                vaddq_s32(vmlal_s16(c_lo, uu_lo, k_516), c_bias), 8);

            /* ── High 4 pixels ── */
            int16x4_t yy_hi = vget_high_s16(yy);
            int16x4_t uu_hi = vget_high_s16(uu);
            int16x4_t vv_hi = vget_high_s16(vv);

            int32x4_t c_hi = vmull_s16(yy_hi, k_298);
            int32x4_t r_hi = vshrq_n_s32(
                vaddq_s32(vmlal_s16(c_hi, vv_hi, k_409), c_bias), 8);
            int32x4_t g_hi = vshrq_n_s32(
                vaddq_s32(vmlsl_s16(
                    vmlsl_s16(c_hi, uu_hi, k_100), vv_hi, k_208), c_bias), 8);
            int32x4_t b_hi = vshrq_n_s32(
                vaddq_s32(vmlal_s16(c_hi, uu_hi, k_516), c_bias), 8);

            /* Pack 32→16→8 with saturation. */
            int16x8_t r16 = vcombine_s16(vqmovn_s32(r_lo), vqmovn_s32(r_hi));
            int16x8_t g16 = vcombine_s16(vqmovn_s32(g_lo), vqmovn_s32(g_hi));
            int16x8_t b16 = vcombine_s16(vqmovn_s32(b_lo), vqmovn_s32(b_hi));
            uint8x8_t r8 = vqmovun_s16(r16);
            uint8x8_t g8 = vqmovun_s16(g16);
            uint8x8_t b8 = vqmovun_s16(b16);

            uint8x8x4_t out;
            out.val[0] = r8;
            out.val[1] = g8;
            out.val[2] = b8;
            out.val[3] = vdup_n_u8(255);
            vst4_u8(dst + col * 4, out);
        }

        /* Scalar tail. */
        for (; col < width; ++col) {
            int y = (int)yp[col] - 16;
            int u = (int)up[col / 2] - 128;
            int v = (int)vp[col / 2] - 128;
            int c = y * 298;
            int r = (c + 409 * v + 128) >> 8;
            int g = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            dst[col * 4 + 0] = (uint8_t)r;
            dst[col * 4 + 1] = (uint8_t)g;
            dst[col * 4 + 2] = (uint8_t)b;
            dst[col * 4 + 3] = 255;
        }
    }
}
#endif /* NEON */

/* ── Scalar fallback ─────────────────────────────────────────────── */

static void yuv420_to_rgba_scalar(const uint8_t *y_plane, int y_stride,
                                  const uint8_t *u_plane, int u_stride,
                                  const uint8_t *v_plane, int v_stride,
                                  uint8_t *rgba, uint32_t width, uint32_t height)
{
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp = y_plane + row * y_stride;
        const uint8_t *up = u_plane + (row / 2) * u_stride;
        const uint8_t *vp = v_plane + (row / 2) * v_stride;
        uint8_t *dst = rgba + row * width * 4;

        for (uint32_t col = 0; col < width; ++col) {
            int y = (int)yp[col] - 16;
            int u = (int)up[col / 2] - 128;
            int v = (int)vp[col / 2] - 128;
            int c = y * 298;
            int r = (c + 409 * v + 128) >> 8;
            int g = (c - 100 * u - 208 * v + 128) >> 8;
            int b = (c + 516 * u + 128) >> 8;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            dst[col * 4 + 0] = (uint8_t)r;
            dst[col * 4 + 1] = (uint8_t)g;
            dst[col * 4 + 2] = (uint8_t)b;
            dst[col * 4 + 3] = 255;
        }
    }
}

/* ── Public dispatch ─────────────────────────────────────────────── */

/* Runtime flag set by jce_yuv_set_avx2() — checked in the x86 dispatch path. */
static int s_yuv_use_avx2 = 0;

void jce_yuv_set_avx2(int enabled)
{
    s_yuv_use_avx2 = enabled;
}

#if JCE_YUV_HAS_SSE2
/* Defined in jce_yuv_convert_avx2.c, compiled with /arch:AVX2 or -mavx2. */
extern void jce_yuv420_to_rgba_avx2(const uint8_t *y_plane, int y_stride,
                                    const uint8_t *u_plane, int u_stride,
                                    const uint8_t *v_plane, int v_stride,
                                    uint8_t *rgba, uint32_t width, uint32_t height);
#endif

void jce_yuv420_to_rgba(const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        uint8_t *rgba, uint32_t width, uint32_t height)
{
    if (!y_plane || !u_plane || !v_plane || !rgba || width == 0 || height == 0)
        return;

#if JCE_YUV_HAS_SSE2
    if (s_yuv_use_avx2) {
        jce_yuv420_to_rgba_avx2(y_plane, y_stride, u_plane, u_stride,
                                v_plane, v_stride, rgba, width, height);
        return;
    }
    yuv420_to_rgba_sse2(y_plane, y_stride, u_plane, u_stride,
                        v_plane, v_stride, rgba, width, height);
#elif JCE_YUV_HAS_NEON
    yuv420_to_rgba_neon(y_plane, y_stride, u_plane, u_stride,
                        v_plane, v_stride, rgba, width, height);
#else
    yuv420_to_rgba_scalar(y_plane, y_stride, u_plane, u_stride,
                          v_plane, v_stride, rgba, width, height);
#endif
}
