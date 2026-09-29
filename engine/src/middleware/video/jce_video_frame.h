/* Worker-owned preview preparation; no renderer or container dependencies. */
#ifndef JCE_VIDEO_FRAME_H
#define JCE_VIDEO_FRAME_H
#include "jce_yuv_convert.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    JceYuvPreview preview;
    uint8_t *rgba;
    size_t capacity;
    int width, height;
} JceVideoFrameProcessor;

bool jce_video_frame_prepare(JceVideoFrameProcessor *processor,
                            const JceYuv420Frame *input, uint32_t limit);
void jce_video_frame_destroy(JceVideoFrameProcessor *processor);
#ifdef __cplusplus
}
#endif
#endif
