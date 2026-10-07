/* Owned I420 storage: codec feed/drain may invalidate borrowed planes. */
#ifndef JCE_H264_PLANES_H
#define JCE_H264_PLANES_H
#include "jce_yuv_convert.h"
#include <string.h>
#include <limits.h>
#include "os/core/jce_memory.h"

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    JceYuv420Frame frame;
    bool valid;
} JceH264Planes;

static inline bool jce_h264_planes_copy(JceH264Planes *out, uint8_t *const planes[3],
                                       int y_stride, int uv_stride, uint32_t width,
                                       uint32_t height)
{
    out->valid = false;
    if (!width || !height || width > INT_MAX || height > INT_MAX ||
        !planes[0] || !planes[1] || !planes[2]) return false;
    size_t chroma_w = width / 2u + width % 2u;
    size_t chroma_h = height / 2u + height % 2u;
    if (y_stride < (int)width || uv_stride < (int)chroma_w) return false;
    if ((size_t)width > SIZE_MAX / height || chroma_w > SIZE_MAX / chroma_h ||
        (size_t)y_stride > SIZE_MAX / height || (size_t)uv_stride > SIZE_MAX / chroma_h)
        return false;
    size_t luma = (size_t)width * height, chroma = chroma_w * chroma_h;
    if (chroma > (SIZE_MAX - luma) / 2u) return false;
    if (!jce_yuv_buffer_reserve(&out->bytes, &out->capacity, luma + 2u * chroma)) return false;
    uint8_t *u = out->bytes + luma, *v = u + chroma;
    for (uint32_t row = 0; row < height; ++row)
        memcpy(out->bytes + (size_t)row * width, planes[0] + (size_t)row * y_stride, width);
    for (size_t row = 0; row < chroma_h; ++row) {
        memcpy(u + row * chroma_w, planes[1] + row * (size_t)uv_stride, chroma_w);
        memcpy(v + row * chroma_w, planes[2] + row * (size_t)uv_stride, chroma_w);
    }
    // Pinned OpenH264 public decoder options expose SAR/profile, not CICP.
    // Preserve the adapter's existing BT.601 limited conversion contract.
    out->frame = (JceYuv420Frame){out->bytes, u, v, (int)width, (int)chroma_w,
                                (int)width, (int)height, {JCE_YUV_BT601, false}};
    out->valid = true;
    return true;
}
#endif
