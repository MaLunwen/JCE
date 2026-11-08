/* Private Benchmark observation owner. Panel-to-panel wall cadence is not
 * presentation FPS, GPU completion or an exact CPU/GPU frame join. */
#pragma once
#include <jce/application/jce_lifecycle.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

struct JceBenchmarkProfile {
    int backend=0, tier=0, target_fps=0, play=0;
    bool vsync=false, workload_isolated=false;
};
inline bool jce_benchmark_same_profile(const JceBenchmarkProfile &a,
                                      const JceBenchmarkProfile &b)
{
    return a.backend==b.backend && a.tier==b.tier &&
        a.target_fps==b.target_fps && a.play==b.play &&
        a.vsync==b.vsync && a.workload_isolated==b.workload_isolated;
}
struct JceBenchmarkCadence {
    JceLifecycleHandle listener{};
    bool attempted=false, stopped=false, anchored=false, complete=true;
    uint64_t tick=0, frequency=0, context=0;
    int ui_frame=0;
    uint32_t host_frame=0;
    JceBenchmarkProfile profile;
    uint64_t interruptions=0, missed_ui=0, missed_host=0, accepted=0;
    JceBenchmarkCadence()=default;
    JceBenchmarkCadence(const JceBenchmarkCadence &)=delete;
    JceBenchmarkCadence &operator=(const JceBenchmarkCadence &)=delete;
    ~JceBenchmarkCadence();
};
inline void jce_benchmark_count(uint64_t &value, uint64_t amount=1)
{
    const uint64_t max=std::numeric_limits<uint64_t>::max();
    value=amount>max-value?max:value+amount;
}
inline void jce_benchmark_interrupt(JceBenchmarkCadence *state)
{
    state->complete=false; state->anchored=false;
    jce_benchmark_count(state->interruptions);
}
inline void jce_benchmark_end(JceBenchmarkCadence *state)
{
    const auto listener=state->listener;
    state->listener={}; state->stopped=true; state->anchored=false;
    if (listener.id) jce_lifecycle_unregister(listener);
}
inline JceBenchmarkCadence::~JceBenchmarkCadence() { jce_benchmark_end(this); }
inline void JCE_CALL jce_benchmark_lifecycle(JceLifecycleEvent event, void *user)
{
    auto *state=static_cast<JceBenchmarkCadence *>(user);
    jce_benchmark_interrupt(state);
    if (event==JCE_LIFECYCLE_WILL_QUIT) jce_benchmark_end(state);
}
inline bool jce_benchmark_start(JceBenchmarkCadence *state)
{
    if (state->stopped) return false;
    if (state->listener.id) return true;
    if (state->attempted) return false;
    state->attempted=true;
    state->listener=jce_lifecycle_register(jce_benchmark_lifecycle, 100, state);
    if (!state->listener.id) jce_benchmark_interrupt(state);
    return state->listener.id!=0;
}
inline void jce_benchmark_reset(JceBenchmarkCadence *state)
{
    state->anchored=false; state->complete=true;
    state->interruptions=state->missed_ui=state->missed_host=state->accepted=0;
    /* Only explicit Reset retries a failed registration. */
    if (!state->listener.id) { state->attempted=false; state->stopped=false; }
}
struct JceBenchmarkCadenceSample { bool accepted=false; double ms=0; };
inline JceBenchmarkCadenceSample jce_benchmark_consume(JceBenchmarkCadence *state,
    uint64_t tick, uint64_t frequency, int ui_frame, bool blocked,
    const JceBenchmarkProfile &profile, const JceGpuTimingObservation *gpu)
{
    JceBenchmarkCadenceSample result;
    if (!jce_benchmark_start(state)) return result;
    /* CPU wall cadence does not require a GPU timer or a positive GPU interval. */
    if (blocked || !jce_lifecycle_is_focused() || jce_lifecycle_is_paused() ||
        !frequency || !gpu || gpu->version!=1 || gpu->struct_size<sizeof(*gpu) ||
        !gpu->context_available || !gpu->context_generation) {
        jce_benchmark_interrupt(state); return result;
    }
    if (state->anchored) {
        const int64_t ui_delta=static_cast<int64_t>(ui_frame)-state->ui_frame;
        const uint32_t host_delta=gpu->read_host_frame_index-state->host_frame;
        if (ui_delta==0 && host_delta==0 && tick>=state->tick &&
            state->frequency==frequency && state->context==gpu->context_generation &&
            jce_benchmark_same_profile(state->profile,profile)) return result;
        const bool consecutive=ui_delta==1 && host_delta==1 && tick>state->tick &&
            state->frequency==frequency && state->context==gpu->context_generation &&
            jce_benchmark_same_profile(state->profile,profile);
        if (consecutive) {
            result.ms=static_cast<double>(tick-state->tick)*1000.0/static_cast<double>(frequency);
            result.accepted=std::isfinite(result.ms) && result.ms>0;
            if (result.accepted) jce_benchmark_count(state->accepted);
            else jce_benchmark_interrupt(state);
        } else {
            if (ui_delta>1) jce_benchmark_count(state->missed_ui,static_cast<uint64_t>(ui_delta-1));
            if (host_delta>1 && host_delta<0x80000000u)
                jce_benchmark_count(state->missed_host,host_delta-1);
            jce_benchmark_interrupt(state);
        }
    }
    /* Gaps rebuild the anchor; completeness stays false until explicit Reset.
     * A long consecutive wall interval is retained without clamping. */
    state->tick=tick; state->frequency=frequency; state->ui_frame=ui_frame;
    state->host_frame=gpu->read_host_frame_index; state->context=gpu->context_generation;
    state->profile=profile; state->anchored=true;
    return result;
}
inline JceBenchmarkCadenceSample jce_benchmark_poll(JceBenchmarkCadence *state,
    int ui_frame, bool blocked, const JceBenchmarkProfile &profile,
    JceGpuTimingObservation *gpu)
{
    const uint64_t tick = jce_time_perf_counter();
    const uint64_t frequency=jce_time_perf_freq();
    *gpu={};
    const bool observed=jce_renderer_get_gpu_timing_observation(gpu,sizeof(*gpu));
    return jce_benchmark_consume(state,tick,frequency,ui_frame,blocked,profile,observed?gpu:nullptr);
}
struct JceBenchmarkBaseline {
    int kind=0, count=0;
    JceBenchmarkProfile profile;
    double avg_fps=0, low1_fps=0, p99_ms=0;
    bool found=false;
};
inline bool jce_benchmark_metrics_valid(double avg, double low, double p99)
{
    return std::isfinite(avg) && std::isfinite(low) && std::isfinite(p99) &&
        avg>0 && low>0 && p99>0;
}
inline JceBenchmarkBaseline jce_benchmark_parse_baseline(const char *line)
{
    JceBenchmarkBaseline value;
    char schema[32]{}; int vsync=0, isolated=0, end=0;
    if (!line || std::sscanf(line,"%31s %d %d %d %d %d %d %d %d %lf %lf %lf %n",
        schema,&value.kind,&value.count,&value.profile.backend,&value.profile.tier,
        &value.profile.target_fps,&value.profile.play,&vsync,&isolated,
        &value.avg_fps,&value.low1_fps,&value.p99_ms,&end)!=12) return value;
    while (line[end]==' ' || line[end]=='\r' || line[end]=='\n' || line[end]=='\t') ++end;
    value.found=std::strcmp(schema,"jce-ui-cadence-v1")==0 && !line[end] &&
        value.kind>=0 && value.kind<5 && value.count>0 &&
        value.profile.backend>=0 && value.profile.tier>=0 && value.profile.tier<4 &&
        value.profile.target_fps>=0 && value.profile.play>=0 && value.profile.play<3 &&
        (vsync==0 || vsync==1) && (isolated==0 || isolated==1) &&
        jce_benchmark_metrics_valid(value.avg_fps,value.low1_fps,value.p99_ms);
    value.profile.vsync=vsync!=0; value.profile.workload_isolated=isolated!=0;
    return value;
}
inline bool jce_benchmark_baseline_matches(const JceBenchmarkBaseline &value,
    int kind, int count, const JceBenchmarkProfile &profile)
{
    return value.found && value.kind==kind && value.count==count &&
        jce_benchmark_same_profile(value.profile,profile);
}
inline bool jce_benchmark_format_baseline(char *out, size_t size,
    const JceBenchmarkCadence &state, int kind, int count,
    const JceBenchmarkProfile &profile, double avg, double low, double p99)
{
    if (!out || !size || !state.complete || !state.anchored || !state.accepted ||
        !jce_benchmark_same_profile(state.profile,profile) || kind<0 || kind>=5 ||
        count<=0 || !jce_benchmark_metrics_valid(avg,low,p99)) return false;
    const int n=std::snprintf(out,size,"jce-ui-cadence-v1 %d %d %d %d %d %d %d %d %.9g %.9g %.9g\n",
        kind,count,profile.backend,profile.tier,profile.target_fps,profile.play,
        profile.vsync?1:0,profile.workload_isolated?1:0,avg,low,p99);
    return n>0 && static_cast<size_t>(n)<size;
}
