#include "jce_video_frame.h"
#include "os/core/jce_memory.h"
#include <string.h>

bool jce_video_frame_prepare(JceVideoFrameProcessor *processor,
                            const JceYuv420Frame *input, uint32_t limit)
{
    JceYuv420Frame output;
    size_t bytes;
    if (!processor || !input) return false;
    processor->preview.max_dimension = limit;
    if (!jce_yuv_preview_prepare(&processor->preview, input, &output))
        return false;
    if ((size_t)output.width > SIZE_MAX / 4u / (size_t)output.height)
        return false;
    bytes = (size_t)output.width * (size_t)output.height * 4u;
    if (!jce_yuv_buffer_reserve(&processor->rgba, &processor->capacity, bytes))
        return false;
    jce_yuv420_to_rgba(output.y, output.y_stride, output.u, output.uv_stride,
                      output.v, output.uv_stride, processor->rgba,
                      (uint32_t)output.width, (uint32_t)output.height);
    processor->width = output.width;
    processor->height = output.height;
    return true;
}

void jce_video_frame_destroy(JceVideoFrameProcessor *processor)
{
    if (!processor) return;
    JCE_FREE(processor->rgba);
    JCE_FREE(processor->preview.buffer);
    memset(processor, 0, sizeof(*processor));
}
