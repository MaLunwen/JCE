/* Worker-owned preview preparation; no renderer or container dependencies. */
#ifndef JCE_VIDEO_FRAME_H
#define JCE_VIDEO_FRAME_H
#include "jce_yuv_convert.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Swap pointers/capacities between an entry plane and the display-side
 * scratch slot. Worker reuses entry buffers when refilling — sizes will
 * be fixed up by realloc-on-grow at next push, so size mismatches are
 * benign. */
static inline void jce_video_frame_swap_buffer(uint8_t **a, size_t *acap, uint8_t **b, size_t *bcap)
{
    uint8_t *tp = *a; *a = *b; *b = tp;
    size_t   tc = *acap; *acap = *bcap; *bcap = tc;
}

/* Ensure display_rgba has at least w*h*4 capacity (only realloc on
 * shortage; track w/h as the active payload extents). */
static inline bool jce_video_frame_rgba_bytes(int w, int h, size_t *bytes)
{
    if (!bytes || w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / 4u / (size_t)h)
        return false;
    *bytes = (size_t)w * (size_t)h * 4u;
    return true;
}

static inline bool jce_video_frame_reserve_rgba(uint8_t **data, size_t *capacity,
    int *width, int *height, int w, int h)
{
    size_t need;
    if (!jce_video_frame_rgba_bytes(w, h, &need)) return false;
    if (!jce_yuv_buffer_reserve(data, capacity, need))
        return false;
    *width = w;
    *height = h;
    return true;
}

typedef struct {
    JceYuvPreview preview;
    uint8_t *rgba;
    size_t capacity;
    int width, height;
    const char *perf_context;
    bool perf_checked, perf_enabled;
    uint32_t perf_interval_ms;
    uint64_t perf_frequency, perf_samples, perf_next_counter;
    double perf_scale_ema, perf_scale_last, perf_color_ema, perf_color_last;
} JceVideoFrameProcessor;

bool jce_video_frame_prepare(JceVideoFrameProcessor *processor,
                            const JceYuv420Frame *input, uint32_t limit);
/* Owner lends its existing display buffer and always recovers ownership,
 * including failure. No second persistent full-frame allocation is retained. */
bool jce_video_frame_prepare_display(JceVideoFrameProcessor *processor,
    const JceYuv420Frame *input, uint32_t limit, uint8_t **rgba, size_t *capacity,
    int *width, int *height);
void jce_video_frame_destroy(JceVideoFrameProcessor *processor);
#ifdef __cplusplus
}
#endif
#endif
