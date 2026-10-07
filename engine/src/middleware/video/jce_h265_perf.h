/* Private producer observations; never part of the public video ABI. */
#ifndef JCE_H265_PERF_H
#define JCE_H265_PERF_H
#include "jce_h265_decode.h"
#include "jce_video_perf.h"
#include <jce/os/core/jce_read_source.h>
typedef enum JceHevcPerfStage {
    JCE_HEVC_PERF_IO, JCE_HEVC_PERF_ANNEXB,
    JCE_HEVC_PERF_CODEC, JCE_HEVC_PERF_PUBLISH, JCE_HEVC_PERF_COUNT
} JceHevcPerfStage;
typedef struct JceHevcPerf {
    bool enabled;
    uint32_t interval_ms;
    uint64_t frequency, next_report;
    uint64_t count[JCE_HEVC_PERF_COUNT];
    double ema[JCE_HEVC_PERF_COUNT], last[JCE_HEVC_PERF_COUNT];
    uint64_t codec_empty, codec_no_output;
} JceHevcPerf;
static inline void jce_hevc_perf_record(JceHevcPerf *p, JceHevcPerfStage stage,
                                       uint64_t begin, uint64_t end)
{
    if (!p->enabled || (unsigned)stage >= JCE_HEVC_PERF_COUNT) return;
    jce_video_perf_ema_update(&p->ema[stage], &p->last[stage],
                            jce_video_perf_us(&p->frequency, begin, end));
    ++p->count[stage];
}
/* Retains the source reader's result and ownership, including disabled codecs. */
#ifdef __cplusplus
extern "C" {
#endif
size_t jce_h265_read_sample(JceH265Decoder *dec, JceReadSource *source,
                            uint64_t offset, void *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
