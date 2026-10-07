#include "jce_video_frame.h"
#include "os/core/jce_memory.h"
#include <string.h>
#include <stdlib.h>
#include <jce/os/core/jce_log.h>
#include "jce_video_perf.h"

static void frame_perf_init(JceVideoFrameProcessor *p)
{
    if (p->perf_checked) return;
    p->perf_checked = true;
    const char *enabled = getenv("JCE_DBG_VIDEO_FRAME_PERF");
    p->perf_enabled = enabled && strcmp(enabled, "1") == 0;
    /* Reuse the existing viewer diagnostic cadence; no second rate setting. */
    const char *cadence = getenv("JCE_DBG_FILE_PREVIEW_TRACE_MS");
    int ms = cadence ? atoi(cadence) : 1000;
    p->perf_interval_ms = ms >= 100 && ms <= 1000 ? (uint32_t)ms : 1000u;
}
static void frame_perf_report(JceVideoFrameProcessor *p, const JceYuv420Frame *input,
                              uint64_t begin, uint64_t scaled, uint64_t color_begin,
                              uint64_t end)
{
    if (!p->perf_enabled) return;
    jce_video_perf_ema_update(&p->perf_scale_ema, &p->perf_scale_last,
        jce_video_perf_us(&p->perf_frequency, begin, scaled));
    jce_video_perf_ema_update(&p->perf_color_ema, &p->perf_color_last,
        jce_video_perf_us(&p->perf_frequency, color_begin, end));
    ++p->perf_samples;
    if (end < p->perf_next_counter) return;
    p->perf_next_counter = end + p->perf_frequency * p->perf_interval_ms / 1000u;
    LOG_INFO("video_frame",
        "frame perf context=%s samples=%llu source=%dx%d preview=%dx%d scale_ms=%.6f color_ms=%.6f",
        p->perf_context ? p->perf_context : "worker", (unsigned long long)p->perf_samples, input->width, input->height,
        p->width, p->height, p->perf_scale_ema / 1000.0, p->perf_color_ema / 1000.0);
}

bool jce_video_frame_prepare(JceVideoFrameProcessor *processor,
                            const JceYuv420Frame *input, uint32_t limit)
{
    JceYuv420Frame output;
    size_t bytes;
    if (!processor || !input || !input->y || !input->u || !input->v
        || input->width <= 0 || input->height <= 0 || input->y_stride < input->width
        || input->uv_stride < input->width / 2 + input->width % 2) return false;
    frame_perf_init(processor);
    const uint64_t begin = jce_video_perf_begin(processor->perf_enabled, jce_time_perf_counter);
    processor->preview.max_dimension = limit;
    if (!jce_yuv_preview_prepare(&processor->preview, input, &output))
        return false;
    const uint64_t scaled = jce_video_perf_begin(processor->perf_enabled, jce_time_perf_counter);
    if (!jce_video_frame_rgba_bytes(output.width, output.height, &bytes))
        return false;
    if (!jce_yuv_buffer_reserve(&processor->rgba, &processor->capacity, bytes))
        return false;
    const uint64_t color_begin = jce_video_perf_begin(processor->perf_enabled, jce_time_perf_counter);
    jce_yuv420_to_rgba_color(output.y, output.y_stride, output.u, output.uv_stride,
                      output.v, output.uv_stride, processor->rgba,
                      (uint32_t)output.width, (uint32_t)output.height, output.color);
    processor->width = output.width;
    processor->height = output.height;
    const uint64_t end = jce_video_perf_begin(processor->perf_enabled, jce_time_perf_counter);
    frame_perf_report(processor, input, begin, scaled, color_begin, end);
    return true;
}

bool jce_video_frame_prepare_display(JceVideoFrameProcessor *p,
    const JceYuv420Frame *input, uint32_t limit, uint8_t **rgba, size_t *capacity,
    int *width, int *height)
{
    if (!p || !rgba || !capacity || !width || !height || p->rgba) return false;
    p->perf_context = "owner";
    jce_video_frame_swap_buffer(&p->rgba, &p->capacity, rgba, capacity);
    const bool ok = jce_video_frame_prepare(p, input, limit);
    jce_video_frame_swap_buffer(&p->rgba, &p->capacity, rgba, capacity);
    if (ok) { *width = p->width; *height = p->height; }
    return ok;
}

void jce_video_frame_destroy(JceVideoFrameProcessor *processor)
{
    if (!processor) return;
    JCE_FREE(processor->rgba);
    JCE_FREE(processor->preview.buffer);
    memset(processor, 0, sizeof(*processor));
}
