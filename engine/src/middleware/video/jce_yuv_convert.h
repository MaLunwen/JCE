/*
 * jce_yuv_convert.h  YUV420P → RGBA8 conversion with SIMD fast path.
 */

#ifndef JCE_YUV_CONVERT_H
#define JCE_YUV_CONVERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert planar YUV420P to packed RGBA8 (BT.601 limited range).
 * Uses AVX2 (16px/iter) > SSE2 (8px/iter) > NEON > scalar, selected at runtime.
 * Alpha channel is always set to 255. */
void jce_yuv420_to_rgba(const uint8_t *y_plane, int y_stride,
                        const uint8_t *u_plane, int u_stride,
                        const uint8_t *v_plane, int v_stride,
                        uint8_t *rgba, uint32_t width, uint32_t height);

/* Call once at startup (before any jce_yuv420_to_rgba) to enable AVX2 path.
 * Pass the result of SDL_HasAVX2() or equivalent runtime CPUID check. */
void jce_yuv_set_avx2(int enabled);

#ifdef __cplusplus
}
#endif

#endif /* JCE_YUV_CONVERT_H */
