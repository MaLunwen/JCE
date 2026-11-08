/* Actual CPU body, drawable query, watcher and skipped-render branch plus
 * actual lifecycle/PlayerLoop registries and fixed clock. Typed host ports;
 * no OS window, graphics device, pixels or timing acceptance. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_runtime_host_policy.inc.h>
#include <jce/application/jce_subsystem.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/runtime/jce_player_loop.h>
#include <SDL3/SDL.h>
#include "application/jce_engine_perf_hitch.h"
#ifdef JCE_TRACY_ENABLED
#undef JCE_TRACY_ENABLED
#endif
#define JCE_TRACY_ENABLED 0
#include "application/jce_engine_callbacks.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include <math.h>
#include <string.h>
#include "os/core/jce_trace_scope_internal.h"
#ifndef JCE_TEST_ENGINE_HOST_LIFECYCLE
#define JCE_TEST_ENGINE_HOST_LIFECYCLE "application/jce_engine_host_lifecycle.h"
#endif
struct JceRuntime { unsigned pauses,resumes; };
static bool flags_valid=true;
static uint32_t flags=JCE_WINDOW_INPUT_FOCUS;
static int pixel_w=800,pixel_h=600;
static bool pixel_query_valid=true,pixel_write_outputs=true,refuse_drawable_scope;
static unsigned flags_queries,pixel_queries,scope_refusals;
static unsigned draws,resizes,window_resizes,iterations,inputs,polls,sleeps;
static uint32_t last_sleep;
static uint64_t counter=1000;
static char order[256]; static unsigned order_size;
static unsigned phases[JCE_PHASE_COUNT];
static int marker;
static void record(char c) { TEST_ASSERT_LESS_THAN_UINT(sizeof(order)-1,order_size); order[order_size++]=c;order[order_size]='\0'; }
bool jce_window_get_flags(const JceWindow *w,uint32_t *out) { (void)w;++flags_queries;if(!flags_valid)return false;*out=flags;return true; }
/* The actual Core scope is retained except for an explicit zero-id refusal. */
static JceTraceScope drawable_scope_begin(const char *name)
{
    if (refuse_drawable_scope) { ++scope_refusals;return (JceTraceScope){0}; }
    return jce_trace_scope_begin(name);
}
#define jce_trace_scope_begin drawable_scope_begin
#include JCE_TEST_ENGINE_HOST_LIFECYCLE
#undef jce_trace_scope_begin
void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt) { ++rt->pauses; }
void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt) { ++rt->resumes; }
static uint64_t counted_counter(void) { return counter++; }
static uint64_t counted_freq(void) { return 1000; }
static void counted_sleep(uint32_t ms) { ++sleeps;last_sleep=ms;counter+=ms;record('Y'); }
#define jce_time_perf_counter counted_counter
#define jce_time_perf_freq counted_freq
#define jce_thread_sleep_ms counted_sleep
#include "application/jce_engine_frame_cap.inc.h"
struct JceEngine {
    bool app_init_entered;
    bool headless,destroying,finalizing,event_dispatching;
    JceWindow *window; JceRenderer *renderer; JceInput *input; JceInputActions *actions;
    JceInputRecorder *input_replayer,*input_recorder;
    jce_subsystem_registry_t *subsystems; JceServices svc;
    JceEngineHostLifecycle host_lifecycle; JceEnginePerfHitch perf_hitch;
    uint32_t max_frames,frame_index; double max_seconds;
    uint64_t perf_freq,run_start_counter; struct { bool enabled; } kpi_clock;
    void *trace_capture;
};
static JceEngine engine; static JceAppDesc g_app_desc;
static JceEngine *g_engine_owner = &engine;
/* Typed admission port; full Engine ownership is exercised separately. */
static bool owner_ready = true, close_in_update;
static int close_in_phase = -1;
static bool engine_owner_ready(const JceEngine *e)
{ TEST_ASSERT_EQUAL_PTR(&engine,e); return owner_ready; }
bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)
{ (void)r; return true; } /* Existing drawable controls retain a valid device. */
static SDL_AtomicInt s_render_paused={0},s_in_render_frame={0};
static SDL_Window *counted_window(JceWindow *w) { TEST_ASSERT_EQUAL_PTR(engine.window,w);return (SDL_Window*)&marker; }
static bool counted_pixels(SDL_Window *w,int *pw,int *ph) { TEST_ASSERT_EQUAL_PTR(&marker,w);++pixel_queries;if(pixel_write_outputs){*pw=pixel_w;*ph=pixel_h;}return pixel_query_valid; }
static void counted_resize(const JceRenderer *r,uint32_t w,uint32_t h) { TEST_ASSERT_EQUAL_PTR(engine.renderer,r);TEST_ASSERT_EQUAL_INT(pixel_w,w);TEST_ASSERT_EQUAL_INT(pixel_h,h);++resizes; }
static void counted_window_resize(JceWindow *w,uint32_t width,uint32_t height) { (void)w;(void)width;(void)height;++window_resizes; }
static bool counted_fallback(const JceRenderer *r) { (void)r;return false; }
JceAppResult JCE_CALL jce_engine_iterate(JceEngine *e) { TEST_ASSERT_EQUAL_PTR(&engine,e);++iterations;return JCE_APP_CONTINUE; }
void JCE_CALL jce_input_update(JceInput *i) { TEST_ASSERT_EQUAL_PTR(engine.input,i);++inputs;record('N'); }
void jce_actions_update(JceInputActions *a,const JceInput *i) { TEST_ASSERT_EQUAL_PTR(engine.actions,a);TEST_ASSERT_EQUAL_PTR(engine.input,i);record('B'); }
void jce_subsystem_update_all(jce_subsystem_registry_t *r,float dt) { (void)dt;TEST_ASSERT_EQUAL_PTR(engine.subsystems,r);record('S'); }
bool jce_input_replay_tick(JceInputRecorder *r,JceInput *i) { (void)r;(void)i;TEST_FAIL_MESSAGE("unexpected replay");return false; }
bool jce_input_record_tick(JceInputRecorder *r,const JceInput *i) { (void)r;(void)i;TEST_FAIL_MESSAGE("unexpected record");return false; }
void jce_input_record_close(JceInputRecorder *r) { (void)r; }
uint64_t jce_input_record_frame_count(const JceInputRecorder *r) { (void)r;return 0; }
static void counted_poll(void *capture) { (void)capture;++polls;record('T'); }
static bool counted_clock(void *clock,uint64_t freq,uint64_t now) { (void)clock;(void)freq;(void)now;return false; }
static JceAppResult engine_frame_release(JceEngine *e,JceAppResult result,bool full) { TEST_ASSERT_EQUAL_PTR(&engine,e);TEST_ASSERT_FALSE(full);jce_engine_perf_disposition_release(&e->perf_hitch,full);return result; }
#define LOG_TAG "engine"
#define jce_window_sdl counted_window
#define SDL_GetWindowSizeInPixels counted_pixels
#define jce_renderer_resize counted_resize
#define jce_window_handle_resize counted_window_resize
#define jce_renderer_is_fallback counted_fallback
#define jce_trace_capture_poll counted_poll
#define jce_engine_kpi_clock_check counted_clock
#include JCE_TEST_ENGINE_BACKGROUND_OWNER
static float app_dt;
static void update(float dt,void *u) { (void)u;app_dt=dt;record('A');if(close_in_update)owner_ready=false; }
static void draw(const JceServices *s,void *u) { (void)s;(void)u;++draws;record('D'); }
static void hook(float dt,void *user)
{
    (void)dt; unsigned phase=(unsigned)(uintptr_t)user;++phases[phase];
    const char names[]="IEFULRPO";record(names[phase]);
    if ((int)phase == close_in_phase) owner_ready=false;
}
void setUp(void)
{
    jce_trace_set_enabled(false);jce_trace_reset();jce_trace_task_restore_id(0);
    jce_lifecycle_shutdown();jce_player_loop_shutdown();
    jce_fixed_clock_init(jce_fixed_clock_default(),.01,.1);
    jce_engine_set_target_fps(0);jce_engine_set_run_in_background(false);
    memset(&engine,0,sizeof engine);memset(&g_app_desc,0,sizeof g_app_desc);
    engine.app_init_entered=true;
    owner_ready=true;close_in_update=false;close_in_phase=-1;
    memset(phases,0,sizeof phases);order[0]='\0';order_size=0;
    flags_valid=true;flags=JCE_WINDOW_INPUT_FOCUS;pixel_w=800;pixel_h=600;
    pixel_query_valid=pixel_write_outputs=true;refuse_drawable_scope=false;
    flags_queries=pixel_queries=scope_refusals=0;
    counter=1000;draws=resizes=window_resizes=iterations=inputs=polls=sleeps=0;last_sleep=0;app_dt=-1;
    engine.window=(JceWindow*)&marker;engine.renderer=(JceRenderer*)&marker;
    engine.input=(JceInput*)&marker;engine.actions=(JceInputActions*)&marker;
    engine.subsystems=(jce_subsystem_registry_t*)&marker;engine.perf_freq=1000;
    engine.run_start_counter=1000;engine.trace_capture=&marker;
    g_app_desc.update=update;g_app_desc.draw=draw;
    SDL_SetAtomicInt(&s_render_paused,0);SDL_SetAtomicInt(&s_in_render_frame,0);
    for(unsigned i=0;i<JCE_PHASE_COUNT;++i) {
        JcePlayerLoopHandle h=jce_player_loop_register((JcePlayerLoopPhase)i,0,hook,(void*)(uintptr_t)i);
        TEST_ASSERT_NOT_EQUAL(0,h.id);
    }
}
void tearDown(void) { jce_player_loop_shutdown();jce_lifecycle_shutdown();jce_trace_set_enabled(false); }
static void test_normal_and_background_consume_same_actual_cpu_body_in_order(void)
{
    engine_cpu_tick(&engine,.034f);
    TEST_ASSERT_EQUAL_STRING("IEBFFFSUAL",order);TEST_ASSERT_EQUAL_UINT(0,draws);
    TEST_ASSERT_EQUAL_UINT(0,phases[JCE_PHASE_PRE_RENDER]);TEST_ASSERT_EQUAL_UINT(0,phases[JCE_PHASE_END_OF_FRAME]);
    TEST_ASSERT_EQUAL_UINT64(3,jce_fixed_clock_default()->tick_count);
    order_size=0;order[0]='\0';
    SDL_SetAtomicInt(&s_render_paused,1);jce_engine_set_run_in_background(true);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.034f));
    TEST_ASSERT_EQUAL_STRING("IEBFFFSUALNYT",order);
    TEST_ASSERT_EQUAL_UINT(2,phases[JCE_PHASE_INITIALIZATION]); /* preserve per-iteration contract */
    TEST_ASSERT_EQUAL_UINT64(6,jce_fixed_clock_default()->tick_count);
    TEST_ASSERT_EQUAL_UINT(0,draws);TEST_ASSERT_EQUAL_UINT(0,resizes);TEST_ASSERT_EQUAL_UINT(0,engine.frame_index);
}
static void test_closing_stops_later_cpu_phases(void)
{
    close_in_phase=JCE_PHASE_INITIALIZATION;
    engine_cpu_tick(&engine,.034f);
    TEST_ASSERT_EQUAL_STRING("I",order);
    owner_ready=true;close_in_phase=-1;close_in_update=true;
    order_size=0;order[0]='\0';
    engine_cpu_tick(&engine,.034f);
    TEST_ASSERT_EQUAL_STRING("IEBFFFSUA",order);
    TEST_ASSERT_EQUAL_UINT(0,phases[JCE_PHASE_LATE_UPDATE]);
    order_size=0;order[0]='\0';
    engine_cpu_tick(&engine,.034f);
    TEST_ASSERT_EQUAL_STRING("",order);
}
static void test_desktop_minimize_allows_true_but_false_freezes_all_cpu_hooks(void)
{
    JceRuntimeHostPolicy policy={0};JceRuntime rt={0};
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&policy,&rt,true,true,30,15));
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_MINIMIZED);
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());TEST_ASSERT_FALSE(jce_lifecycle_is_focused());
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    SDL_SetAtomicInt(&s_render_paused,1);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_EQUAL_UINT(1,phases[JCE_PHASE_UPDATE]);TEST_ASSERT_EQUAL_UINT(0,rt.pauses);
    jce_runtime_host_policy_end(&policy);jce_engine_set_run_in_background(false);
    unsigned before=phases[JCE_PHASE_UPDATE];
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_EQUAL_UINT(before,phases[JCE_PHASE_UPDATE]);TEST_ASSERT_EQUAL_UINT(0,draws);
}
static void test_manual_and_mobile_pause_freeze_background_true_and_restore_is_not_resume(void)
{
    JceRuntimeHostPolicy policy={0};JceRuntime rt={0};float dt=.02f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&policy,&rt,true,true,30,15));
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&policy,true,&dt));
    SDL_SetAtomicInt(&s_render_paused,1);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_EQUAL_UINT(0,phases[JCE_PHASE_UPDATE]);
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_DID_ENTER_BACKGROUND);
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_RESTORED);
    TEST_ASSERT_TRUE(jce_lifecycle_is_paused());
    jce_engine_set_run_in_background(true); /* cannot override a real OS pause */
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_EQUAL_UINT(0,phases[JCE_PHASE_UPDATE]);
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WILL_ENTER_FOREGROUND);
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
    jce_runtime_host_policy_end(&policy);
}
static void test_zero_drawable_real_query_rejects_render_but_keeps_bounded_cpu_tick(void)
{
    jce_engine_set_run_in_background(true);pixel_w=0;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,30.0f));
    TEST_ASSERT_EQUAL_FLOAT(.1f,app_dt);TEST_ASSERT_EQUAL_UINT(0,draws);TEST_ASSERT_EQUAL_UINT(0,resizes);
    pixel_w=800;TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,30.0f));
    TEST_ASSERT_EQUAL_FLOAT(.02f,engine_host_step_dt(&engine.host_lifecycle,.02f));
}
static void test_restoration_uses_actual_focus_and_discards_one_wall_interval(void)
{
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_MINIMIZED);
    flags=0;engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_RESTORED);
    TEST_ASSERT_FALSE(jce_lifecycle_is_focused());TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,100.0f));
    flags=JCE_WINDOW_INPUT_FOCUS;engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_RESTORED);
    TEST_ASSERT_TRUE(jce_lifecycle_is_focused());
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,100.0f));
    TEST_ASSERT_EQUAL_FLOAT(.02f,engine_host_step_dt(&engine.host_lifecycle,.02f));
    flags_valid=false;engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_MINIMIZED);
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WINDOW_RESTORED);
    TEST_ASSERT_FALSE(jce_lifecycle_is_focused());
}
static void test_finite_background_dt_and_same_pacing_owner_consumes_low_cap(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,NAN));
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,-1));
    TEST_ASSERT_EQUAL_FLOAT(.1f,engine_host_step_dt(&engine.host_lifecycle,INFINITY));
    SDL_SetAtomicInt(&s_render_paused,1);jce_engine_set_run_in_background(true);jce_engine_set_target_fps(5);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.02f));
    TEST_ASSERT_GREATER_THAN_UINT(33,last_sleep);TEST_ASSERT_EQUAL_INT(5,jce_engine_get_target_fps());
    TEST_ASSERT_EQUAL_UINT(0,draws);
}
static void test_os_pause_and_zero_drawable_resize_watch_never_resize_or_draw(void)
{
    SDL_Event event={0};event.type=SDL_EVENT_WINDOW_RESIZED;
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_DID_ENTER_BACKGROUND);
    TEST_ASSERT_TRUE(jce_resize_event_watch(&engine,&event));
    TEST_ASSERT_EQUAL_UINT(0,resizes);TEST_ASSERT_EQUAL_UINT(0,window_resizes);TEST_ASSERT_EQUAL_UINT(0,iterations);
    engine_host_lifecycle_event(&engine.host_lifecycle,engine.window,SDL_EVENT_WILL_ENTER_FOREGROUND);
    pixel_h=0;TEST_ASSERT_TRUE(jce_resize_event_watch(&engine,&event));
    TEST_ASSERT_EQUAL_UINT(0,resizes);TEST_ASSERT_EQUAL_UINT(0,iterations);
    pixel_h=600;TEST_ASSERT_TRUE(jce_resize_event_watch(&engine,&event));
    TEST_ASSERT_EQUAL_UINT(1,resizes);TEST_ASSERT_EQUAL_UINT(1,window_resizes);TEST_ASSERT_EQUAL_UINT(1,iterations);
}

static JceTraceEvent drawable_events[1024];
static uint32_t drawable_event_count;
static uint64_t drawable_packet;
static unsigned drawable_packets(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    drawable_event_count=jce_trace_read(&cursor,drawable_events,1024);
    TEST_ASSERT_LESS_THAN_UINT(1024,drawable_event_count);
    TEST_ASSERT_EQUAL_UINT64(0,cursor.lost_events);
    unsigned submitted=0,begun=0,ended=0;
    drawable_packet=0;
    for(uint32_t i=0;i<drawable_event_count;++i) {
        const JceTraceEvent *event=&drawable_events[i];
        if(strcmp(event->name,"engine.drawable_transition"))continue;
        if(event->type==JCE_TRACE_EVENT_TASK_SUBMIT){++submitted;drawable_packet=event->id;}
        if(event->type==JCE_TRACE_EVENT_TASK_BEGIN)++begun;
        if(event->type==JCE_TRACE_EVENT_TASK_END)++ended;
    }
    TEST_ASSERT_EQUAL_UINT(submitted,begun);
    TEST_ASSERT_EQUAL_UINT(submitted,ended);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0,stats.active_work_items);
    return submitted;
}
static bool drawable_value(const char *name,int64_t *out)
{
    for(uint32_t i=drawable_event_count;i>0;--i) {
        const JceTraceEvent *event=&drawable_events[i-1];
        if(event->type==JCE_TRACE_EVENT_COUNTER && event->id==drawable_packet &&
            strcmp(event->name,name)==0) {
            *out=(int64_t)event->value_u64;
            return true;
        }
    }
    return false;
}
static void expect_drawable(const char *name,int64_t expected)
{
    int64_t value=0;
    TEST_ASSERT_TRUE_MESSAGE(drawable_packet!=0,"Missing actual drawable transition scope");
    TEST_ASSERT_TRUE_MESSAGE(drawable_value(name,&value),name);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(expected,value,name);
}
static void absent_drawable(const char *name)
{
    int64_t value=0;
    TEST_ASSERT_FALSE_MESSAGE(drawable_value(name,&value),name);
}
static void test_drawable_trace_initial_repeat_size_and_generation(void)
{
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    expect_drawable("drawable.headless",0);
    expect_drawable("drawable.window_present",1);
    expect_drawable("drawable.extent_query_valid",1);
    expect_drawable("drawable.native_width",800);
    expect_drawable("drawable.native_height",600);
    expect_drawable("drawable.allowed",1);
    expect_drawable("drawable.transition_flags_valid",1);
    expect_drawable("drawable.transition_input_focus",1);
    flags=JCE_WINDOW_HIDDEN; /* Flags alone are not a continuous observation. */
    for(unsigned i=0;i<32;++i)TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    TEST_ASSERT_EQUAL_UINT(1,flags_queries);
    TEST_ASSERT_EQUAL_UINT(33,pixel_queries);
    pixel_w=801;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,drawable_packets());
    expect_drawable("drawable.native_width",801);
    expect_drawable("drawable.transition_flags",JCE_WINDOW_HIDDEN);
    expect_drawable("drawable.transition_hidden",1);
    expect_drawable("drawable.allowed",1); /* Publication does not add policy. */
    jce_trace_reset(); /* Same input needs a baseline in the new ring. */
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    expect_drawable("drawable.generation",jce_trace_capture_generation());
    TEST_ASSERT_EQUAL_UINT(3,flags_queries);
}
static void test_drawable_trace_simultaneous_reasons_and_restore(void)
{
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,drawable_packets());
    expect_drawable("drawable.lifecycle_paused",1);
    expect_drawable("drawable.minimized_gate",0);
    SDL_SetAtomicInt(&s_render_paused,1);
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(3,drawable_packets());
    expect_drawable("drawable.lifecycle_paused",1);
    expect_drawable("drawable.minimized_gate",1);
    pixel_w=0;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(4,drawable_packets());
    expect_drawable("drawable.native_width",0);
    expect_drawable("drawable.extent_gate_open",0);
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
    SDL_SetAtomicInt(&s_render_paused,0);
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(5,drawable_packets());
    pixel_w=800;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(6,drawable_packets());
    expect_drawable("drawable.allowed",1);
    TEST_ASSERT_EQUAL_FLOAT(0,engine_host_step_dt(&engine.host_lifecycle,30));
    TEST_ASSERT_EQUAL_FLOAT(.02f,engine_host_step_dt(&engine.host_lifecycle,.02f));
    TEST_ASSERT_EQUAL_UINT(0,draws);
}
static void test_drawable_trace_unknown_extent_and_flags_are_not_known_zero(void)
{
    jce_trace_set_enabled(true);pixel_w=0;flags=0;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    expect_drawable("drawable.extent_query_valid",1);
    expect_drawable("drawable.native_width",0);
    expect_drawable("drawable.transition_flags_valid",1);
    expect_drawable("drawable.transition_flags",0);
    pixel_query_valid=pixel_write_outputs=flags_valid=false;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,drawable_packets());
    expect_drawable("drawable.extent_query_attempted",1);
    expect_drawable("drawable.extent_query_valid",0);
    expect_drawable("drawable.transition_flags_valid",0);
    absent_drawable("drawable.native_width");absent_drawable("drawable.native_height");
    absent_drawable("drawable.transition_flags");absent_drawable("drawable.transition_hidden");
    /* Retain the original positivity decision even if a failed SDL port writes. */
    pixel_write_outputs=true;pixel_w=800;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(3,drawable_packets());
    expect_drawable("drawable.extent_query_valid",0);
    expect_drawable("drawable.extent_gate_open",1);
    expect_drawable("drawable.allowed",1);
    absent_drawable("drawable.native_width");
    pixel_query_valid=true;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(4,drawable_packets());
    expect_drawable("drawable.extent_query_valid",1);
    expect_drawable("drawable.native_height",600);
}
static void test_drawable_trace_headless_never_queries_a_window(void)
{
    engine.headless=true;engine.host_lifecycle.render_blocked=true;
    SDL_SetAtomicInt(&s_render_paused,1);jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    expect_drawable("drawable.headless",1);
    expect_drawable("drawable.window_present",1);
    expect_drawable("drawable.windowed_inputs_used",0);
    expect_drawable("drawable.minimized_gate",1);
    expect_drawable("drawable.lifecycle_paused",1);
    expect_drawable("drawable.extent_query_attempted",0);
    expect_drawable("drawable.extent_query_valid",0);
    expect_drawable("drawable.transition_flags_query_attempted",0);
    expect_drawable("drawable.transition_flags_valid",0);
    expect_drawable("drawable.allowed",1);
    absent_drawable("drawable.native_width");absent_drawable("drawable.transition_flags");
    TEST_ASSERT_EQUAL_UINT(0,pixel_queries);TEST_ASSERT_EQUAL_UINT(0,flags_queries);
    TEST_ASSERT_TRUE(engine.host_lifecycle.render_blocked); /* Original bypass retained. */
    engine.headless=false;engine.window=NULL;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,drawable_packets());
    expect_drawable("drawable.window_present",0);
    expect_drawable("drawable.extent_query_attempted",0);
    TEST_ASSERT_EQUAL_UINT(0,pixel_queries);TEST_ASSERT_EQUAL_UINT(0,flags_queries);
}
static void test_actual_blocked_branch_retains_all_gate_reasons_without_trace(void)
{
    /* Each combination is one disjoint bucket. Trace-off classification must
     * not consume the transition cache or infer focus/minimize from flags. */
    for (uint32_t mask=1;mask<=7;++mask) {
        jce_engine_perf_hitch_init(&engine.perf_hitch,false,4.0);
        jce_engine_perf_disposition_init(&engine.perf_hitch,true,1000);
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&engine.perf_hitch));
        TEST_ASSERT_TRUE(jce_engine_perf_disposition_boundary(&engine.perf_hitch,1000,0,9));
        SDL_SetAtomicInt(&s_render_paused,(mask&JCE_ENGINE_DRAWABLE_MINIMIZED)!=0);
        if(mask&JCE_ENGINE_DRAWABLE_PAUSED)jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
        else jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
        pixel_w=(mask&JCE_ENGINE_DRAWABLE_EXTENT)?0:800;
        pixel_h=600;
        TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_background_iteration(&engine,.016f));
        jce_engine_perf_hitch_end_interval(&engine.perf_hitch,false);
        TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&engine.perf_hitch,1000,33));
        const JceEnginePerfDispositionBucket *b=
            &engine.perf_hitch.disposition.buckets[JCE_ENGINE_DISPOSITION_DRAWABLE+mask];
        TEST_ASSERT_EQUAL_UINT64(1,b->count);TEST_ASSERT_EQUAL_UINT64(33,b->ticks);
        TEST_ASSERT_EQUAL_UINT64(0,b->first.interval);TEST_ASSERT_EQUAL_UINT64(9,b->first.row);
        TEST_ASSERT_EQUAL_UINT64(0,b->first.begin);TEST_ASSERT_EQUAL_UINT64(33,b->last.end);
        TEST_ASSERT_EQUAL_UINT64(1,engine.perf_hitch.disposition.nonfull);
        TEST_ASSERT_EQUAL_UINT(0,flags_queries);
    }
}

static void test_drawable_trace_disabled_suppressed_and_refused_scope_retry(void)
{
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(0,drawable_packets());TEST_ASSERT_EQUAL_UINT(0,flags_queries);
    jce_trace_set_enabled(true);refuse_drawable_scope=true;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,scope_refusals);
    TEST_ASSERT_EQUAL_UINT(0,drawable_packets());TEST_ASSERT_EQUAL_UINT(0,flags_queries);
    refuse_drawable_scope=false;
    TEST_ASSERT_TRUE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,drawable_packets());
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());pixel_w=0;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(1,flags_queries);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,drawable_packets());
    expect_drawable("drawable.native_width",0);
    jce_trace_set_enabled(false);pixel_h=0;
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(2,flags_queries);
    jce_trace_set_enabled(true);
    TEST_ASSERT_FALSE(engine_drawable_allowed(&engine));
    TEST_ASSERT_EQUAL_UINT(3,drawable_packets());
    expect_drawable("drawable.native_height",0);
    TEST_ASSERT_EQUAL_UINT(3,flags_queries);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_and_background_consume_same_actual_cpu_body_in_order);
    RUN_TEST(test_closing_stops_later_cpu_phases);
    RUN_TEST(test_desktop_minimize_allows_true_but_false_freezes_all_cpu_hooks);
    RUN_TEST(test_manual_and_mobile_pause_freeze_background_true_and_restore_is_not_resume);
    RUN_TEST(test_zero_drawable_real_query_rejects_render_but_keeps_bounded_cpu_tick);
    RUN_TEST(test_restoration_uses_actual_focus_and_discards_one_wall_interval);
    RUN_TEST(test_finite_background_dt_and_same_pacing_owner_consumes_low_cap);
    RUN_TEST(test_os_pause_and_zero_drawable_resize_watch_never_resize_or_draw);
    RUN_TEST(test_drawable_trace_initial_repeat_size_and_generation);
    RUN_TEST(test_drawable_trace_simultaneous_reasons_and_restore);
    RUN_TEST(test_drawable_trace_unknown_extent_and_flags_are_not_known_zero);
    RUN_TEST(test_drawable_trace_headless_never_queries_a_window);
    RUN_TEST(test_drawable_trace_disabled_suppressed_and_refused_scope_retry);
    RUN_TEST(test_actual_blocked_branch_retains_all_gate_reasons_without_trace);
    return UNITY_END();
}
