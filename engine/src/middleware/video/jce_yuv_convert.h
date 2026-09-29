/*
 * jce_yuv_convert.h  YUV420P → RGBA8 conversion with SIMD fast path.
 */

#ifndef JCE_YUV_CONVERT_H
#define JCE_YUV_CONVERT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/* Bilinear downsample of a single 8-bit plane. Dimensions must be positive,
 * destination no larger than source, with stride >= width. No allocation. */
void jce_yuv_plane_downsample(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                             int src_stride, uint8_t *dst,
                             uint32_t dst_w, uint32_t dst_h);

typedef struct {
    const uint8_t *y, *u, *v;
    int y_stride, uv_stride;
    int width, height;
} JceYuv420Frame;

typedef struct {
    uint32_t max_dimension;
    uint8_t *buffer;
    size_t capacity;
} JceYuvPreview;

/* UI-owned scratch storage, reused between frames. Caller frees buffer on
 * unload. Native/default output borrows input without allocation. */
bool jce_yuv_preview_prepare(JceYuvPreview *preview, const JceYuv420Frame *input,
                             JceYuv420Frame *output);
bool jce_yuv_buffer_reserve(uint8_t **buffer, size_t *capacity, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* JCE_YUV_CONVERT_H */
