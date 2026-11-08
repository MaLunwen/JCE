/* Exact Engine entry/event/watcher/post-callback refusal branches. Typed
 * ports count terminal cleanup requests and forbidden next-stage work; no
 * actual engine boot, browser, GPU, or renderer recreation is certified. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/renderer/jce_renderer.h>
#ifdef JCE_TRACY_ENABLED
#undef JCE_TRACY_ENABLED
#endif
#define JCE_TRACY_ENABLED 0
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include "application/jce_engine_perf_hitch.h"
#include <SDL3/SDL.h>
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "application/jce_engine_trace_scope.h"
#include "os/core/jce_trace_capture_internal.h"
#include <string.h>
struct JceEngine {
    JceRenderer *renderer; JceTraceCapture *trace_capture; void *input;
    JceEnginePerfHitch perf_hitch; bool destroying,finalizing;
};
static JceEngine engine;
static bool current;
static unsigned observations,polls,releases,finalizations,next_work,atomics;
static int marker;
static SDL_AtomicInt s_in_render_frame={0};
bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)
{ TEST_ASSERT_EQUAL_PTR(engine.renderer,r);++observations;return current; }
static void counted_poll(JceTraceCapture *capture) { TEST_ASSERT_EQUAL_PTR(engine.trace_capture,capture);++polls; }
static JceAppResult engine_frame_release(JceEngine *e,JceAppResult result,bool full)
{ TEST_ASSERT_EQUAL_PTR(&engine,e);TEST_ASSERT_FALSE(full);++releases;
  jce_engine_perf_disposition_release(&e->perf_hitch,full);return result; }
static void counted_input(void *input) { (void)input;TEST_FAIL_MESSAGE("Unexpected input port"); }
static JceAppResult engine_application_result(JceEngine *e,JceAppResult result)
{ TEST_ASSERT_EQUAL_PTR(&engine,e);++finalizations;return result; }
static int counted_atomic(SDL_AtomicInt *value,int next)
{ TEST_ASSERT_EQUAL_PTR(&s_in_render_frame,value);TEST_ASSERT_EQUAL_INT(0,next);++atomics;return 1; }
#define jce_trace_capture_poll counted_poll
#define SDL_SetAtomicInt counted_atomic
#define jce_input_update counted_input
#include JCE_TEST_ENGINE_CONTEXT_OWNER
#undef jce_input_update
#undef SDL_SetAtomicInt
#undef jce_trace_capture_poll
void setUp(void)
{
    memset(&engine,0,sizeof engine);engine.renderer=(JceRenderer*)&marker;
    engine.trace_capture=(JceTraceCapture*)&marker;current=true;
    observations=polls=releases=finalizations=next_work=atomics=0;
    SDL_SetAtomicInt(&s_in_render_frame,0);
    jce_trace_set_enabled(true);jce_trace_reset();jce_trace_task_restore_id(0);
}
void tearDown(void) { jce_trace_set_enabled(false); }
static void failed_scope(void)
{
    JceTraceCursor cursor;JceTraceEvent events[3];jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(3,jce_trace_read(&cursor,events,3));
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED,events[2].value_u32[0]);
}
static void test_current_entries_keep_next_stage_and_do_not_finalize(void)
{
    TEST_ASSERT_EQUAL_UINT32(JCE_APP_CONTINUE,actual_main(&engine));
    TEST_ASSERT_EQUAL_UINT32(JCE_APP_CONTINUE,actual_event(&engine));
    TEST_ASSERT_TRUE(actual_watch(&engine));
    TEST_ASSERT_EQUAL_UINT32(JCE_APP_CONTINUE,actual_after_begin(&engine));
    TEST_ASSERT_EQUAL_UINT32(JCE_APP_CONTINUE,actual_after_submit(&engine));
    TEST_ASSERT_EQUAL_UINT32(5,next_work);TEST_ASSERT_EQUAL_UINT32(5,observations);
    TEST_ASSERT_EQUAL_UINT32(0,releases);TEST_ASSERT_EQUAL_UINT32(0,finalizations);
}
static void test_loss_before_main_refuses_all_following_work_and_requests_failure(void)
{
    current=false;TEST_ASSERT_EQUAL_UINT32(JCE_APP_FAILURE,actual_main(&engine));
    TEST_ASSERT_EQUAL_UINT32(0,next_work);TEST_ASSERT_EQUAL_UINT32(1,polls);
    TEST_ASSERT_EQUAL_UINT32(1,releases);failed_scope();
}
static void test_loss_before_event_refuses_user_callback_and_finalizes_failure(void)
{
    current=false;TEST_ASSERT_EQUAL_UINT32(JCE_APP_FAILURE,actual_event(&engine));
    TEST_ASSERT_EQUAL_UINT32(0,next_work);TEST_ASSERT_EQUAL_UINT32(1,finalizations);failed_scope();
}
static void test_lost_resize_watch_never_reaches_reset_or_callback(void)
{
    current=false;TEST_ASSERT_TRUE(actual_watch(&engine));
    TEST_ASSERT_EQUAL_UINT32(0,next_work);TEST_ASSERT_EQUAL_UINT32(0,atomics);
    TEST_ASSERT_EQUAL_UINT32(0,releases); /* Main iteration owns terminal result. */
}
static void test_loss_after_callbacks_or_submit_is_not_completed_render(void)
{
    current=false;TEST_ASSERT_EQUAL_UINT32(JCE_APP_FAILURE,actual_after_begin(&engine));
    TEST_ASSERT_EQUAL_UINT32(JCE_APP_FAILURE,actual_after_submit(&engine));
    TEST_ASSERT_EQUAL_UINT32(0,next_work);TEST_ASSERT_EQUAL_UINT32(2,releases);
    TEST_ASSERT_EQUAL_UINT32(2,atomics);
}
static void test_actual_render_guard_retains_reason_and_raw_interval(void)
{
    jce_engine_perf_hitch_init(&engine.perf_hitch,false,4.0);
    jce_engine_perf_disposition_init(&engine.perf_hitch,true,1000);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&engine.perf_hitch));
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_boundary(&engine.perf_hitch,1000,9,71));
    SDL_SetAtomicInt(&s_in_render_frame,1);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_guard(&engine));
    jce_engine_perf_hitch_end_interval(&engine.perf_hitch,false);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&engine.perf_hitch,1000,13));
    const JceEnginePerfDispositionBucket *b =
        &engine.perf_hitch.disposition.buckets[JCE_ENGINE_DISPOSITION_GUARD];
    TEST_ASSERT_EQUAL_UINT64(1,b->count);TEST_ASSERT_EQUAL_UINT64(4,b->ticks);
    TEST_ASSERT_EQUAL_UINT64(0,b->first.interval);TEST_ASSERT_EQUAL_UINT64(71,b->first.row);
    TEST_ASSERT_EQUAL_UINT64(9,b->first.begin);TEST_ASSERT_EQUAL_UINT64(13,b->last.end);
    TEST_ASSERT_EQUAL_UINT(0,next_work);TEST_ASSERT_EQUAL_UINT(1,releases);
}
int main(void)
{
    UNITY_BEGIN();RUN_TEST(test_current_entries_keep_next_stage_and_do_not_finalize);
    RUN_TEST(test_loss_before_main_refuses_all_following_work_and_requests_failure);
    RUN_TEST(test_loss_before_event_refuses_user_callback_and_finalizes_failure);
    RUN_TEST(test_lost_resize_watch_never_reaches_reset_or_callback);
    RUN_TEST(test_loss_after_callbacks_or_submit_is_not_completed_render);
    RUN_TEST(test_actual_render_guard_retains_reason_and_raw_interval);
    return UNITY_END();
}
