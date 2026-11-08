/* Actual panel observation owner; typed public observations are controlled
 * evidence, not hardware timings or an exact engine-frame join. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_PROFILER_OBSERVATION_HEADER
#define JCE_PROFILER_OBSERVATION_HEADER "panels/jce_profiler_observation.h"
#endif
#include JCE_PROFILER_OBSERVATION_HEADER
#include <limits>
#include <utility>
#include <initializer_list>
static JceGpuTimingObservation timing()
{
    JceGpuTimingObservation gpu{};gpu.struct_size=sizeof(gpu);gpu.version=1;
    gpu.context_available=gpu.backend_available=gpu.timer_available=true;
    gpu.unavailable_reason=JCE_GPU_TIMING_AVAILABLE;gpu.duration_ms=1.004;
    return gpu;
}
TEST_CASE("render submit waiting is not evidence of a GPU bottleneck")
{
    JceFrameStats stats{};auto gpu=timing();
    stats.cpu_timer_freq=1000000;stats.cpu_time_frame=16630;
    stats.wait_submit=16100;stats.wait_render=3;
    const auto sample=jce_profiler_observe(&stats,&gpu);
    CHECK(sample.cpu_available);CHECK(sample.gpu_available);
    CHECK(sample.cpu_interval_ms==doctest::Approx(16.630));
    CHECK(sample.api_wait_ms==doctest::Approx(.003));
    CHECK(sample.submit_wait_ms==doctest::Approx(16.100));
    CHECK(sample.gpu_ms==doctest::Approx(1.004));
    CHECK(jce_profiler_observation_verdict(sample)==JceProfilerObservationVerdict::SubmissionWait);
}
TEST_CASE("unjoined GPU and CPU values never produce a bottleneck verdict")
{
    JceFrameStats stats{};stats.cpu_timer_freq=1000000;
    for(const auto pair:{std::pair<int64_t,int64_t>{1000,50000},{50000,1000},{20000,20000}}) {
        auto gpu=timing();gpu.duration_ms=pair.second/1000.0;stats.cpu_time_frame=pair.first;
        const auto sample=jce_profiler_observe(&stats,&gpu);
        CHECK(sample.cpu_available);CHECK(sample.gpu_available);
        CHECK(jce_profiler_observation_verdict(sample)==JceProfilerObservationVerdict::Unknown);
    }
}
TEST_CASE("missing timers are unavailable rather than zero work")
{
    const auto empty=jce_profiler_observe(nullptr);CHECK_FALSE(empty.cpu_available);CHECK_FALSE(empty.gpu_available);
    JceFrameStats stats{};stats.cpu_time_frame=16630;stats.wait_submit=16100;
    auto gpu=timing();gpu.timer_available=false;gpu.unavailable_reason=JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE;
    for(int64_t freq:{0,-1}) {
        stats.cpu_timer_freq=freq;
        const auto sample=jce_profiler_observe(&stats,&gpu);
        CHECK_FALSE(sample.cpu_available);CHECK_FALSE(sample.gpu_available);
        CHECK_FALSE(sample.api_wait_available);CHECK_FALSE(sample.submit_wait_available);
        CHECK(jce_profiler_observation_verdict(sample)==JceProfilerObservationVerdict::Unknown);
    }
}
TEST_CASE("nonforward GPU observation and invalid CPU interval stay unavailable")
{
    JceFrameStats stats{};stats.cpu_timer_freq=1000000;stats.cpu_time_frame=-1;
    stats.wait_render=stats.wait_submit=-1;auto gpu=timing();
    gpu.timer_available=false;gpu.unavailable_reason=JCE_GPU_TIMING_INTERVAL_UNAVAILABLE;
    const auto sample=jce_profiler_observe(&stats,&gpu);
    CHECK_FALSE(sample.cpu_available);CHECK_FALSE(sample.gpu_available);
    CHECK_FALSE(sample.api_wait_available);CHECK_FALSE(sample.submit_wait_available);
    CHECK(jce_profiler_observation_verdict(sample)==JceProfilerObservationVerdict::Unknown);
}
TEST_CASE("legacy GPU numbers cannot bypass the public observation owner")
{
    JceFrameStats stats{};stats.gpu_timer_freq=1000000;stats.gpu_time_begin=1;stats.gpu_time_end=1005;
    const auto sample=jce_profiler_observe(&stats);
    CHECK_FALSE(sample.gpu_available);CHECK(sample.gpu_ms==0);
}
TEST_CASE("missing context and malformed public observation refuse GPU admission")
{
    JceFrameStats stats{};stats.cpu_timer_freq=1000000;
    auto gpu=timing();gpu.context_available=false;CHECK_FALSE(jce_profiler_observe(&stats,&gpu).gpu_available);
    gpu=timing();gpu.struct_size=0;CHECK_FALSE(jce_profiler_observe(&stats,&gpu).gpu_available);
    gpu=timing();gpu.version=0;CHECK_FALSE(jce_profiler_observe(&stats,&gpu).gpu_available);
    gpu=timing();gpu.duration_ms=std::numeric_limits<double>::quiet_NaN();CHECK_FALSE(jce_profiler_observe(&stats,&gpu).gpu_available);
}
TEST_CASE("wait larger than the CPU interval cannot be subtracted from CPU work")
{
    JceFrameStats stats{};stats.cpu_timer_freq=1000000;stats.cpu_time_frame=16000;
    stats.wait_render=20000;stats.wait_submit=25000;
    const auto sample=jce_profiler_observe(&stats);
    CHECK(sample.cpu_interval_ms==16);CHECK(sample.api_wait_ms==20);CHECK(sample.submit_wait_ms==25);
    CHECK(sample.cpu_available);CHECK(jce_profiler_observation_verdict(sample)==JceProfilerObservationVerdict::SubmissionWait);
}


#ifdef JCE_PROFILER_CHART_OWNER
/* Exact current state/sample/ring/chart and real series-call bodies, bound to
 * counted drawing/public observation ports. No renderer or ImGui is created. */
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace {
using ImU32=uint32_t;
struct ImVec2 {
    float x,y;
    ImVec2(float a=0,float b=0):x(a),y(b) {}
};
#define IM_COL32(R,G,B,A) (static_cast<ImU32>(R) | (static_cast<ImU32>(G)<<8) | (static_cast<ImU32>(B)<<16) | (static_cast<ImU32>(A)<<24))
constexpr ImU32 JCE_COL32_STATUS_OK=IM_COL32(20,160,60,255);
constexpr ImU32 JCE_COL32_STATUS_WARN=IM_COL32(220,160,60,255);
constexpr int ImGuiTableFlags_SizingStretchSame=1;
struct ChartPorts {
    struct Line { ImVec2 a,b; ImU32 color; };
    struct Dot { ImVec2 point; ImU32 color; };
    std::vector<Line> lines;
    std::vector<Dot> dots;
    std::vector<std::string> text, captions, tooltips;
    int quads=0, public_stats=0, public_gpu=0, refill=0;
    int view_formats=0, view_tables=0;
    int phase_capture=0, reductions=0, presentations=0;
    bool game_maximized=false, detached=false;
    bool finite=true;
    void accept(ImVec2 point) { finite=finite&&std::isfinite(point.x)&&std::isfinite(point.y); }
};
ChartPorts chart_ports;
struct ImGuiIO { float DeltaTime=0.016f; };
struct ImGuiViewport {};
ImGuiIO chart_io;
ImGuiViewport main_viewport, detached_viewport;
double chart_time=0;
int chart_frame=7;
struct ImDrawList {
    void AddRectFilled(ImVec2 a,ImVec2 b,ImU32,float=0) { chart_ports.accept(a);chart_ports.accept(b); }
    void AddRect(ImVec2 a,ImVec2 b,ImU32,float=0) { chart_ports.accept(a);chart_ports.accept(b); }
    void AddConvexPolyFilled(const ImVec2 *points,int n,ImU32) {
        ++chart_ports.quads;for(int i=0;i<n;++i)chart_ports.accept(points[i]);
    }
    void AddLine(ImVec2 a,ImVec2 b,ImU32 color,float=1) {
        chart_ports.lines.push_back({a,b,color});chart_ports.accept(a);chart_ports.accept(b);
    }
    void AddCircleFilled(ImVec2 p,float,ImU32 color) {
        chart_ports.dots.push_back({p,color});chart_ports.accept(p);
    }
    void AddText(ImVec2 p,ImU32,const char *value) {
        chart_ports.accept(p);chart_ports.text.emplace_back(value);
    }
};
ImDrawList draw_list;
std::string format_port(const char *format,va_list args) {
    char value[384];std::vsnprintf(value,sizeof(value),format,args);return value;
}
namespace ImGui {
ImDrawList *GetWindowDrawList() { return &draw_list; }
ImVec2 GetCursorScreenPos() { return {0,0}; }
ImVec2 GetContentRegionAvail() { return {320,400}; }
float GetTextLineHeight() { return 14; }
ImVec2 CalcTextSize(const char *text) { return {static_cast<float>(std::strlen(text))*7,14}; }
void Dummy(ImVec2) {}
bool IsItemHovered() { return true; }
int GetFrameCount() { return chart_frame; }
ImGuiIO &GetIO() { return chart_io; }
double GetTime() { return chart_time; }
ImGuiViewport *GetMainViewport() { return &main_viewport; }
ImGuiViewport *GetWindowViewport() { return chart_ports.detached?&detached_viewport:&main_viewport; }
void TextDisabled(const char *format,...) {
    va_list args;va_start(args,format);chart_ports.captions.push_back(format_port(format,args));va_end(args);
}
void SetTooltip(const char *format,...) {
    va_list args;va_start(args,format);chart_ports.tooltips.push_back(format_port(format,args));va_end(args);
}
bool BeginTable(const char *,int,int) { return true; }
void TableNextRow() {}
void TableNextColumn() {}
void EndTable() {}
}
namespace jce_theme {
ImU32 canvas_bg() { return 1; }
ImU32 grid_minor() { return 2; }
ImU32 text_secondary() { return 3; }
}
const char *jce_editor_i18n(const char *key) {
    if(!std::strcmp(key,"profiler.value.unavailable"))return "Unavailable";
    if(!std::strcmp(key,"profiler.row.samples"))return "Samples";
    return key;
}
JceFrameStats chart_stats{};
JceViewStats chart_views[2]{};
JceGpuTimingObservation chart_gpu{};
bool chart_stats_available=true,chart_gpu_copied=true;
const JceFrameStats *chart_capture() {
    ++chart_ports.public_stats;return chart_stats_available?&chart_stats:nullptr;
}
bool chart_gpu_capture(JceGpuTimingObservation *out,size_t size) {
    ++chart_ports.public_gpu;
    if(!out||size<sizeof(*out)||!chart_gpu_copied)return false;
    *out=chart_gpu;return true;
}
int test_counted_view_snprintf(char *dst,size_t cap,const char *format,...) {
    ++chart_ports.view_formats;
    va_list args;va_start(args,format);
    const int result=std::vsnprintf(dst,cap,format,args);va_end(args);return result;
}
/* Only later table widgets are a port. The exported lazy refresh and complete
 * derived-row conversion/copy are production code imported below. */
void draw_view_table() { ++chart_ports.view_tables; }
void jce_perf_phase_set_enabled(int enabled) {
    CHECK(enabled==1);++chart_ports.phase_capture;
}
bool jce_editor_game_view_maximized() { return chart_ports.game_maximized; }
void test_counted_compute_stats(const float *ring,float &mn,float &mx,float &avg,const bool *available=nullptr);
#define jce_gfx_stats_capture chart_capture
#define jce_renderer_get_gpu_timing_observation chart_gpu_capture
#include JCE_PROFILER_CHART_OWNER
#undef jce_gfx_stats_capture
#undef jce_renderer_get_gpu_timing_observation
#undef IM_COL32
void test_counted_compute_stats(const float *ring,float &mn,float &mx,float &avg,const bool *available) {
    ++chart_ports.reductions;compute_stats(ring,mn,mx,avg,available);
}
void reset_chart() {
    s_prof=ProfilerState{};chart_ports=ChartPorts{};
    chart_io.DeltaTime=0.016f;chart_time+=10;chart_frame+=1;
    chart_stats={};chart_stats.cpu_timer_freq=1000000;chart_stats.cpu_time_frame=16000;
    chart_stats.wait_render=0;chart_stats.wait_submit=15000;
    chart_gpu=timing();chart_gpu.context_generation=1;chart_gpu.timer_frequency=1000000;
    chart_gpu.raw_begin=10;chart_gpu.raw_end=1014;
    chart_stats_available=chart_gpu_copied=true;
}
void set_chart_views() {
    for(int i=0;i<2;++i) {
        chart_views[i]={};chart_views[i].view_id=static_cast<uint16_t>(120+i);
        std::snprintf(chart_views[i].name,sizeof(chart_views[i].name),"Game %d",i);
        chart_views[i].cpu_time_begin=1;chart_views[i].cpu_time_end=1001;
        chart_views[i].gpu_time_begin=100;chart_views[i].gpu_time_end=2100;
    }
    chart_stats.view_stats=chart_views;chart_stats.view_stats_count=2;
}
size_t colored_lines(ImU32 color) {
    return static_cast<size_t>(std::count_if(chart_ports.lines.begin(),chart_ports.lines.end(),
        [=](const ChartPorts::Line &line){return line.color==color;}));
}
size_t colored_dots(ImU32 color) {
    return static_cast<size_t>(std::count_if(chart_ports.dots.begin(),chart_ports.dots.end(),
        [=](const ChartPorts::Dot &dot){return dot.color==color;}));
}
constexpr ImU32 chart_gpu_color=80u|(160u<<8)|(230u<<16)|(255u<<24);
void mixed_history() {
    s_prof.head=s_prof.filled=4;
    for(int i=0;i<4;++i) {
        s_prof.gpu_ms[i]=s_prof.cpu_ms[i]=s_prof.wait_ms[i]=static_cast<float>(i);
        s_prof.cpu_available[i]=s_prof.gpu_available[i]=s_prof.api_wait_available[i]=(i!=1);
    }
    s_prof.cpu_ms[0]=s_prof.gpu_ms[0]=s_prof.wait_ms[0]=0;
}
}
TEST_CASE("covered main-viewport CPU presentation keeps every frame sample") {
    reset_chart();set_chart_views();chart_ports.game_maximized=true;
    draw_content();
    CHECK(s_prof.filled==1);CHECK(s_prof.head==1);
    CHECK(s_prof.frame_ms[0]==doctest::Approx(16));
    CHECK(s_prof.uptime_s==doctest::Approx(0.016));
    CHECK(chart_ports.phase_capture==1);CHECK(chart_ports.public_stats==1);
    CHECK(chart_ports.public_gpu==1);CHECK(chart_ports.refill==0);
    CHECK(chart_ports.view_formats==0);CHECK(s_prof.view_rows.empty());
    CHECK(chart_ports.reductions==0);CHECK(chart_ports.presentations==0);
    chart_time+=0.016;chart_frame+=1;draw_content();
    CHECK(s_prof.filled==2);CHECK(s_prof.head==2);
    CHECK(chart_ports.public_stats==2);CHECK(chart_ports.public_gpu==2);
    CHECK(chart_ports.phase_capture==2);CHECK(chart_ports.refill==0);
    CHECK(chart_ports.view_formats==0);CHECK(s_prof.view_rows_frame==-1);
    CHECK(chart_ports.reductions==0);CHECK(chart_ports.presentations==0);
}
TEST_CASE("ordinary and detached CPU profiler presentation retains its reductions") {
    for(bool detached:{false,true}) {
        reset_chart();set_chart_views();chart_ports.game_maximized=detached;chart_ports.detached=detached;
        draw_content();
        CHECK(s_prof.filled==1);CHECK(chart_ports.phase_capture==1);
        CHECK(chart_ports.public_stats==1);CHECK(chart_ports.public_gpu==1);
        CHECK(chart_ports.reductions==6);CHECK(chart_ports.presentations==1);
        CHECK(chart_ports.refill==1);CHECK(chart_ports.view_formats==2);
        REQUIRE(s_prof.view_rows.size()==2);CHECK(s_prof.view_rows_frame==chart_frame);
        CHECK(s_prof.view_rows[0].id==120);CHECK(s_prof.view_rows[0].gpu_available);
        CHECK(s_prof.view_rows[0].gpu_ms==doctest::Approx(2));
    }
}
TEST_CASE("covered CPU history still resets stale UI epoch and invalidates backend epoch") {
    reset_chart();chart_ports.game_maximized=true;
    draw_content();chart_time+=0.016;chart_frame+=1;draw_content();
    CHECK(s_prof.filled==2);
    chart_gpu.context_generation=2;chart_time+=0.016;chart_frame+=1;draw_content();
    CHECK(s_prof.filled==3);CHECK_FALSE(s_prof.gpu_available[0]);
    CHECK_FALSE(s_prof.cpu_available[1]);CHECK(s_prof.gpu_available[2]);
    chart_io.DeltaTime=0.020f;chart_time+=2.1;chart_frame+=1;draw_content();
    CHECK(s_prof.filled==1);CHECK(s_prof.head==1);
    CHECK(s_prof.frame_ms[0]==doctest::Approx(20));
    CHECK(s_prof.uptime_s==doctest::Approx(0.020));
    CHECK(chart_ports.reductions==0);CHECK(chart_ports.presentations==0);
    chart_ports.game_maximized=false;chart_time+=0.020;chart_frame+=1;draw_content();
    CHECK(s_prof.filled==2);CHECK(s_prof.head==2);
    CHECK(chart_ports.reductions==6);CHECK(chart_ports.presentations==1);
}
TEST_CASE("actual debugger consumer refreshes rows once per demanded UI frame") {
    reset_chart();set_chart_views();
    jce_panel_profiler_draw_view_table();
    CHECK(s_prof.filled==0);CHECK(chart_ports.phase_capture==0);
    CHECK(chart_ports.public_stats==1);CHECK(chart_ports.public_gpu==1);
    CHECK(chart_ports.refill==1);CHECK(chart_ports.view_formats==2);
    CHECK(chart_ports.view_tables==1);REQUIRE(s_prof.view_rows.size()==2);
    CHECK(s_prof.view_rows[0].cpu_ms==doctest::Approx(1));
    jce_panel_profiler_draw_view_table();
    CHECK(chart_ports.public_stats==1);CHECK(chart_ports.public_gpu==1);
    CHECK(chart_ports.refill==1);CHECK(chart_ports.view_tables==2);
    ++chart_frame;chart_gpu.context_generation=2;chart_gpu.timer_frequency=2000000;
    chart_views[0].view_id=160;std::strcpy(chart_views[0].name,"Recreated");
    jce_panel_profiler_draw_view_table();
    CHECK(chart_ports.public_stats==2);CHECK(chart_ports.public_gpu==2);
    CHECK(chart_ports.refill==2);CHECK(chart_ports.view_formats==4);
    CHECK(s_prof.view_rows[0].id==160);CHECK(std::string(s_prof.view_rows[0].name)=="Recreated");
    CHECK(s_prof.view_rows[0].gpu_ms==doctest::Approx(1));
    ++chart_frame;chart_gpu.context_available=false;chart_gpu.timer_available=false;
    jce_panel_profiler_draw_view_table();
    CHECK(chart_ports.refill==3);CHECK_FALSE(s_prof.view_rows[0].gpu_available);
}
TEST_CASE("covered samples defer derived rows until the same owner is revealed") {
    reset_chart();set_chart_views();draw_content();
    REQUIRE(s_prof.view_rows.size()==2);CHECK(chart_ports.refill==1);
    chart_ports.game_maximized=true;++chart_frame;chart_time+=0.016;
    chart_views[0].view_id=180;std::strcpy(chart_views[0].name,"Latest");draw_content();
    CHECK(s_prof.filled==2);CHECK(chart_ports.refill==1);CHECK(s_prof.view_rows[0].id==120);
    ++chart_frame;chart_time+=0.016;chart_gpu.context_generation=2;draw_content();
    CHECK(s_prof.filled==3);CHECK_FALSE(s_prof.gpu_available[0]);
    CHECK(chart_ports.refill==1);CHECK(chart_ports.view_formats==2);
    chart_ports.game_maximized=false;++chart_frame;chart_time+=0.016;draw_content();
    CHECK(s_prof.filled==4);CHECK(chart_ports.refill==2);CHECK(chart_ports.view_formats==4);
    CHECK(chart_ports.public_stats==4);CHECK(chart_ports.public_gpu==4);
    CHECK(s_prof.view_rows[0].id==180);CHECK(std::string(s_prof.view_rows[0].name)=="Latest");
}
TEST_CASE("missing stats still clear old derived rows while CPU history is covered") {
    reset_chart();set_chart_views();draw_content();REQUIRE(s_prof.view_rows.size()==2);
    chart_ports.game_maximized=true;chart_stats_available=false;++chart_frame;chart_time+=0.016;
    draw_content();CHECK(s_prof.filled==2);CHECK(s_prof.view_rows.empty());
    CHECK(s_prof.view_rows_frame==chart_frame);CHECK(chart_ports.refill==1);
    CHECK(chart_ports.view_formats==2);CHECK(chart_ports.public_stats==2);
    jce_panel_profiler_draw_view_table();
    CHECK(chart_ports.public_stats==2);CHECK(s_prof.view_rows.empty());
    ++chart_frame;chart_stats_available=true;jce_panel_profiler_draw_view_table();
    CHECK(chart_ports.refill==2);CHECK(s_prof.view_rows.size()==2);
    CHECK(s_prof.filled==2);CHECK(chart_ports.phase_capture==2);
}
TEST_CASE("actual series chart does not bridge unavailable backend history") {
    reset_chart();mixed_history();test_actual_profiler_series(1.0f,3.0f,1.0f,3.0f,1.0f,3.0f,0,0);
    CHECK(colored_lines(chart_gpu_color)==1);
    CHECK(colored_lines(JCE_COL32_STATUS_OK)==1);
    CHECK(colored_lines(JCE_COL32_STATUS_WARN)==1);
    CHECK(colored_dots(chart_gpu_color)==1);CHECK(chart_ports.finite);
    CHECK(std::count(chart_ports.captions.begin(),chart_ports.captions.end(),
                     "Samples: 3/4; Unavailable: 1")==3);
    CHECK(chart_ports.tooltips==chart_ports.captions);
}
TEST_CASE("latest unavailable does not hide prior chart coverage or invent endpoint") {
    reset_chart();s_prof.head=s_prof.filled=3;
    s_prof.gpu_ms[0]=0;s_prof.gpu_ms[1]=4;
    s_prof.gpu_available[0]=s_prof.gpu_available[1]=true;
    test_actual_profiler_series(0,0,2.0f,4.0f,0,0,0,0);
    CHECK(colored_lines(chart_gpu_color)==1);CHECK(colored_dots(chart_gpu_color)==0);
    CHECK(chart_ports.finite);
    CHECK(std::find(chart_ports.captions.begin(),chart_ports.captions.end(),
                    "Samples: 2/3; Unavailable: 1")!=chart_ports.captions.end());
    CHECK(std::count(chart_ports.text.begin(),chart_ports.text.end(),"Unavailable")>=1);
}
TEST_CASE("all unavailable backend values stay gaps with visible coverage") {
    reset_chart();s_prof.head=s_prof.filled=3;
    test_actual_profiler_series(0,0,0,0,0,0,0,0);
    CHECK(colored_lines(chart_gpu_color)==0);CHECK(colored_lines(JCE_COL32_STATUS_OK)==0);
    CHECK(colored_lines(JCE_COL32_STATUS_WARN)==0);CHECK(colored_dots(chart_gpu_color)==0);
    CHECK(chart_ports.finite);
    CHECK(std::count(chart_ports.captions.begin(),chart_ports.captions.end(),
                    "Samples: 0/3; Unavailable: 3")==3);
}
TEST_CASE("known zero stays a real chart segment and does not become unavailable") {
    reset_chart();s_prof.head=s_prof.filled=2;
    for(int i=0;i<2;++i)s_prof.gpu_available[i]=true;
    test_actual_profiler_series(0,0,0,0,0,0,0,0);
    CHECK(colored_lines(chart_gpu_color)==1);CHECK(colored_dots(chart_gpu_color)==1);
    CHECK(chart_ports.finite);
    CHECK(std::find(chart_ports.text.begin(),chart_ports.text.end(),"0.00 ms")!=chart_ports.text.end());
    CHECK(std::find(chart_ports.captions.begin(),chart_ports.captions.end(),
                    "Samples: 2/2; Unavailable: 0")!=chart_ports.captions.end());
}
TEST_CASE("actual ring chronology retains mask after its bounded wrap") {
    reset_chart();s_prof.head=1;s_prof.filled=kHistoryLen;
    for(int i=0;i<kHistoryLen;++i) {
        s_prof.gpu_ms[i]=static_cast<float>(i);
        s_prof.gpu_available[i]=true;
    }
    s_prof.gpu_available[2]=false;
    CHECK(ring_gpu(nullptr,0)==1);CHECK_FALSE(jce_profiler_history_known(ring_gpu(nullptr,1)));
    CHECK(ring_gpu(nullptr,kHistoryLen-1)==0);
}
TEST_CASE("actual sampler invalidates only backend history on context replacement") {
    reset_chart();push_sample(31);push_sample(32);
    chart_gpu.context_generation=2;push_sample(33);
    CHECK(s_prof.filled==3);CHECK(s_prof.frame_ms[0]==31);CHECK(s_prof.frame_ms[1]==32);
    for(int i=0;i<2;++i) {
        CHECK_FALSE(s_prof.cpu_available[i]);CHECK_FALSE(s_prof.gpu_available[i]);
        CHECK_FALSE(s_prof.api_wait_available[i]);CHECK_FALSE(s_prof.submit_wait_available[i]);
    }
    CHECK(s_prof.cpu_available[2]);CHECK(s_prof.gpu_available[2]);
    CHECK(s_prof.api_wait_available[2]);CHECK(s_prof.submit_wait_available[2]);
    CHECK(chart_ports.public_gpu==3);CHECK(chart_ports.public_stats==3);
    chart_gpu.gpu_source_frame=99;chart_gpu.read_host_frame_index=101;
    push_sample(34);CHECK(s_prof.gpu_available[2]);CHECK(s_prof.gpu_available[3]);
}
TEST_CASE("unavailable timer in same context is retained as a gap not an epoch reset") {
    reset_chart();push_sample(16);
    chart_gpu.timer_available=false;chart_gpu.unavailable_reason=JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE;
    push_sample(18);
    CHECK(s_prof.gpu_available[0]);CHECK_FALSE(s_prof.gpu_available[1]);
    CHECK(s_prof.cpu_available[0]);CHECK(s_prof.cpu_available[1]);
    CHECK_FALSE(jce_profiler_history_known(ring_gpu(nullptr,1)));
    float mn,mx,avg;compute_stats(s_prof.gpu_ms,mn,mx,avg,s_prof.gpu_available);
    CHECK(avg==doctest::Approx(1.004));CHECK(mn==doctest::Approx(1.004));
}
TEST_CASE("actual sampler closes prior backend epoch on missing and restored context") {
    reset_chart();push_sample(16);
    chart_gpu.context_available=false;chart_gpu.timer_available=false;
    chart_gpu.unavailable_reason=JCE_GPU_TIMING_CONTEXT_UNAVAILABLE;
    push_sample(33);CHECK_FALSE(s_prof.gpu_available[0]);CHECK_FALSE(s_prof.gpu_available[1]);
    chart_gpu=timing();chart_gpu.context_generation=1;push_sample(20);
    CHECK_FALSE(s_prof.cpu_available[1]);CHECK(s_prof.gpu_available[2]);
    CHECK(s_prof.filled==3);CHECK(s_prof.frame_ms[1]==33);
}
TEST_CASE("empty actual chart has no known point or zero cost caption") {
    reset_chart();test_actual_profiler_series(0,0,0,0,0,0,0,0);
    CHECK(colored_dots(chart_gpu_color)==0);CHECK(colored_lines(chart_gpu_color)==0);
    CHECK(chart_ports.finite);
    CHECK(std::find(chart_ports.text.begin(),chart_ports.text.end(),"0.00 ms")==chart_ports.text.end());
    CHECK(std::count(chart_ports.captions.begin(),chart_ports.captions.end(),
                    "Samples: 0/0; Unavailable: 0")==4);
}
#endif
