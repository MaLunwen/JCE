/* Exact host policy plus actual lifecycle registry. Faultable allocator and
 * counted FPS/audio ports, no engine/SDL window, physical timing or GPU. */
#include "unity.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#ifndef JCE_TEST_RUNTIME_HOST_POLICY_HEADER
#define JCE_TEST_RUNTIME_HOST_POLICY_HEADER <jce/application/jce_runtime_host_policy.inc.h>
#endif
#include JCE_TEST_RUNTIME_HOST_POLICY_HEADER
#include <stdlib.h>
#include <string.h>
#ifndef JCE_TEST_BACKGROUND_PROPERTY_OWNER
#error Actual background property owner is required
#endif
#include JCE_TEST_BACKGROUND_PROPERTY_OWNER
struct JceRuntime { unsigned pause_count, resume_count; };
static struct {
    int fps;
    unsigned fps_writes, allocations, frees;
    bool fail_alloc, nested_focus_gain, close_on_pause;
    JceRuntimeHostPolicy *close_policy;
} witness;
static void *port_alloc(size_t size,void *ctx) { (void)ctx; ++witness.allocations; return witness.fail_alloc ? NULL : malloc(size); }
static void *port_realloc(void *ptr,size_t size,void *ctx) { (void)ctx; ++witness.allocations; return witness.fail_alloc ? NULL : realloc(ptr,size); }
static void port_free(void *ptr,void *ctx) { (void)ctx; if (ptr) ++witness.frees; free(ptr); }
jce_allocator_t JCE_CALL jce_allocator_default(void) { jce_allocator_t a={port_alloc,port_realloc,port_free,NULL}; return a; }
void JCE_CALL jce_log_write(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{ (void)level;(void)tag;(void)file;(void)line;(void)format; }
int JCE_CALL jce_engine_get_target_fps(void) { return witness.fps; }
void JCE_CALL jce_engine_set_target_fps(int value) { witness.fps=value; ++witness.fps_writes; }
void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt) {
    ++rt->pause_count;
    if (witness.nested_focus_gain) { witness.nested_focus_gain=false; jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED); }
    if (witness.close_on_pause) { witness.close_on_pause=false; jce_runtime_host_policy_end(witness.close_policy); }
}
void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt) { ++rt->resume_count; }
#ifdef JCE_TEST_DEFAULT_HOST_POLICY_ADAPTER
static struct { JceRuntimeHostPolicy policy; } s_default_host;
#include JCE_TEST_DEFAULT_HOST_POLICY_ADAPTER
static void test_actual_default_main_adapter_obeys_focus_and_drops_resume_dt(void) {
    JceRuntime rt={0}; float dt=20.0f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&s_default_host.policy,&rt,false,true,30,15));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_FALSE(default_player_policy_update(&dt));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    TEST_ASSERT_FALSE(default_player_policy_update(&dt)); TEST_ASSERT_EQUAL_FLOAT(0.0f,dt);
    dt=0.016f; TEST_ASSERT_TRUE(default_player_policy_update(&dt));
    jce_runtime_host_policy_end(&s_default_host.policy);
}
#endif
void setUp(void) { jce_lifecycle_shutdown(); jce_engine_set_run_in_background(false); memset(&witness,0,sizeof witness);
#ifdef JCE_TEST_DEFAULT_HOST_POLICY_ADAPTER
    memset(&s_default_host,0,sizeof s_default_host);
#endif
}
void tearDown(void) { jce_lifecycle_shutdown(); }
static void test_foreground_uncapped_intent_retained_and_repeated_begin_no_second_listener(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=0.016f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    const unsigned allocations=witness.allocations;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,99,99));
    TEST_ASSERT_EQUAL_UINT(allocations,witness.allocations);
    TEST_ASSERT_TRUE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_INT(0,witness.fps);
    TEST_ASSERT_EQUAL_UINT(0,rt.pause_count); TEST_ASSERT_EQUAL_UINT(0,witness.fps_writes);
    jce_runtime_host_policy_end(&p);
}
static void test_focus_loss_false_blocks_simulation_and_restores_zero_without_catchup(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=20.0f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_EQUAL_INT(15,witness.fps); TEST_ASSERT_EQUAL_UINT(1,rt.pause_count);
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt));
    TEST_ASSERT_FALSE(jce_runtime_host_policy_draw(&p,false)); TEST_ASSERT_TRUE(jce_runtime_host_policy_draw(&p,true));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED); TEST_ASSERT_EQUAL_INT(0,witness.fps);
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_FLOAT(0.0f,dt);
    dt=0.016f; TEST_ASSERT_TRUE(jce_runtime_host_policy_step(&p,false,&dt));
    TEST_ASSERT_EQUAL_FLOAT(0.016f,dt); TEST_ASSERT_EQUAL_UINT(1,rt.resume_count);
    jce_runtime_host_policy_end(&p);
}
static void test_background_true_keeps_simulation_and_temporarily_paces_present(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=0.067f; witness.fps=144;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,true,true,30,15));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_TRUE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_FLOAT(0.067f,dt);
    TEST_ASSERT_EQUAL_INT(15,witness.fps); TEST_ASSERT_EQUAL_UINT(0,rt.pause_count);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    TEST_ASSERT_EQUAL_INT(144,witness.fps); TEST_ASSERT_TRUE(jce_runtime_host_policy_step(&p,false,&dt));
    jce_runtime_host_policy_end(&p);
}
static void test_manual_pause_focus_nesting_resumes_audio_only_after_both_release(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=0.016f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,true,&dt)); TEST_ASSERT_EQUAL_INT(30,witness.fps);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); TEST_ASSERT_EQUAL_INT(15,witness.fps);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED); TEST_ASSERT_EQUAL_INT(30,witness.fps);
    TEST_ASSERT_EQUAL_UINT(1,rt.pause_count); TEST_ASSERT_EQUAL_UINT(0,rt.resume_count);
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_INT(0,witness.fps);
    TEST_ASSERT_EQUAL_UINT(1,rt.resume_count); jce_runtime_host_policy_end(&p);
}
static void test_os_pause_resume_between_updates_still_discards_observed_interval(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=30.0f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,true,true,30,15));
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE); TEST_ASSERT_EQUAL_INT(15,witness.fps);
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_FLOAT(0.0f,dt);
    TEST_ASSERT_EQUAL_UINT(1,rt.pause_count); TEST_ASSERT_EQUAL_UINT(1,rt.resume_count);
    jce_runtime_host_policy_end(&p);
}
static void test_repeated_focus_and_end_unregister_exactly_once(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; witness.fps=240;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_EQUAL_UINT(1,rt.pause_count); TEST_ASSERT_EQUAL_UINT(1,witness.fps_writes);
    jce_runtime_host_policy_end(&p); TEST_ASSERT_EQUAL_INT(240,witness.fps);
    const unsigned writes=witness.fps_writes;
    jce_runtime_host_policy_end(&p); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_EQUAL_UINT(writes,witness.fps_writes); TEST_ASSERT_EQUAL_UINT(0,rt.resume_count);
    TEST_ASSERT_EQUAL_UINT(1,rt.pause_count);
}
static void test_listener_oom_fails_before_audio_or_fps_and_can_retry(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; witness.fail_alloc=true;
    TEST_ASSERT_FALSE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    TEST_ASSERT_FALSE(p.initialized); TEST_ASSERT_NULL(p.runtime);
    TEST_ASSERT_EQUAL_UINT(0,witness.fps_writes); TEST_ASSERT_EQUAL_UINT(0,rt.pause_count);
    witness.fail_alloc=false; TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    jce_runtime_host_policy_end(&p);
}
static void test_nested_focus_gain_does_not_apply_obsolete_outer_background_rate(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=0.016f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    witness.nested_focus_gain=true; jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_TRUE(jce_lifecycle_is_focused()); TEST_ASSERT_FALSE(p.blocked);
    TEST_ASSERT_EQUAL_INT(0,witness.fps); TEST_ASSERT_EQUAL_UINT(1,rt.pause_count); TEST_ASSERT_EQUAL_UINT(1,rt.resume_count);
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt));
    jce_runtime_host_policy_end(&p);
}
static void test_stop_during_policy_callback_cannot_rearm_or_receive_later_events(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0};
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,30,15));
    witness.close_policy=&p; witness.close_on_pause=true;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); TEST_ASSERT_FALSE(p.initialized);
    TEST_ASSERT_EQUAL_INT(0,witness.fps); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    TEST_ASSERT_EQUAL_UINT(0,rt.resume_count);
}
static void test_headless_zero_dt_no_listener_and_explicit_rates_zero_remain_valid(void) {
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=0.0f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,false,30,15));
    TEST_ASSERT_EQUAL_UINT(0,witness.allocations); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_TRUE(jce_runtime_host_policy_step(&p,false,&dt)); TEST_ASSERT_EQUAL_FLOAT(0.0f,dt);
    TEST_ASSERT_EQUAL_INT(0,witness.fps); jce_runtime_host_policy_end(&p);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,false,true,0,0));
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,true,&dt)); TEST_ASSERT_EQUAL_INT(0,witness.fps);
    jce_runtime_host_policy_end(&p);
}
static void test_effective_background_permission_tracks_manual_and_os_pause(void)
{
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; float dt=.02f;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,true,true,30,15));
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,true,&dt));
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt));
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    TEST_ASSERT_FALSE(jce_runtime_host_policy_step(&p,false,&dt));
    TEST_ASSERT_EQUAL_FLOAT(0.0f,dt);
    jce_runtime_host_policy_end(&p);
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
}
static void test_actual_background_property_nested_lifo_restore_and_external_override(void)
{
    JceRuntimeHostPolicy outer={0},inner={0}; JceRuntime rt={0};
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&outer,&rt,true,true,30,15));
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&inner,&rt,false,true,30,15));
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    jce_runtime_host_policy_end(&inner); TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    jce_runtime_host_policy_end(&outer); TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&inner,&rt,false,true,30,15));
    jce_engine_set_run_in_background(true); /* new owner intent must win */
    jce_runtime_host_policy_end(&inner); TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    jce_runtime_host_policy_end(&inner); TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
}
static void test_background_property_is_not_published_on_listener_oom_or_end_inside_pause(void)
{
    JceRuntimeHostPolicy p={0}; JceRuntime rt={0}; witness.fail_alloc=true;
    TEST_ASSERT_FALSE(jce_runtime_host_policy_begin(&p,&rt,true,true,30,15));
    TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
    witness.fail_alloc=false;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&p,&rt,true,true,30,15));
    witness.close_on_pause=true; witness.close_policy=&p;
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    TEST_ASSERT_FALSE(p.initialized); TEST_ASSERT_FALSE(jce_engine_get_run_in_background());
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_effective_background_permission_tracks_manual_and_os_pause);
    RUN_TEST(test_actual_background_property_nested_lifo_restore_and_external_override);
    RUN_TEST(test_background_property_is_not_published_on_listener_oom_or_end_inside_pause);
    RUN_TEST(test_foreground_uncapped_intent_retained_and_repeated_begin_no_second_listener);
    RUN_TEST(test_focus_loss_false_blocks_simulation_and_restores_zero_without_catchup);
    RUN_TEST(test_background_true_keeps_simulation_and_temporarily_paces_present);
    RUN_TEST(test_manual_pause_focus_nesting_resumes_audio_only_after_both_release);
    RUN_TEST(test_os_pause_resume_between_updates_still_discards_observed_interval);
    RUN_TEST(test_repeated_focus_and_end_unregister_exactly_once);
    RUN_TEST(test_listener_oom_fails_before_audio_or_fps_and_can_retry);
    RUN_TEST(test_nested_focus_gain_does_not_apply_obsolete_outer_background_rate);
    RUN_TEST(test_stop_during_policy_callback_cannot_rearm_or_receive_later_events);
    RUN_TEST(test_headless_zero_dt_no_listener_and_explicit_rates_zero_remain_valid);
#ifdef JCE_TEST_DEFAULT_HOST_POLICY_ADAPTER
    RUN_TEST(test_actual_default_main_adapter_obeys_focus_and_drops_resume_dt);
#endif
    return UNITY_END();
}
