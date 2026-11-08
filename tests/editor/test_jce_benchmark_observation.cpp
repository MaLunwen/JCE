/* Complete Benchmark observer, actual panel sampling wire, and real lifecycle
 * registry. Typed clock/renderer ports are CPU controls, never GPU evidence. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/application/jce_lifecycle.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <limits>

static uint64_t clock_tick, clock_frequency;
static int ui_frame;
static JceGpuTimingObservation supplied;
static bool getter_ok, registration_fails;
static unsigned registrations, removals, getter_calls, pushed;
static double pushed_ms;
static JceLifecycleHandle JCE_CALL benchmark_register_port(JceLifecycleCallback,int32_t,void *);
static void JCE_CALL benchmark_unregister_port(JceLifecycleHandle);
static uint64_t JCE_CALL benchmark_tick_port(void);
static uint64_t JCE_CALL benchmark_frequency_port(void);
static bool benchmark_gpu_port(JceGpuTimingObservation *,size_t);
#define jce_lifecycle_register benchmark_register_port
#define jce_lifecycle_unregister benchmark_unregister_port
#define jce_time_perf_counter benchmark_tick_port
#define jce_time_perf_freq benchmark_frequency_port
#define jce_renderer_get_gpu_timing_observation benchmark_gpu_port
#ifndef JCE_BENCHMARK_OBSERVATION_HEADER
#define JCE_BENCHMARK_OBSERVATION_HEADER "panels/jce_benchmark_observation.h"
#endif
#include JCE_BENCHMARK_OBSERVATION_HEADER
#undef jce_lifecycle_register
#undef jce_lifecycle_unregister
#undef jce_time_perf_counter
#undef jce_time_perf_freq
#undef jce_renderer_get_gpu_timing_observation

static JceLifecycleHandle JCE_CALL benchmark_register_port(JceLifecycleCallback cb,int32_t priority,void *user)
{
    ++registrations;
    return registration_fails?JceLifecycleHandle{}:jce_lifecycle_register(cb,priority,user);
}
static void JCE_CALL benchmark_unregister_port(JceLifecycleHandle handle)
{
    ++removals; jce_lifecycle_unregister(handle);
}
static uint64_t JCE_CALL benchmark_tick_port(void) { return clock_tick; }
static uint64_t JCE_CALL benchmark_frequency_port(void) { return clock_frequency; }
static bool benchmark_gpu_port(JceGpuTimingObservation *out,size_t size)
{
    ++getter_calls;
    if (!getter_ok || !out || size<sizeof(*out)) return false;
    std::memcpy(out,&supplied,sizeof(supplied)); return true;
}
namespace ImGui { static int GetFrameCount() { return ui_frame; } }
static void ft_push(float ms) { ++pushed; pushed_ms=ms; }
static void actual_panel_poll(JceBenchmarkCadence &state,bool blocked,
                              const JceBenchmarkProfile &profile)
{
    struct { JceBenchmarkCadence &cadence; } s_observation{state};
    JceGpuTimingObservation gpu{};
#include JCE_BENCHMARK_WIRE_HEADER
}
static JceBenchmarkProfile profile()
{
    JceBenchmarkProfile p; p.backend=1; p.tier=2; p.target_fps=60; p.vsync=true;
    return p;
}
static void reset_ports()
{
    jce_lifecycle_shutdown(); registrations=removals=getter_calls=pushed=0;
    pushed_ms=0; clock_tick=0; clock_frequency=1000000; ui_frame=10;
    getter_ok=true; registration_fails=false; supplied={};
    supplied.struct_size=sizeof(supplied); supplied.version=1;
    supplied.context_available=true; supplied.context_generation=7;
    supplied.read_host_frame_index=50; supplied.gpu_source_frame=5;
    /* A CPU cadence does not depend on this absent GPU frequency. */
    supplied.unavailable_reason=JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE;
}
static void next(uint64_t ticks=16000)
{
    clock_tick+=ticks; ++ui_frame; ++supplied.read_host_frame_index;
}
static JceBenchmarkCadenceSample poll(JceBenchmarkCadence &state,bool blocked=false)
{
    JceGpuTimingObservation gpu{};
    return jce_benchmark_poll(&state,ui_frame,blocked,profile(),&gpu);
}
TEST_CASE("actual Benchmark wire feeds wall cadence with unavailable GPU timing")
{
    reset_ports();
    {
        JceBenchmarkCadence state;
        actual_panel_poll(state,false,profile()); CHECK(pushed==0);
        next(1000000); actual_panel_poll(state,false,profile());
        CHECK(getter_calls==2); CHECK(pushed==1); CHECK(pushed_ms==doctest::Approx(1000));
        CHECK(state.complete); CHECK(state.accepted==1); CHECK(registrations==1);
    }
    CHECK(removals==1);
}
TEST_CASE("unique consecutive host and UI observations retain long frames and host wrap")
{
    reset_ports(); JceBenchmarkCadence state;
    supplied.read_host_frame_index=UINT32_MAX; poll(state);
    next(2000000); auto sample=poll(state);
    CHECK(sample.accepted); CHECK(sample.ms==doctest::Approx(2000));
    CHECK(supplied.read_host_frame_index==0); CHECK(state.complete);
    clock_tick+=1000; CHECK_FALSE(poll(state).accepted); CHECK(state.accepted==1);
    next(15000); sample=poll(state);
    CHECK(sample.accepted); CHECK(sample.ms==doctest::Approx(16));
    CHECK(supplied.gpu_source_frame==5); CHECK(state.accepted==2);
}
TEST_CASE("hidden UI and missing host observations are explicit incomplete coverage")
{
    reset_ports(); JceBenchmarkCadence state; poll(state);
    clock_tick=64000; ui_frame+=4; supplied.read_host_frame_index+=4;
    CHECK_FALSE(poll(state).accepted); CHECK(state.missed_ui==3); CHECK(state.missed_host==3);
    CHECK(state.interruptions==1); CHECK_FALSE(state.complete);
    next(); CHECK(poll(state).accepted); CHECK_FALSE(state.complete);
    jce_benchmark_reset(&state); CHECK(state.complete); CHECK(state.missed_ui==0);
    CHECK_FALSE(poll(state).accepted); next(); CHECK(poll(state).accepted);
}
TEST_CASE("clock changes context replacement and profile changes invalidate coverage")
{
    reset_ports(); JceBenchmarkCadence state; poll(state);
    next(); clock_frequency=2000000; CHECK_FALSE(poll(state).accepted);
    CHECK_FALSE(state.complete); CHECK(state.interruptions==1);
    next(); ++supplied.context_generation; CHECK_FALSE(poll(state).accepted);
    CHECK(state.interruptions==2);
    next(); auto changed=profile(); changed.target_fps=30; JceGpuTimingObservation gpu{};
    CHECK_FALSE(jce_benchmark_poll(&state,ui_frame,false,changed,&gpu).accepted);
    CHECK(state.interruptions==3);
    next(); clock_tick=1; CHECK_FALSE(jce_benchmark_poll(&state,ui_frame,false,changed,&gpu).accepted);
    CHECK(state.interruptions==4);
}
TEST_CASE("real lifecycle listener captures unseen focus pause and quit with paired cleanup")
{
    reset_ports();
    {
        JceBenchmarkCadence state; poll(state); poll(state); CHECK(registrations==1);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
        jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
        CHECK_FALSE(state.complete); CHECK_FALSE(state.anchored); CHECK(state.interruptions==4);
        jce_benchmark_reset(&state); poll(state); next(); CHECK(poll(state).accepted);
        jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_LOST); CHECK_FALSE(state.complete);
        jce_lifecycle_emit(JCE_LIFECYCLE_WILL_QUIT);
        CHECK(state.listener.id==0); CHECK(state.stopped); CHECK(removals==1);
        const auto interrupts=state.interruptions;
        jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_RESET); CHECK(state.interruptions==interrupts);
        next(); CHECK_FALSE(poll(state).accepted); CHECK(registrations==1);
    }
    CHECK(removals==1);
}
TEST_CASE("failed registration leaves no handle and only explicit Reset retries")
{
    reset_ports(); registration_fails=true;
    {
        JceBenchmarkCadence state; CHECK_FALSE(poll(state).accepted);
        CHECK_FALSE(state.complete); CHECK(state.listener.id==0); CHECK(registrations==1);
        next(); poll(state); CHECK(registrations==1); CHECK(removals==0);
        registration_fails=false; jce_benchmark_reset(&state); poll(state);
        CHECK(registrations==2); CHECK(state.listener.id!=0);
        next(); CHECK(poll(state).accepted); CHECK(state.complete);
    }
    CHECK(removals==1);
}
TEST_CASE("blocked missing context and zero clock are unavailable rather than zero cost")
{
    reset_ports(); JceBenchmarkCadence state; poll(state);
    next(); CHECK_FALSE(poll(state,true).accepted); CHECK_FALSE(state.complete);
    next(); supplied.context_available=false; CHECK_FALSE(poll(state).accepted);
    next(); supplied.context_available=true; clock_frequency=0; CHECK_FALSE(poll(state).accepted);
    CHECK(state.accepted==0); CHECK(state.interruptions==3);
    next(); clock_frequency=1000000; getter_ok=false; CHECK_FALSE(poll(state).accepted);
    CHECK(state.interruptions==4);
}
TEST_CASE("baseline admits only new metric schema and exact bounded profile")
{
    reset_ports(); JceBenchmarkCadence state; poll(state); next(); poll(state);
    char text[256]{}; const auto p=profile();
    REQUIRE(jce_benchmark_format_baseline(text,sizeof(text),state,0,1000,p,60,30,33.3));
    const auto value=jce_benchmark_parse_baseline(text);
    CHECK(jce_benchmark_baseline_matches(value,0,1000,p)); CHECK(value.avg_fps==60);
    CHECK_FALSE(jce_benchmark_parse_baseline("0 1000 60 30 33.3").found);
    CHECK_FALSE(jce_benchmark_parse_baseline("jce-max-cpu-gpu-v1 0 1000 1 2 60 0 1 0 60 30 33.3").found);
    CHECK_FALSE(jce_benchmark_parse_baseline((std::string(text)+"garbage").c_str()).found);
    for (unsigned field=0;field<6;++field) {
        auto changed=p;
        switch(field) { case 0:++changed.backend;break; case 1:++changed.tier;break;
        case 2:++changed.target_fps;break; case 3:++changed.play;break;
        case 4:changed.vsync=!changed.vsync;break; default:changed.workload_isolated=!changed.workload_isolated;break; }
        CHECK_FALSE(jce_benchmark_baseline_matches(value,0,1000,changed));
    }
}
TEST_CASE("baseline refuses incomplete coverage invalid metrics and truncation")
{
    reset_ports(); JceBenchmarkCadence state; char text[256]{}; auto p=profile();
    CHECK_FALSE(jce_benchmark_format_baseline(text,sizeof(text),state,0,1000,p,60,30,33));
    poll(state); next(); poll(state);
    CHECK_FALSE(jce_benchmark_format_baseline(text,8,state,0,1000,p,60,30,33));
    CHECK_FALSE(jce_benchmark_format_baseline(text,sizeof(text),state,0,1000,p,
        std::numeric_limits<double>::quiet_NaN(),30,33));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    CHECK_FALSE(jce_benchmark_format_baseline(text,sizeof(text),state,0,1000,p,60,30,33));
}
