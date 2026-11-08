/* Production controller + native C VM + actual public runtime time control.
 * Presentation ports are headless; fake wall time deliberately disagrees with
 * host dt. This is consumer clock evidence, not graphics or OS timing. */
#include "view.h"
#include <jce/application/jce_runtime.h>
#include <jce/application/jce_main.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/application/jce_runtime_host_policy.inc.h>
#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/script_vm/jce_script_vm_c.h>
#include <jce_script_register_linked.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The headless SDK host has no assets; match the supported empty-project
 * stub instead of forcing the engine to resolve application PAK symbols. */
const unsigned char assets_pak_data[1]={0};
const size_t assets_pak_data_size=0;
static uint64_t wall_counter=UINT64_C(1000000);
static unsigned render_calls, checks, failures, cases;
static const char *timing, *timing_start, *legacy, *fixed, *fixture;
static const char *census;
static char boot[1600];
static JceRuntime *runtime;
static JceScene *scene;
static const char *clock_env(const char *name)
{
    if (!strcmp(name,"TD_TIMING")) return timing;
    if (!strcmp(name,"TD_TIMING_START")) return timing_start;
    if (!strcmp(name,"TD_LEGACY_FRAME_MODE")) return legacy;
    if (!strcmp(name,"JCE_FRAME_DT_FIXED")) return fixed;
    if (!strcmp(name,"TD_FIXTURE")) return fixture;
    if (!strcmp(name,"TD_UPDATE_CENSUS")) return census;
    return NULL;
}
static uint64_t clock_counter(void) { return wall_counter; }
static uint64_t clock_frequency(void) { return UINT64_C(1000000); }
static int64_t clock_epoch(void) { return INT64_C(1700000000000)+(int64_t)(wall_counter/1000u); }
static void log_line(void *user,const char *text)
{
    (void)user;
    if (strstr(text,"TD_BOOT ")) snprintf(boot,sizeof boot,"%s",text);
}
static void warning_line(const char *format,...)
{
    char text[1600]; va_list args;
    va_start(args,format); vsnprintf(text,sizeof text,format,args); va_end(args);
    log_line(NULL,text);
}
/* Include the unchanged application owner. Only external input/time/logging
 * ports are controlled; normal/negative builds execute its real callbacks. */
#define getenv clock_env
#define jce_time_perf_counter clock_counter
#define jce_time_perf_freq clock_frequency
#define jce_time_now_epoch_milliseconds clock_epoch
#undef LOG_WARN
#define LOG_WARN(tag,...) warning_line(__VA_ARGS__)
#ifndef TD_CONTROLLER_SOURCE
#define TD_CONTROLLER_SOURCE "controller.c"
#endif
#include TD_CONTROLLER_SOURCE
#undef getenv
#undef jce_time_perf_counter
#undef jce_time_perf_freq
#undef jce_time_now_epoch_milliseconds
/* Headless presentation ports deliberately do not claim view acceptance. */
bool td_view_init(TdView *v,const JceScriptHost *host)
{
    memset(v,0,sizeof *v); v->host=*host; v->width=1280; v->height=720;
    app.host.log=log_line;
    return true;
}
void td_view_camera(TdView *v) { (void)v; }
void td_view_render(TdView *v,const TdGame *g,int menu,int selected,const char *notice)
{ (void)v;(void)g;(void)menu;(void)selected;(void)notice; ++render_calls; }
bool td_view_ui(TdView *v,const TdGame *g,int menu,int selected,const char *notice)
{ (void)v;(void)g;(void)menu;(void)selected;(void)notice; return true; }
int td_view_pick(TdView *v,float x,float y) { (void)v;(void)x;(void)y; return -1; }
int td_view_ui_action(TdView *v,float x,float y) { (void)v;(void)x;(void)y; return 0; }
bool td_view_ui_contains(TdView *v,float x,float y) { (void)v;(void)x;(void)y; return false; }
void td_view_pan(TdView *v,float x,float y) { (void)v;(void)x;(void)y; }
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("TD_CLOCK FAIL case=%s line=%d: %s\n",name,__LINE__,#x); return; } } while(0)
static void close_runtime(void)
{
    if (runtime) jce_runtime_destroy(runtime);
    if (scene) jce_scene_destroy(scene);
    runtime=NULL; scene=NULL; jce_console_shutdown();
}
static bool open_runtime(bool start_game)
{
    JceScriptComponent script={0}; JceRuntimeDesc desc={0};
    close_runtime(); boot[0]=0; render_calls=0;
    scene=jce_scene_create(); if (!scene) return false;
    JceEntity entity=jce_scene_create_entity(scene,"clock-controller");
    snprintf(script.script_path,sizeof script.script_path,"TdController.jcec");
    jce_scene_set_script(scene,entity,&script);
    jce_scene_set_component_enabled(scene,entity,JCE_COMP_FLAG_SCRIPT,true);
    desc.scene=scene; desc.enable_physics=false;
    runtime=jce_runtime_create(&desc);
    if (!runtime || !app.used || !boot[0]) return false;
    if (start_game) {
        command(2);
        if (!td_place(&app.rules,11,1) || !td_place(&app.rules,23,2) || !td_wave(&app.rules)) return false;
    }
    return true;
}
static void update(float dt,uint64_t wall_us)
{
    wall_counter+=wall_us;
    jce_runtime_step(runtime,dt);
}
static void runtime_pause(void)
{
    const char *name="runtime_pause"; static TdGame before;
    CHECK(open_runtime(true)); update(TD_STEP,16667); before=app.rules;
    unsigned before_calls=render_calls;
    jce_runtime_set_paused(runtime,true);
    for (int i=0;i<20;++i) update(TD_STEP,500000);
    CHECK(jce_runtime_is_paused(runtime)); CHECK(render_calls==before_calls+20);
    CHECK(memcmp(&before,&app.rules,sizeof before)==0);
    jce_runtime_set_paused(runtime,false); update(TD_STEP,16667);
    CHECK(app.rules.frames==before.frames+1); CHECK(app.rules.dropped_seconds==0);
}
static void zero_scale(void)
{
    const char *name="zero_time_scale"; static TdGame before;
    CHECK(open_runtime(true)); update(TD_STEP,16667); before=app.rules;
    jce_runtime_set_time_scale(runtime,0);
    for (int i=0;i<20;++i) update(TD_STEP,500000);
    CHECK(jce_runtime_get_time_scale(runtime)==0);
    CHECK(memcmp(&before,&app.rules,sizeof before)==0);
}
static void scaled_time(float scale,uint64_t expected,const char *name)
{
    CHECK(open_runtime(true)); jce_runtime_set_time_scale(runtime,scale);
    for (int i=0;i<120;++i) update(TD_STEP,16667);
    CHECK(app.rules.frames==expected); CHECK(fabsf(app.rules.time-(float)expected/60)<.00001f);
    CHECK(app.rules.dropped_seconds==0);
}
static void half_scale(void) { scaled_time(.5f,60,"half_time_scale"); }
static void double_scale(void) { scaled_time(2,240,"double_time_scale"); }
static void host_pause_resume(void)
{
    const char *name="host_pause_resume"; JceRuntimeHostPolicy policy={0}; float dt=TD_STEP;
    CHECK(open_runtime(true)); update(TD_STEP,16667); uint64_t frames=app.rules.frames;
    CHECK(jce_runtime_host_policy_begin(&policy,runtime,false,false,30,15));
    CHECK(!jce_runtime_host_policy_step(&policy,true,&dt));
    wall_counter+=UINT64_C(20000000); dt=20;
    CHECK(!jce_runtime_host_policy_step(&policy,false,&dt)); CHECK(dt==0);
    dt=TD_STEP; CHECK(jce_runtime_host_policy_step(&policy,false,&dt));
    update(dt,16667); jce_runtime_host_policy_end(&policy);
    CHECK(app.rules.frames==frames+1); CHECK(app.rules.dropped_seconds==0);
}
static void host_interval_not_wall(void)
{
    const char *name="host_interval_not_wall";
    CHECK(open_runtime(true)); update(TD_STEP,16667);
    update(.1f,UINT64_C(12000000));
    CHECK(app.rules.frames==7); CHECK(app.rules.dropped_seconds==0);
}
static void bounded_hitch(void)
{
    const char *name="bounded_simulation_hitch";
    CHECK(open_runtime(true)); update(7,16667);
    CHECK(app.rules.frames==15); CHECK(fabs(app.rules.dropped_seconds-6.75)<.00001);
    update(TD_STEP,16667); CHECK(app.rules.frames==16);
    CHECK(fabs(app.rules.dropped_seconds-6.75)<.00001);
}
static void presentation_rates(void)
{
    const char *name="presentation_rates"; const int rates[]={60,30,120,144};
    static TdGame expected, actual;
    for (unsigned r=0;r<sizeof rates/sizeof rates[0];++r) {
        CHECK(open_runtime(true));
        for (int i=0;i<rates[r]*10;++i) update(1.0f/rates[r],37000);
        CHECK(app.rules.frames==600); CHECK(app.rules.shots>0 && app.rules.hits>0 && app.rules.kills>0);
        CHECK(app.rules.accumulator>=0 && app.rules.accumulator<1.0/60+.000001);
        actual=app.rules; actual.accumulator=0; /* Only substep float-rounding remainder differs. */
        if (!r) expected=actual;
        else CHECK(memcmp(&actual,&expected,sizeof actual)==0);
    }
}
static void timing_is_observer(void)
{
    const char *name="timing_is_observer"; timing="1";
    CHECK(open_runtime(false));
    CHECK(app.menu==1 && app.rules.spawned==0 && app.rules.frames==0);
    CHECK(strstr(boot,"clock=runtime_sim_dt") && strstr(boot,"timing_observer=1 timing_start=0"));
    update(TD_STEP,UINT64_C(7000000)); CHECK(app.rules.frames==0 && app.rules.spawned==0);
    timing=NULL;
}
static void timing_start_is_explicit(void)
{
    const char *name="timing_start_is_explicit"; timing="1"; timing_start="1";
    CHECK(open_runtime(false)); CHECK(app.menu==0 && app.rules.spawned==1);
    CHECK(strstr(boot,"timing_observer=1 timing_start=1"));
    update(TD_STEP,16667); CHECK(app.rules.frames==1);
    timing=timing_start=NULL;
}
static void fixed_qa_keeps_runtime_pause(void)
{
    const char *name="fixed_qa_keeps_runtime_pause"; fixed="0.0166666667";
    CHECK(open_runtime(true)); CHECK(strstr(boot,"clock=runtime_sim_dt") && strstr(boot,"qa_fixed_dt_requested=1"));
    update(TD_STEP,700000); CHECK(app.rules.frames==1);
    jce_runtime_set_paused(runtime,true); update(TD_STEP,700000); CHECK(app.rules.frames==1);
    jce_runtime_set_paused(runtime,false); update(TD_STEP,700000); CHECK(app.rules.frames==2);
    fixed=NULL;
}
static void fixture_isolated_and_paused(void)
{
    const char *name="fixture_isolated_and_paused"; fixture="ghost";
    CHECK(open_runtime(false)); CHECK(strstr(boot,"clock=fixture_fixed_60hz"));
    update(.001f,1000000); CHECK(app.rules.frames==1 && app.fixture_ticks==1);
    jce_runtime_set_paused(runtime,true); update(TD_STEP,1000000);
    CHECK(app.rules.frames==1 && app.fixture_ticks==1);
    jce_runtime_set_paused(runtime,false);
    for (int i=0;i<125;++i) update(.001f,1000000);
    CHECK(app.rules.frames==120 && app.fixture_ticks==120);
    fixture=NULL;
}
static void legacy_is_explicit(void)
{
    const char *name="legacy_is_explicit"; legacy="1";
    CHECK(open_runtime(true)); CHECK(strstr(boot,"clock=legacy_frame_wall legacy_frame=1"));
    update(.001f,100000); CHECK(app.rules.frames==1 && fabsf(app.rules.time-.1f)<.00001f);
    legacy=NULL;
}
/* These execute the same actual runtime/C VM/controller path with no GPU.
 * The diagnostic-only build selects them without changing the 13 clock cases. */
#if defined(TD_CENSUS_TEST_ONLY)
static void census_disabled(void)
{
    const char *name="census_disabled";
    CHECK(open_runtime(false)); CHECK(td_qa_census.alive && td_qa_census.generation);
    CHECK(!td_qa_census.enabled);
    for(int i=0;i<3;++i)update(TD_STEP,16667);
    CHECK(render_calls==3); CHECK(td_qa_census.entered==0 && td_qa_census.normal_returned==0);
}
static void census_initial_consumption(void)
{
    const char *name="census_initial_consumption"; census="1"; fixture="initial";
    CHECK(open_runtime(false)); CHECK(td_qa_census.enabled && td_qa_census.alive);
    const uint32_t generation=td_qa_census.generation;
    for(int i=0;i<12;++i)update(TD_STEP,16667);
    CHECK(td_qa_census.entered==12); CHECK(td_qa_census.normal_returned==12);
    CHECK(td_qa_census.generation==generation && !td_qa_census.saturated && !td_qa_census.invalidated);
    CHECK(app.rules.frames==0 && render_calls==12); /* Rules freeze does not freeze on_update. */
}
static void census_no_update_no_credit(void)
{
    const char *name="census_no_update_no_credit"; census="1";
    CHECK(open_runtime(false)); const uint32_t entered=td_qa_census.entered;
    wall_counter+=1000000; /* A callback/window with no actual runtime update. */
    CHECK(td_qa_census.entered==entered && td_qa_census.normal_returned==0 && render_calls==0);
}
static void census_instance_generation(void)
{
    const char *name="census_instance_generation"; census="1";
    CHECK(open_runtime(false)); update(TD_STEP,16667);
    CHECK(td_qa_census.entered==1 && td_qa_census.normal_returned==1);
    const uint32_t previous=td_qa_census.generation;
    close_runtime(); CHECK(!td_qa_census.alive && td_qa_census.generation==previous);
    CHECK(open_runtime(false)); CHECK(td_qa_census.generation==previous+1u);
    CHECK(td_qa_census.entered==0 && td_qa_census.normal_returned==0);
    update(TD_STEP,16667); CHECK(td_qa_census.entered==1 && td_qa_census.normal_returned==1);
}
static void census_counter_overflow(void)
{
    const char *name="census_counter_overflow"; census="1";
    CHECK(open_runtime(false)); td_qa_census.entered=UINT32_MAX;
    update(TD_STEP,16667); CHECK(td_qa_census.entered==UINT32_MAX && td_qa_census.saturated);
    td_qa_census.normal_returned=UINT32_MAX;
    update(TD_STEP,16667); CHECK(td_qa_census.normal_returned==UINT32_MAX && td_qa_census.saturated);
}
static void census_invalid_host(void)
{
    const char *name="census_invalid_host"; census="1";
    CHECK(open_runtime(false)); CHECK(on_update(NULL,TD_STEP)!=JCE_C_OK);
    CHECK(td_qa_census.invalidated && td_qa_census.entered==0 && td_qa_census.normal_returned==0);
    CHECK(render_calls==0);
}
static void census_multiple_instances(void)
{
    const char *name="census_multiple_instances"; census="1";
    CHECK(open_runtime(false)); JceCScriptContext context={0};
    context.host=&app.host; context.host_size=sizeof app.host;
    CHECK(create(&context)==NULL); CHECK(td_qa_census.invalidated && td_qa_census.alive);
}
static void census_generation_overflow(void)
{
    const char *name="census_generation_overflow"; census="1";
    const uint32_t previous=td_qa_census.generation;
    td_qa_census.generation=UINT32_MAX;
    CHECK(open_runtime(false)); CHECK(td_qa_census.generation==UINT32_MAX && td_qa_census.saturated);
    close_runtime(); td_qa_census.generation=previous; td_qa_census.saturated=0;
}
#endif

static void run_case(void (*test)(void))
{
    timing=timing_start=legacy=fixed=fixture=census=NULL; ++cases; test(); close_runtime();
}
static int clock_tests(void)
{
    if (jce_script_register_linked_languages(NULL)<1) {
        puts("TD_CLOCK SETUP_FAILED native C registration unavailable"); return 2;
    }
#if defined(TD_CENSUS_TEST_ONLY)
    run_case(census_disabled); run_case(census_initial_consumption); run_case(census_no_update_no_credit);
    run_case(census_instance_generation); run_case(census_counter_overflow); run_case(census_invalid_host);
    run_case(census_multiple_instances); run_case(census_generation_overflow);
    printf("TD_CENSUS %s cases=%u checks=%u failures=%u scope=actual_runtime_native_c_update_census_headless\n",
        failures?"FAIL":"PASS",cases,checks,failures);
#else
    run_case(runtime_pause); run_case(zero_scale); run_case(half_scale); run_case(double_scale);
    run_case(host_pause_resume); run_case(host_interval_not_wall); run_case(bounded_hitch);
    run_case(presentation_rates); run_case(timing_is_observer); run_case(timing_start_is_explicit);
    run_case(fixed_qa_keeps_runtime_pause); run_case(fixture_isolated_and_paused); run_case(legacy_is_explicit);
    printf("TD_CLOCK %s cases=%u checks=%u failures=%u scope=actual_runtime_native_c_rules_headless\n",
        failures?"FAIL":"PASS",cases,checks,failures);
#endif
    return failures?1:0;
}

/* Use the SDK public SDL-free application entry. The linked native roster
 * owns its normal static dependencies; do not prune the SDK link graph. */
static bool clock_init(const JceServices *services,void *user)
{
    (void)services; (void)user;
    const int result=clock_tests();
    fflush(stdout);
    jce_engine_request_quit();
    return result==0; /* Normal init succeeds; ordinary assertion failures exit 1. */
}
static bool clock_should_quit(void *user) { (void)user; return true; }
JceAppDesc td_clock_get_desc(void)
{
    JceAppDesc desc={0}; desc.name="TD runtime clock consumer";
    desc.headless=true; desc.init=clock_init; desc.should_quit=clock_should_quit;
    return desc;
}
JCE_MAIN(td_clock_get_desc)
