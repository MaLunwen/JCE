/* Interpret backend observations without inventing an engine-frame join. */
#pragma once
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_renderer.h>
#include <cmath>
#include <limits>
struct JceProfilerObservation {
    double cpu_interval_ms=0, gpu_ms=0, api_wait_ms=0, submit_wait_ms=0;
    bool cpu_available=false, gpu_available=false;
    bool api_wait_available=false, submit_wait_available=false;
};
enum class JceProfilerObservationVerdict { Unknown, SubmissionWait, BackendWait };
inline JceProfilerObservation jce_profiler_observe(const JceFrameStats *stats,
    const JceGpuTimingObservation *gpu=nullptr)
{
    JceProfilerObservation sample;
    if (!stats) return sample;
    if (stats->cpu_timer_freq>0) {
        const double scale=1000.0/static_cast<double>(stats->cpu_timer_freq);
        sample.cpu_available=stats->cpu_time_frame>0;
        sample.api_wait_available=stats->wait_render>=0;
        sample.submit_wait_available=stats->wait_submit>=0;
        if (sample.cpu_available) sample.cpu_interval_ms=static_cast<double>(stats->cpu_time_frame)*scale;
        if (sample.api_wait_available) sample.api_wait_ms=static_cast<double>(stats->wait_render)*scale;
        if (sample.submit_wait_available) sample.submit_wait_ms=static_cast<double>(stats->wait_submit)*scale;
    }
    /* Only the public observation owner admits GPU timing. Legacy stats
     * alone do not establish a live context or a nonforward timer sample. */
    if (gpu && gpu->version==1 && gpu->struct_size>=sizeof(*gpu) &&
        gpu->context_available && gpu->backend_available && gpu->timer_available &&
        gpu->unavailable_reason==JCE_GPU_TIMING_AVAILABLE &&
        std::isfinite(gpu->duration_ms) && gpu->duration_ms>0) {
        sample.gpu_available=true;
        sample.gpu_ms=gpu->duration_ms;
    }
    return sample;
}
inline JceProfilerObservationVerdict jce_profiler_observation_verdict(const JceProfilerObservation &sample)
{
    /* A render thread waiting for the next submission is not a GPU-bound
     * verdict. GPU timings are latched and have no engine-frame identity. */
    if (sample.cpu_available && sample.submit_wait_available &&
        sample.submit_wait_ms>0.1 && sample.submit_wait_ms>=sample.cpu_interval_ms*.5)
        return JceProfilerObservationVerdict::SubmissionWait;
    if (sample.api_wait_available && sample.api_wait_ms>0.1)
        return JceProfilerObservationVerdict::BackendWait;
    return JceProfilerObservationVerdict::Unknown;
}

/* Preserve known zero values; an unknown slot must remain a graph gap. */
inline float jce_profiler_history_value(float value, bool available)
{
    return available ? value : std::numeric_limits<float>::quiet_NaN();
}
inline bool jce_profiler_history_known(float value)
{
    return std::isfinite(value) && value>=0;
}
struct JceProfilerHistoryEpoch {
    bool initialized=false, context_available=false;
    uint64_t generation=0;
};
inline bool jce_profiler_history_epoch_changed(JceProfilerHistoryEpoch *epoch,
                                               const JceGpuTimingObservation *gpu)
{
    const bool context=gpu && gpu->version==1 && gpu->struct_size>=sizeof(*gpu) &&
        gpu->context_available && gpu->context_generation!=0;
    const uint64_t generation=context?gpu->context_generation:0;
    const bool changed=epoch->initialized &&
        (epoch->context_available!=context || epoch->generation!=generation);
    epoch->initialized=true;
    epoch->context_available=context;
    epoch->generation=generation;
    return changed;
}
