/* Exact private timer and paused branch extracted from jce_engine.c.
 * Counted host ports, no real SDL window/GPU or rendered-frame certificate. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/application/jce_lifecycle.h>
#include <jce/os/core/jce_log.h>
#include "application/jce_engine_perf_hitch.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Only the fields borrowed by these two exact production bodies. */
struct JceEngine {
    uint32_t max_frames, frame_index;
    double max_seconds;
    uint64_t perf_freq, run_start_counter;
    struct { bool enabled; } kpi_clock;
    void *input, *trace_capture, *renderer;
    bool app_init_entered;
    JceEnginePerfHitch perf_hitch;
    struct { uint32_t drawable_reason; } host_lifecycle;
};
static void engine_frame_cap_wait(void *state) { (void)state; TEST_FAIL_MESSAGE("Frozen host cannot consume render pacing"); }
int JCE_CALL jce_engine_get_target_fps(void) { return 0; }
static int s_render_paused, s_in_render_frame;
static void SDL_SetAtomicInt(int *value,int state) { *value=state; }
bool JCE_CALL jce_lifecycle_is_paused(void) { return false; }
static bool background;
static unsigned cpu_ticks, software_frames, should_quit_calls, dialogs;
static int dialog_flags, log_level;
static char dialog_title[64], dialog_message[512];
static struct { bool (*should_quit)(void *); void *user_data; } g_app_desc;
bool JCE_CALL jce_engine_get_run_in_background(void) { return background; }
static void engine_cpu_tick(JceEngine *owner,float dt) { (void)owner; (void)dt; ++cpu_ticks; }
static bool engine_owner_ready(const JceEngine *owner) { (void)owner;return true; }
static JceEngine engine;
static unsigned input_calls, sleeps, polls, counter_calls, clocks, logs, quits, releases;
static uint32_t sleep_ms;
static uint64_t counter, sleep_advance;
static bool release_full;
static unsigned full_releases;
static char sequence[64], message[256];
static unsigned sequence_size;
static void record(char code)
{
    TEST_ASSERT_LESS_THAN_UINT(sizeof(sequence)-1, sequence_size);
    sequence[sequence_size++]=code;
    sequence[sequence_size]='\0';
}
static int SDL_GetAtomicInt(const int *value) { return *value; }
static void counted_input(void *input)
{
    TEST_ASSERT_EQUAL_PTR(engine.input,input); ++input_calls; record('I');
}
static void counted_sleep(uint32_t ms)
{
    ++sleeps; sleep_ms=ms; counter+=sleep_advance; record('S');
}
static void counted_poll(void *capture)
{
    TEST_ASSERT_EQUAL_PTR(engine.trace_capture,capture); ++polls; record('T');
}
static uint64_t counted_counter(void) { ++counter_calls; record('Q'); return counter; }
static bool counted_clock(void *clock, uint64_t freq, uint64_t now)
{
    TEST_ASSERT_EQUAL_PTR(&engine.kpi_clock,clock);
    TEST_ASSERT_EQUAL_UINT64(engine.perf_freq,freq);
    TEST_ASSERT_EQUAL_UINT64(counter,now);
    ++clocks; record('C'); return true;
}
static void counted_log(int level, const char *tag, const char *file, int line,
                        const char *format, ...)
{
    log_level=level;
    TEST_ASSERT_EQUAL_STRING("engine",tag);
    TEST_ASSERT_NOT_NULL(file); TEST_ASSERT_GREATER_THAN_INT(0,line);
    va_list args; va_start(args,format);
    vsnprintf(message,sizeof(message),format,args); va_end(args);
    ++logs; record('L');
}
static void counted_lifecycle(int event)
{
    TEST_ASSERT_EQUAL_INT(JCE_LIFECYCLE_WILL_QUIT,event); ++quits; record('W');
}
static JceAppResult engine_frame_release(JceEngine *owner,JceAppResult result,bool full)
{
    TEST_ASSERT_EQUAL_PTR(&engine,owner);
    ++releases; full_releases+=full?1u:0u; release_full=full; record('R'); return result;
}
static bool counted_should_quit(void *user) { (void)user; ++should_quit_calls; return false; }
static bool counted_is_fallback(void *renderer) { TEST_ASSERT_EQUAL_PTR(engine.renderer,renderer); return true; }
static void counted_software_frame(void *renderer) { TEST_ASSERT_EQUAL_PTR(engine.renderer,renderer); ++software_frames; record('F'); }
static int counted_dialog(unsigned flags,const char *title,const char *text,void *parent)
{
    (void)parent; ++dialogs; dialog_flags=(int)flags;
    snprintf(dialog_title,sizeof dialog_title,"%s",title);
    snprintf(dialog_message,sizeof dialog_message,"%s",text); return 0;
}
#define SDL_MESSAGEBOX_ERROR 1u
#define SDL_ShowSimpleMessageBox counted_dialog
#define jce_renderer_is_fallback counted_is_fallback
#define jce_renderer_render_fallback_frame counted_software_frame
#define LOG_TAG "engine"
#define jce_input_update counted_input
#define jce_thread_sleep_ms counted_sleep
#define jce_trace_capture_poll counted_poll
#define jce_time_perf_counter counted_counter
#define jce_engine_kpi_clock_check counted_clock
#define jce_log_write counted_log
#define jce_lifecycle_emit counted_lifecycle
#define JCE_PROFILE_ZONE_END record('Z')
#include JCE_ENGINE_MINIMIZED_AUTOSTOP_OWNER

void setUp(void)
{
    memset(&engine,0,sizeof(engine));
    engine.perf_freq=1000; engine.run_start_counter=1000;
    engine.app_init_entered=true; engine.renderer=&software_frames;
    background=false; cpu_ticks=software_frames=should_quit_calls=dialogs=0;
    dialog_flags=log_level=0; dialog_title[0]=dialog_message[0]='\0';
    g_app_desc.should_quit=counted_should_quit;
    engine.input=&input_calls; engine.trace_capture=&polls;
    s_render_paused=1; counter=1000; sleep_advance=33;
    input_calls=sleeps=polls=counter_calls=clocks=logs=quits=releases=0;
    sleep_ms=0; release_full=true; full_releases=0; sequence_size=0;
    sequence[0]=message[0]='\0';
}
void tearDown(void) {}
static void check_paused_ports(unsigned calls)
{
    TEST_ASSERT_EQUAL_UINT(calls,sleeps);
    TEST_ASSERT_EQUAL_UINT(calls,polls);
    TEST_ASSERT_EQUAL_UINT(calls,releases);
    TEST_ASSERT_EQUAL_UINT(33,sleep_ms);
    TEST_ASSERT_FALSE(release_full); /* Never marks a new completed render; prior capture keeps its owner. */
}
static void test_elapsed_limit_runs_after_yield_without_counting_frame(void)
{
    engine.max_seconds=.03; engine.frame_index=7; engine.kpi_clock.enabled=true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(7,engine.frame_index);
    TEST_ASSERT_EQUAL_UINT(1,input_calls); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(1,counter_calls); TEST_ASSERT_EQUAL_UINT(1,clocks);
    TEST_ASSERT_EQUAL_UINT(1,quits); TEST_ASSERT_EQUAL_UINT(1,logs);
    TEST_ASSERT_EQUAL_STRING("ISTQCLWZR",sequence);
    TEST_ASSERT_NOT_NULL(strstr(message,"JCE_MAX_SECONDS reached (7 frames"));
}
static void test_before_deadline_preserves_frame_count_and_continues(void)
{
    engine.max_seconds=1; engine.frame_index=9;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(9,engine.frame_index); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(0,quits); TEST_ASSERT_EQUAL_UINT(0,logs);
    TEST_ASSERT_EQUAL_STRING("ISTQZR",sequence);
}
static void test_disabled_limits_preserve_ordinary_paused_behavior(void)
{
    engine.frame_index=12;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(12,engine.frame_index); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(0,counter_calls); TEST_ASSERT_EQUAL_UINT(0,quits);
    TEST_ASSERT_EQUAL_STRING("ISTZR",sequence);
}
static void test_frame_limit_cannot_count_repeated_paused_polls(void)
{
    engine.max_frames=1;
    for (unsigned i=0;i<3;++i)
        TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(0,engine.frame_index); check_paused_ports(3);
    TEST_ASSERT_EQUAL_UINT(0,quits); TEST_ASSERT_EQUAL_UINT(0,counter_calls);
}
static void test_already_reached_frame_limit_is_not_paused_completion(void)
{
    engine.max_frames=7; engine.frame_index=7;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(7,engine.frame_index); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(0,quits);
}
static void test_zero_frequency_does_not_claim_elapsed_limit(void)
{
    engine.max_seconds=.1; engine.perf_freq=0; engine.frame_index=3;
    counter=100000;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(3,engine.frame_index); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(0,counter_calls); TEST_ASSERT_EQUAL_UINT(0,quits);
}
static void test_backward_counter_does_not_claim_elapsed_limit(void)
{
    engine.max_seconds=.1; engine.frame_index=4; counter=900; sleep_advance=0;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(4,engine.frame_index); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(1,counter_calls); TEST_ASSERT_EQUAL_UINT(0,quits);
}
static void test_no_input_still_observes_timed_exit(void)
{
    engine.input=NULL; engine.max_seconds=.03;
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(0,input_calls); check_paused_ports(1);
    TEST_ASSERT_EQUAL_UINT(1,quits); TEST_ASSERT_EQUAL_UINT(0,clocks);
    TEST_ASSERT_EQUAL_UINT(0,engine.frame_index);
    TEST_ASSERT_EQUAL_STRING("STQLWZR",sequence);
}
static void test_completed_frame_limit_keeps_existing_counter_behavior(void)
{
    engine.max_frames=2;
    TEST_ASSERT_FALSE(jce_engine_benchmark_autostop(&engine,false,true));
    TEST_ASSERT_EQUAL_UINT(1,engine.frame_index);
    TEST_ASSERT_TRUE(jce_engine_benchmark_autostop(&engine,false,true));
    TEST_ASSERT_EQUAL_UINT(2,engine.frame_index); TEST_ASSERT_EQUAL_UINT(1,quits);
    TEST_ASSERT_NOT_NULL(strstr(message,"JCE_MAX_FRAMES reached (2 frames"));
}
static void test_headless_completed_iteration_keeps_time_and_frame_count(void)
{
    engine.max_seconds=.1; counter=2000;
    TEST_ASSERT_TRUE(jce_engine_benchmark_autostop(&engine,true,true));
    TEST_ASSERT_EQUAL_UINT(1,engine.frame_index); TEST_ASSERT_EQUAL_UINT(1,quits);
    TEST_ASSERT_NOT_NULL(strstr(message," (headless)"));
}
static void test_software_fallback_counts_only_completed_iterations_and_exits(void)
{
    engine.app_init_entered=false; engine.max_frames=3;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(3,engine.frame_index); TEST_ASSERT_EQUAL_UINT(3,software_frames);
    TEST_ASSERT_EQUAL_UINT(3,input_calls); TEST_ASSERT_EQUAL_UINT(3,polls);
    TEST_ASSERT_EQUAL_UINT(0,full_releases);
    TEST_ASSERT_EQUAL_UINT(1,quits); TEST_ASSERT_FALSE(release_full);
    TEST_ASSERT_EQUAL_UINT(0,cpu_ticks); TEST_ASSERT_EQUAL_UINT(0,should_quit_calls);
}
static void test_software_fallback_elapsed_limit_uses_same_quit_owner(void)
{
    engine.app_init_entered=false; engine.max_seconds=1; engine.kpi_clock.enabled=true;
    counter=1999; TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_software_iteration(&engine));
    counter=2000; TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(2,engine.frame_index); TEST_ASSERT_EQUAL_UINT(1,quits);
    TEST_ASSERT_EQUAL_UINT(1,clocks); TEST_ASSERT_FALSE(release_full);
    TEST_ASSERT_EQUAL_UINT(0,should_quit_calls);
}
static void test_uninitialized_should_quit_never_runs_in_fallback(void)
{
    engine.app_init_entered=false;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(0,should_quit_calls); TEST_ASSERT_EQUAL_UINT(0,cpu_ticks);
    TEST_ASSERT_EQUAL_UINT(1,software_frames); TEST_ASSERT_FALSE(release_full);
}
static void test_uninitialized_blocked_owner_cannot_dispatch_app_update(void)
{
    engine.app_init_entered=false; background=true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(0,cpu_ticks); TEST_ASSERT_EQUAL_UINT(0,engine.frame_index);
    TEST_ASSERT_FALSE(release_full);
}
static void test_initialized_background_owner_keeps_cpu_update(void)
{
    background=true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_paused_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(1,cpu_ticks);
}
static void test_initialized_should_quit_owner_keeps_callback(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,test_software_iteration(&engine));
    TEST_ASSERT_EQUAL_UINT(1,should_quit_calls);
}
static void call_actual_fatal(bool allow)
{
#if JCE_ENGINE_FATAL_POLICY_PARAMETER
    fatal_msg(allow,"device failure %d",7);
#else
    (void)allow; fatal_msg("device failure %d",7);
#endif
}
static void test_noninteractive_fatal_keeps_exact_error_without_modal(void)
{
    call_actual_fatal(false);
    TEST_ASSERT_EQUAL_UINT(0,dialogs); TEST_ASSERT_EQUAL_UINT(1,logs);
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_ERROR,log_level);
    TEST_ASSERT_EQUAL_STRING("device failure 7",message);
}
static void test_interactive_fatal_keeps_original_dialog_and_one_error(void)
{
    call_actual_fatal(true);
    TEST_ASSERT_EQUAL_UINT(1,dialogs); TEST_ASSERT_EQUAL_UINT(1,logs);
    TEST_ASSERT_EQUAL_INT(SDL_MESSAGEBOX_ERROR,dialog_flags);
    TEST_ASSERT_EQUAL_STRING("JCE Fatal Error",dialog_title);
    TEST_ASSERT_EQUAL_STRING("device failure 7",dialog_message);
    TEST_ASSERT_EQUAL_STRING(dialog_message,message);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_elapsed_limit_runs_after_yield_without_counting_frame);
    RUN_TEST(test_before_deadline_preserves_frame_count_and_continues);
    RUN_TEST(test_disabled_limits_preserve_ordinary_paused_behavior);
    RUN_TEST(test_frame_limit_cannot_count_repeated_paused_polls);
    RUN_TEST(test_already_reached_frame_limit_is_not_paused_completion);
    RUN_TEST(test_zero_frequency_does_not_claim_elapsed_limit);
    RUN_TEST(test_backward_counter_does_not_claim_elapsed_limit);
    RUN_TEST(test_no_input_still_observes_timed_exit);
    RUN_TEST(test_completed_frame_limit_keeps_existing_counter_behavior);
    RUN_TEST(test_headless_completed_iteration_keeps_time_and_frame_count);
    RUN_TEST(test_software_fallback_counts_only_completed_iterations_and_exits);
    RUN_TEST(test_software_fallback_elapsed_limit_uses_same_quit_owner);
    RUN_TEST(test_uninitialized_should_quit_never_runs_in_fallback);
    RUN_TEST(test_uninitialized_blocked_owner_cannot_dispatch_app_update);
    RUN_TEST(test_initialized_background_owner_keeps_cpu_update);
    RUN_TEST(test_initialized_should_quit_owner_keeps_callback);
    RUN_TEST(test_noninteractive_fatal_keeps_exact_error_without_modal);
    RUN_TEST(test_interactive_fatal_keeps_original_dialog_and_one_error);
    return UNITY_END();
}
