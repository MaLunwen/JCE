/*
 * jce_yuv_convert_avx2.c  YUV420P → RGBA8, AVX2 fast path (16 px/iter).
 *
 * Must be compiled with /arch:AVX2 (MSVC) or -mavx2 (GCC/Clang).
 * Called only when SDL_HasAVX2() returns true; do NOT include directly.
 * Same BT.601 limited-range math as jce_yuv_convert.c.
 */

#include "jce_yuv_convert.h"
#include <immintrin.h>

void jce_yuv420_to_rgba_avx2(const uint8_t *y_plane, int y_stride,
                              const uint8_t *u_plane, int u_stride,
                              const uint8_t *v_plane, int v_stride,
                              uint8_t *rgba, uint32_t width, uint32_t height)
{
    /*
     * AVX2 processes 16 Y pixels per iteration using 256-bit ymm registers.
     *
     * Chroma subsampling: U/V are 4:2:0, so 8 U/V samples cover 16 Y lumas.
     * We load 8 U bytes, 8 V bytes, and duplicate each using vpunpcklbw with
     * itself → U0 U0 U1 U1 ... U7 U7 (16 entries, filling a 256-bit lane).
     *
     * We use the same _mm256_madd_epi16 trick as the SSE2 path but operate
     * on two 128-bit "lanes" simultaneously, doubling throughput.
     */
    const __m256i coeff_rv  = _mm256_set1_epi32((int)(409u << 16) | 298u);
    const __m256i coeff_gu  = _mm256_set1_epi32((unsigned int)((unsigned short)(-100) << 16) | 298u);
    const __m256i coeff_gv  = _mm256_set1_epi32((int)(208u << 16));
    const __m256i coeff_bu  = _mm256_set1_epi32((int)(516u << 16) | 298u);
    const __m256i c_16      = _mm256_set1_epi16(16);
    const __m256i c_128w    = _mm256_set1_epi16(128);
    const __m256i c_bias32  = _mm256_set1_epi32(128);
    /* Alpha=255 bytes interleaved as 16-bit words for packus path. */
    const __m256i c_alpha16 = _mm256_set1_epi16(255);

    const uint32_t simd_w = width & ~15u; /* 16 pixels per AVX2 iteration */

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *yp  = y_plane + (size_t)row * y_stride;
        const uint8_t *up  = u_plane + (size_t)(row / 2) * u_stride;
        const uint8_t *vp  = v_plane + (size_t)(row / 2) * v_stride;
        uint8_t       *dst = rgba + (size_t)row * width * 4;
        uint32_t col = 0;

        for (; col < simd_w; col += 16) {
            /* ── Load 16 Y bytes → 16 × s16 (ymm). ── */
            __m128i y_bytes = _mm_loadu_si128((const __m128i *)(yp + col));
            __m256i y16     = _mm256_cvtepu8_epi16(y_bytes);

            /* ── Load 8 U bytes, 8 V bytes, duplicate for 4:2:0 → 16 each. ── */
            __m128i u_raw = _mm_loadl_epi64((const __m128i *)(up + col / 2));
            __m128i v_raw = _mm_loadl_epi64((const __m128i *)(vp + col / 2));

            /*
             * Duplicate each u8 chroma sample at the byte level first, THEN widen
             * to u16.  This fills BOTH 128-bit lanes of the ymm register correctly.
             *
             * _mm256_cvtepu8_epi16 only fills the LO lane from the 64-bit payload,
             * so a subsequent _mm256_unpacklo_epi16(x,x) would leave the HI lane as
             * all-zero (U/V = −128 after offset subtraction) → green/red stripe bug.
             *
             * Correct sequence:
             *   _mm_unpacklo_epi8(u_raw, u_raw) → [U0 U0 U1 U1 … U7 U7] as u8 (128b)
             *   _mm256_cvtepu8_epi16(...)        → lo=[U0 U0 U1 U1 U2 U2 U3 U3]
             *                                       hi=[U4 U4 U5 U5 U6 U6 U7 U7] (u16)
             */
            __m128i u_dup = _mm_unpacklo_epi8(u_raw, u_raw); /* byte-level duplication */
            __m128i v_dup = _mm_unpacklo_epi8(v_raw, v_raw);
            __m256i u16   = _mm256_cvtepu8_epi16(u_dup);     /* 16 × u16, both lanes filled */
            __m256i v16   = _mm256_cvtepu8_epi16(v_dup);

            /* ── Subtract offsets → signed 16-bit. ── */
            __m256i yy = _mm256_sub_epi16(y16, c_16);
            __m256i uu = _mm256_sub_epi16(u16, c_128w);
            __m256i vv = _mm256_sub_epi16(v16, c_128w);

            /* ── Low 8 pixels (lo 128-bit lane of each ymm). ── */
            __m128i yy_lo128 = _mm256_castsi256_si128(yy);
            __m128i uu_lo128 = _mm256_castsi256_si128(uu);
            __m128i vv_lo128 = _mm256_castsi256_si128(vv);

#define COMPUTE8(ylo, ulo, vlo, r32, g32, b32)                                 \
    do {                                                                         \
        __m128i yv = _mm_unpacklo_epi16(ylo, vlo);                              \
        __m128i yu = _mm_unpacklo_epi16(ylo, ulo);                              \
        __m128i vz = _mm_unpacklo_epi16(vlo, _mm_setzero_si128());              \
        __m128i yvh= _mm_unpackhi_epi16(ylo, vlo);                              \
        __m128i yuh= _mm_unpackhi_epi16(ylo, ulo);                              \
        __m128i vzh= _mm_unpackhi_epi16(vlo, _mm_setzero_si128());              \
        __m128i rv = _mm256_castsi256_si128(coeff_rv);                          \
        __m128i gu = _mm256_castsi256_si128(coeff_gu);                          \
        __m128i gv = _mm256_castsi256_si128(coeff_gv);                          \
        __m128i bu = _mm256_castsi256_si128(coeff_bu);                          \
        __m128i b32 = _mm256_castsi256_si128(c_bias32);                         \
        __m128i rl = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv,  rv), b32), 8); \
        __m128i rh = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvh, rv), b32), 8); \
        __m128i gl = _mm_srai_epi32(_mm_add_epi32(                              \
            _mm_sub_epi32(_mm_madd_epi16(yu,  gu), _mm_madd_epi16(vz,  gv)), b32), 8); \
        __m128i gh = _mm_srai_epi32(_mm_add_epi32(                              \
            _mm_sub_epi32(_mm_madd_epi16(yuh, gu), _mm_madd_epi16(vzh, gv)), b32), 8); \
        __m128i bl = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu,  bu), b32), 8); \
        __m128i bh = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuh, bu), b32), 8); \
        (r32) = _mm256_set_m128i(rh, rl);                                       \
        (g32) = _mm256_set_m128i(gh, gl);                                       \
        (b32) = _mm256_set_m128i(bh, bl);                                       \
    } while (0)

            /* The macro approach is messy. Use a cleaner inline approach: */
            /* Process low 8 pixels directly. */
            __m256i r32_lo, g32_lo, b32_lo;
            {
                __m128i yv = _mm_unpacklo_epi16(yy_lo128, vv_lo128);
                __m128i yu = _mm_unpacklo_epi16(yy_lo128, uu_lo128);
                __m128i vz = _mm_unpacklo_epi16(vv_lo128, _mm_setzero_si128());
                __m128i yvh= _mm_unpackhi_epi16(yy_lo128, vv_lo128);
                __m128i yuh= _mm_unpackhi_epi16(yy_lo128, uu_lo128);
                __m128i vzh= _mm_unpackhi_epi16(vv_lo128, _mm_setzero_si128());
                __m128i rv = _mm256_castsi256_si128(coeff_rv);
                __m128i gu = _mm256_castsi256_si128(coeff_gu);
                __m128i gv = _mm256_castsi256_si128(coeff_gv);
                __m128i bu = _mm256_castsi256_si128(coeff_bu);
                __m128i bias= _mm256_castsi256_si128(c_bias32);
                __m128i rl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv, rv), bias), 8);
                __m128i rh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvh, rv), bias), 8);
                __m128i gl  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yu, gu), _mm_madd_epi16(vz, gv)), bias), 8);
                __m128i gh  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yuh, gu), _mm_madd_epi16(vzh, gv)), bias), 8);
                __m128i bl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu, bu), bias), 8);
                __m128i bh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuh, bu), bias), 8);
                r32_lo = _mm256_set_m128i(rh, rl);
                g32_lo = _mm256_set_m128i(gh, gl);
                b32_lo = _mm256_set_m128i(bh, bl);
            }

            /* Process high 8 pixels. */
            __m128i yy_hi128 = _mm256_extracti128_si256(yy, 1);
            __m128i uu_hi128 = _mm256_extracti128_si256(uu, 1);
            __m128i vv_hi128 = _mm256_extracti128_si256(vv, 1);
            __m256i r32_hi, g32_hi, b32_hi;
            {
                __m128i yv = _mm_unpacklo_epi16(yy_hi128, vv_hi128);
                __m128i yu = _mm_unpacklo_epi16(yy_hi128, uu_hi128);
                __m128i vz = _mm_unpacklo_epi16(vv_hi128, _mm_setzero_si128());
                __m128i yvh= _mm_unpackhi_epi16(yy_hi128, vv_hi128);
                __m128i yuh= _mm_unpackhi_epi16(yy_hi128, uu_hi128);
                __m128i vzh= _mm_unpackhi_epi16(vv_hi128, _mm_setzero_si128());
                __m128i rv = _mm256_castsi256_si128(coeff_rv);
                __m128i gu = _mm256_castsi256_si128(coeff_gu);
                __m128i gv = _mm256_castsi256_si128(coeff_gv);
                __m128i bu = _mm256_castsi256_si128(coeff_bu);
                __m128i bias= _mm256_castsi256_si128(c_bias32);
                __m128i rl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv, rv), bias), 8);
                __m128i rh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvh, rv), bias), 8);
                __m128i gl  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yu, gu), _mm_madd_epi16(vz, gv)), bias), 8);
                __m128i gh  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yuh, gu), _mm_madd_epi16(vzh, gv)), bias), 8);
                __m128i bl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu, bu), bias), 8);
                __m128i bh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuh, bu), bias), 8);
                r32_hi = _mm256_set_m128i(rh, rl);
                g32_hi = _mm256_set_m128i(gh, gl);
                b32_hi = _mm256_set_m128i(bh, bl);
            }

            /* Pack 32→16→8 and store.
             *
             * r32_lo holds pixels 0-7 as 8 × s32 (lo128=[R0..R3], hi128=[R4..R7]).
             * r32_hi holds pixels 8-15 similarly.
             * We cannot use _mm256_packs_epi32(r32_lo, r32_hi) because AVX2
             * operates per-lane, producing a non-linear element order.
             * Instead, extract and pack each group of 8 independently.
             */
            __m128i zero128 = _mm_setzero_si128();
            __m128i a8_128  = _mm_set1_epi8('\xFF');

            /* Pixels 0-7 (from r32_lo / g32_lo / b32_lo). */
            {
                __m128i r16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(r32_lo),
                    _mm256_extracti128_si256(r32_lo, 1));   /* [R0..R7] */
                __m128i g16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(g32_lo),
                    _mm256_extracti128_si256(g32_lo, 1));
                __m128i b16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(b32_lo),
                    _mm256_extracti128_si256(b32_lo, 1));
                __m128i r8  = _mm_packus_epi16(r16, zero128);
                __m128i g8  = _mm_packus_epi16(g16, zero128);
                __m128i b8  = _mm_packus_epi16(b16, zero128);
                __m128i rg  = _mm_unpacklo_epi8(r8, g8);
                __m128i ba  = _mm_unpacklo_epi8(b8, a8_128);
                _mm_storeu_si128((__m128i *)(dst + col * 4 +  0), _mm_unpacklo_epi16(rg, ba));
                _mm_storeu_si128((__m128i *)(dst + col * 4 + 16), _mm_unpackhi_epi16(rg, ba));
            }

            /* Pixels 8-15 (from r32_hi / g32_hi / b32_hi). */
            {
                __m128i r16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(r32_hi),
                    _mm256_extracti128_si256(r32_hi, 1));   /* [R8..R15] */
                __m128i g16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(g32_hi),
                    _mm256_extracti128_si256(g32_hi, 1));
                __m128i b16 = _mm_packs_epi32(
                    _mm256_castsi256_si128(b32_hi),
                    _mm256_extracti128_si256(b32_hi, 1));
                __m128i r8  = _mm_packus_epi16(r16, zero128);
                __m128i g8  = _mm_packus_epi16(g16, zero128);
                __m128i b8  = _mm_packus_epi16(b16, zero128);
                __m128i rg  = _mm_unpacklo_epi8(r8, g8);
                __m128i ba  = _mm_unpacklo_epi8(b8, a8_128);
                _mm_storeu_si128((__m128i *)(dst + col * 4 + 32), _mm_unpacklo_epi16(rg, ba));
                _mm_storeu_si128((__m128i *)(dst + col * 4 + 48), _mm_unpackhi_epi16(rg, ba));
            }

            (void)c_alpha16;
        }

        /* SSE2 tail for remaining 0-15 pixels (at most one SSE2 iteration of 8,
         * then a scalar tail for 0-7). */
        for (; col + 8 <= width; col += 8) {
            __m128i y16 = _mm_unpacklo_epi8(
                _mm_loadl_epi64((const __m128i *)(yp + col)), _mm_setzero_si128());
            __m128i u4  = _mm_unpacklo_epi8(
                _mm_cvtsi32_si128(*(const int *)(up + col / 2)), _mm_setzero_si128());
            __m128i v4  = _mm_unpacklo_epi8(
                _mm_cvtsi32_si128(*(const int *)(vp + col / 2)), _mm_setzero_si128());
            __m128i u16 = _mm_unpacklo_epi16(u4, u4);
            __m128i v16 = _mm_unpacklo_epi16(v4, v4);
            __m128i yy  = _mm_sub_epi16(y16, _mm256_castsi256_si128(c_16));
            __m128i uu  = _mm_sub_epi16(u16, _mm256_castsi256_si128(c_128w));
            __m128i vv  = _mm_sub_epi16(v16, _mm256_castsi256_si128(c_128w));
            __m128i rv  = _mm256_castsi256_si128(coeff_rv);
            __m128i gu  = _mm256_castsi256_si128(coeff_gu);
            __m128i gv  = _mm256_castsi256_si128(coeff_gv);
            __m128i bu  = _mm256_castsi256_si128(coeff_bu);
            __m128i bias= _mm256_castsi256_si128(c_bias32);
            __m128i zero= _mm_setzero_si128();
            __m128i yv  = _mm_unpacklo_epi16(yy, vv);
            __m128i yu  = _mm_unpacklo_epi16(yy, uu);
            __m128i vz  = _mm_unpacklo_epi16(vv, zero);
            __m128i yvh = _mm_unpackhi_epi16(yy, vv);
            __m128i yuh = _mm_unpackhi_epi16(yy, uu);
            __m128i vzh = _mm_unpackhi_epi16(vv, zero);
            __m128i rl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv,  rv), bias), 8);
            __m128i rh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvh, rv), bias), 8);
            __m128i gl  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yu, gu), _mm_madd_epi16(vz, gv)), bias), 8);
            __m128i gh  = _mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_madd_epi16(yuh, gu), _mm_madd_epi16(vzh, gv)), bias), 8);
            __m128i bl  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu, bu), bias), 8);
            __m128i bh  = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuh, bu), bias), 8);
            __m128i r8s = _mm_packus_epi16(_mm_packs_epi32(rl, rh), zero);
            __m128i g8s = _mm_packus_epi16(_mm_packs_epi32(gl, gh), zero);
            __m128i b8s = _mm_packus_epi16(_mm_packs_epi32(bl, bh), zero);
            __m128i a8s = _mm_set1_epi8('\xFF');
            __m128i rg  = _mm_unpacklo_epi8(r8s, g8s);
            __m128i ba  = _mm_unpacklo_epi8(b8s, a8s);
            _mm_storeu_si128((__m128i *)(dst + col * 4 +  0), _mm_unpacklo_epi16(rg, ba));
            _mm_storeu_si128((__m128i *)(dst + col * 4 + 16), _mm_unpackhi_epi16(rg, ba));
        }

        /* Scalar tail (0-7 remaining). */
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

    _mm256_zeroupper(); /* avoid AVX-SSE transition penalty */
}
