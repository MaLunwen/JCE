/* Complete actual resource-observation owner and strictly extracted Engine
 * boot/no-draw/normal wires; actual trace ring/encoder, counted public stats.
 * No real Engine boot, process identity, renderer, window or GPU is certified. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_renderer.h>
#include <SDL3/SDL_stdinc.h>
#include <string.h>
#include <stdlib.h>
#ifdef JCE_TRACY_ENABLED
#undef JCE_TRACY_ENABLED
#endif
#define JCE_TRACY_ENABLED 0
#include "application/jce_engine_callbacks.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_async_stats_internal.h"

static uint64_t clock_ns, calling_thread;
static unsigned clock_queries, thread_queries, memory_queries, tag_queries;
static unsigned async_queries, gpu_queries, boot_queries, fatal_calls;
static unsigned begin_calls, draw_calls, end_calls, outside_gpu_queries, fallback_queries;
static bool memory_available, gpu_copy_available, frame_active;
static bool async_available, real_default, shutdown_on_draw;
static bool fallback, begin_accept, no_draw;
static JceMemStats memory_source;
static JceAsyncExecutorStats async_source;
static JceGpuTimingObservation gpu_source;
static uint64_t tag_current, tag_peak;
static int renderer_marker, executor_marker;
static const char *env_enabled, *env_interval;
static JceAppDesc g_app_desc;

static uint64_t counted_clock(void) { ++clock_queries; return clock_ns; }
static uint64_t counted_thread(void) { ++thread_queries; return calling_thread; }
static bool counted_memory(JceMemStats *out)
{ ++memory_queries; *out = memory_source; return memory_available; }
static void counted_tags(uint64_t *current, uint64_t *peak)
{ ++tag_queries; *current = tag_current; *peak = tag_peak; }
static bool counted_async(JceAsyncExecutorStats *out)
{
    ++async_queries;
    if (real_default) return jce_async_default_stats_internal(out);
    *out = async_source;
    return async_available;
}
static bool counted_gpu(JceGpuTimingObservation *out, size_t size)
{
    ++gpu_queries; if (!frame_active) ++outside_gpu_queries;
    TEST_ASSERT_EQUAL_UINT64(sizeof(*out),size);
    *out = gpu_source; return gpu_copy_available;
}
static JceAsyncExecutor *counted_boot_executor(void)
{ ++boot_queries; return real_default ? jce_async_default_executor() : (JceAsyncExecutor*)&executor_marker; }
static char *counted_getenv(const char *name)
{
    if (strcmp(name,"JCE_RESOURCE_TRACE")==0) return (char*)env_enabled;
    TEST_ASSERT_EQUAL_STRING("JCE_RESOURCE_TRACE_MS",name); return (char*)env_interval;
}
static void fatal_msg(const char *format, ...)
{ TEST_ASSERT_EQUAL_STRING("Structured async executor initialization failed",format); ++fatal_calls; }
static bool counted_fallback(const JceRenderer *renderer)
{ (void)renderer; ++fallback_queries; return fallback; }
static void counted_begin(const JceRenderer *renderer, JceWindow *window)
{ (void)window; TEST_ASSERT_EQUAL_PTR(&renderer_marker,renderer); ++begin_calls; frame_active=begin_accept; }
static void counted_end(const JceRenderer *renderer)
{ TEST_ASSERT_EQUAL_PTR(&renderer_marker,renderer); ++end_calls; frame_active=false; }
static void counted_draw(const JceServices *svc, void *user)
{
    (void)svc;(void)user; TEST_ASSERT_TRUE(frame_active); ++draw_calls;
    if (shutdown_on_draw) TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE));
}

#define jce_time_ticks_ns counted_clock
#define jce_thread_current_id counted_thread
#define jce_mem_stats counted_memory
#define jce_mem_profile_get_aggregate_totals counted_tags
#define jce_async_default_stats_internal counted_async
#define jce_renderer_get_gpu_timing_observation counted_gpu
#include JCE_TEST_RESOURCE_OBSERVATION_OWNER
#undef jce_renderer_get_gpu_timing_observation
#undef jce_async_default_stats_internal
#undef jce_mem_profile_get_aggregate_totals
#undef jce_mem_stats
#undef jce_thread_current_id
#undef jce_time_ticks_ns

struct JceEngine {
    JceEngineResourceObservation resource_observation;
    JceRenderer *renderer;
    JceWindow *window;
    JceServices svc;
    bool headless;
};
static struct JceEngine engine;
/* This fixture observes resources in a ready host; cleanup owns admission. */
static bool engine_owner_ready(const struct JceEngine *owner)
{ TEST_ASSERT_EQUAL_PTR(&engine,owner); return true; }
#define getenv counted_getenv
#define jce_async_default_executor counted_boot_executor
#define jce_renderer_is_fallback counted_fallback
#define jce_renderer_begin_frame counted_begin
#define jce_renderer_end_frame counted_end
#include JCE_TEST_RESOURCE_OBSERVATION_ENGINE_WIRES
#undef jce_renderer_end_frame
#undef jce_renderer_begin_frame
#undef jce_renderer_is_fallback
#undef jce_async_default_executor
#undef getenv

static JceTraceEvent events[2048];
static uint32_t event_count;
static void read_events(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    event_count=jce_trace_read(&cursor,events,2048);
    TEST_ASSERT_EQUAL_UINT64(0,cursor.lost_events);
}
static int64_t latest(const char *name)
{
    for(uint32_t i=event_count;i>0;--i)
        if(events[i-1].type==JCE_TRACE_EVENT_COUNTER && strcmp(events[i-1].name,name)==0)
            return (int64_t)events[i-1].value_u64;
    TEST_FAIL_MESSAGE("Expected actual resource counter is absent"); return 0;
}
static uint64_t latest_u64(const char *high, const char *low)
{ return ((uint64_t)latest(high)<<32) | (uint64_t)latest(low); }
static unsigned samples(void)
{
    unsigned count=0;
    for(uint32_t i=0;i<event_count;++i)
        if(events[i].type==JCE_TRACE_EVENT_TASK_SUBMIT && strcmp(events[i].name,"engine.app_resources")==0) ++count;
    return count;
}
static void enable(void)
{ env_enabled="1"; actual_resource_init(&engine); TEST_ASSERT_TRUE(actual_resource_bind(&engine)); }
void setUp(void)
{
    clock_queries=thread_queries=memory_queries=tag_queries=async_queries=gpu_queries=0;
    boot_queries=fatal_calls=begin_calls=draw_calls=end_calls=outside_gpu_queries=fallback_queries=0;
    memset(&engine,0,sizeof engine);engine.renderer=(JceRenderer*)&renderer_marker;
    memset(&memory_source,0,sizeof memory_source); memory_source.current_rss=123;memory_source.current_commit=456;
    memset(&async_source,0,sizeof async_source);async_source.live_tasks=7;async_source.queued_tasks=4;async_source.submitted_tasks=UINT64_MAX;
    memset(&gpu_source,0,sizeof gpu_source);gpu_source.struct_size=sizeof(gpu_source);gpu_source.version=1;
    gpu_source.context_available=gpu_source.backend_available=gpu_source.timer_available=true;
    gpu_source.raw_begin=INT64_MAX-17;gpu_source.raw_end=INT64_MAX;gpu_source.timer_frequency=INT64_MAX;
    gpu_source.context_generation=UINT64_MAX;gpu_source.gpu_source_frame=17;gpu_source.read_host_frame_index=23;
    gpu_source.unavailable_reason=JCE_GPU_TIMING_AVAILABLE;
    memory_available=gpu_copy_available=begin_accept=async_available=true;
    real_default=shutdown_on_draw=false;
    frame_active=fallback=no_draw=false;clock_ns=0;calling_thread=77;tag_current=UINT64_MAX;tag_peak=UINT64_MAX;
    env_enabled=NULL;env_interval=NULL;memset(&g_app_desc,0,sizeof g_app_desc);g_app_desc.draw=counted_draw;
    event_count=0;jce_trace_set_enabled(true);jce_trace_reset();jce_trace_task_restore_id(0);
}
void tearDown(void)
{
    if (real_default) (void)jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE);
    jce_trace_set_enabled(false);
}
static void test_default_off_has_no_clock_stats_events_or_engine_allocations(void)
{
    actual_resource_init(&engine);TEST_ASSERT_TRUE(actual_resource_bind(&engine));
    TEST_ASSERT_FALSE(engine.resource_observation.enabled);TEST_ASSERT_EQUAL_UINT32(0,thread_queries);
    uint64_t allocations,bytes;jce_alloc_frame_delta(&allocations,&bytes);
    for(unsigned i=0;i<5;++i) actual_resource_no_draw(&engine,true);
    jce_alloc_frame_delta(&allocations,&bytes);read_events();
    TEST_ASSERT_EQUAL_UINT64(0,allocations);TEST_ASSERT_EQUAL_UINT64(0,bytes);
    TEST_ASSERT_EQUAL_UINT32(0,clock_queries);TEST_ASSERT_EQUAL_UINT32(0,memory_queries);
    TEST_ASSERT_EQUAL_UINT32(0,tag_queries);TEST_ASSERT_EQUAL_UINT32(0,async_queries);TEST_ASSERT_EQUAL_UINT32(0,gpu_queries);
    TEST_ASSERT_EQUAL_UINT32(0,event_count);TEST_ASSERT_EQUAL_UINT32(1,boot_queries);
    TEST_ASSERT_EQUAL_UINT32(0,fallback_queries);
}
static void test_enabled_but_trace_off_performs_no_observation(void)
{
    enable();const unsigned prior_threads=thread_queries;jce_trace_set_enabled(false);
    actual_resource_no_draw(&engine,true);actual_resource_no_draw(&engine,false);read_events();
    TEST_ASSERT_EQUAL_UINT32(0,fallback_queries);
    TEST_ASSERT_EQUAL_UINT32(prior_threads,thread_queries);TEST_ASSERT_EQUAL_UINT32(0,clock_queries);
    TEST_ASSERT_EQUAL_UINT32(0,memory_queries);TEST_ASSERT_EQUAL_UINT32(0,event_count);
}
static void test_zero_clock_deadline_and_jump_never_catch_up(void)
{
    enable();actual_resource_no_draw(&engine,true);
    clock_ns=999999999;actual_resource_no_draw(&engine,true);TEST_ASSERT_EQUAL_UINT32(1,memory_queries);
    clock_ns=1000000000;actual_resource_no_draw(&engine,true);
    clock_ns=UINT64_C(90000000000);actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_UINT32(3,memory_queries);TEST_ASSERT_EQUAL_UINT32(3,samples());
    TEST_ASSERT_EQUAL_UINT32(1,boot_queries);TEST_ASSERT_EQUAL_UINT32(0,gpu_queries);
}
static void test_backward_clock_rebases_without_unsigned_wrap(void)
{
    enable();clock_ns=UINT64_MAX;actual_resource_no_draw(&engine,true);
    clock_ns=0;actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_UINT32(2,samples());TEST_ASSERT_EQUAL_INT64(1,latest("resource.clock_rollback"));
    clock_ns=999999999;actual_resource_no_draw(&engine,true);TEST_ASSERT_EQUAL_UINT32(2,memory_queries);
}
static void test_current_default_snapshot_never_creates_executor(void)
{
    enable();
    actual_resource_no_draw(&engine,true);clock_ns=1000000000;actual_resource_no_draw(&engine,true);
    TEST_ASSERT_EQUAL_UINT32(1,boot_queries);TEST_ASSERT_EQUAL_UINT32(2,async_queries);
    async_available=false;clock_ns=2000000000;actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_UINT32(3,async_queries);TEST_ASSERT_EQUAL_UINT32(1,boot_queries);
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.async.available"));
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.async.live_tasks"));
}
static void test_no_draw_headless_and_fallback_keep_cpu_but_never_query_gpu(void)
{
    enable();no_draw=true;actual_resource_no_draw(&engine,no_draw);
    engine.headless=true;clock_ns=1000000000;actual_resource_no_draw(&engine,false);
    engine.headless=false;fallback=true;clock_ns=2000000000;actual_resource_no_draw(&engine,false);read_events();
    TEST_ASSERT_EQUAL_UINT32(3,samples());TEST_ASSERT_EQUAL_UINT32(3,memory_queries);
    TEST_ASSERT_EQUAL_UINT32(0,gpu_queries);TEST_ASSERT_EQUAL_UINT32(0,begin_calls);
    TEST_ASSERT_EQUAL_UINT32(0,draw_calls);TEST_ASSERT_EQUAL_UINT32(0,end_calls);
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.query_allowed"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.timer_available"));
}
static void test_normal_wire_reads_gpu_after_draw_before_end_and_restoration_waits_cadence(void)
{
    enable();actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_UINT32(1,begin_calls);TEST_ASSERT_EQUAL_UINT32(1,draw_calls);TEST_ASSERT_EQUAL_UINT32(1,end_calls);
    TEST_ASSERT_EQUAL_UINT32(1,gpu_queries);TEST_ASSERT_EQUAL_UINT32(0,outside_gpu_queries);
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.gpu.timer_available"));TEST_ASSERT_FALSE(frame_active);
    clock_ns=1000000000;actual_resource_no_draw(&engine,true);read_events();TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.timer_available"));
    clock_ns=1500000000;actual_resource_normal(&engine);TEST_ASSERT_EQUAL_UINT32(1,gpu_queries);
    clock_ns=2000000000;actual_resource_normal(&engine);read_events();TEST_ASSERT_EQUAL_UINT32(2,gpu_queries);
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.gpu.timer_available"));TEST_ASSERT_EQUAL_UINT32(3,samples());
}
static void test_failed_memory_copy_and_zero_values_are_explicit_unknown(void)
{
    enable();actual_resource_no_draw(&engine,true);
    clock_ns=1000000000;memory_available=false;memory_source.current_rss=(size_t)-1;actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.memory.copied"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.memory.rss_available"));
    TEST_ASSERT_EQUAL_UINT64(0,latest_u64("resource.memory.rss_bytes.hi32","resource.memory.rss_bytes.lo32"));
    clock_ns=2000000000;memory_available=true;memory_source.current_rss=memory_source.current_commit=0;actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.memory.copied"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.memory.commit_available"));
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.tagged.partial_scope"));
}
static void test_context_missing_and_invalid_snapshot_never_inherit_timer_availability(void)
{
    enable();actual_resource_normal(&engine);
    clock_ns=1000000000;gpu_source.context_available=false;gpu_source.backend_available=false;gpu_source.timer_available=false;
    gpu_source.unavailable_reason=JCE_GPU_TIMING_CONTEXT_UNAVAILABLE;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.gpu.copied"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.timer_available"));
    TEST_ASSERT_EQUAL_UINT64(0,latest_u64("resource.gpu.raw_begin.hi32","resource.gpu.raw_begin.lo32"));
    clock_ns=2000000000;gpu_copy_available=false;gpu_source.timer_available=true;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.copied"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.timer_available"));
    clock_ns=3000000000;gpu_copy_available=true;gpu_source.version=99;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.copied"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.reason_available"));
}
static void test_all_wide_values_and_signed_gpu_bits_survive_exact_trace_halves(void)
{
    enable();clock_ns=UINT64_MAX;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,latest_u64("resource.sample_ns.hi32","resource.sample_ns.lo32"));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,latest_u64("resource.tagged.current_bytes.hi32","resource.tagged.current_bytes.lo32"));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,latest_u64("resource.async.submitted_tasks.hi32","resource.async.submitted_tasks.lo32"));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(INT64_MAX-17),latest_u64("resource.gpu.raw_begin.hi32","resource.gpu.raw_begin.lo32"));
    TEST_ASSERT_EQUAL_UINT64(INT64_MAX,latest_u64("resource.gpu.frequency.hi32","resource.gpu.frequency.lo32"));
    gpu_source.raw_begin=INT64_MIN;gpu_source.raw_end=-1;gpu_source.timer_frequency=-2;gpu_source.timer_available=false;
    clock_ns=0;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_UINT64((uint64_t)INT64_MIN,latest_u64("resource.gpu.raw_begin.hi32","resource.gpu.raw_begin.lo32"));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,latest_u64("resource.gpu.raw_end.hi32","resource.gpu.raw_end.lo32"));
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.gpu.timer_available"));
    for(uint32_t i=0;i<event_count;++i)if(events[i].type==JCE_TRACE_EVENT_COUNTER)
        TEST_ASSERT_TRUE(events[i].value_u64<=UINT32_MAX);
    TraceJsonBuffer json={0};bool first=true;
    TEST_ASSERT_TRUE(jce_trace_chrome_encode_batch_into(events,event_count,&first,&json));
    const bool maximum_half=strstr(json.data,"\"value\":4294967295")!=NULL;
    const bool signed_minimum_half=strstr(json.data,"\"value\":2147483648")!=NULL;
    jce_allocator_t allocator=jce_allocator_default();JCE_AFREE(allocator,json.data);
    TEST_ASSERT_TRUE(maximum_half);TEST_ASSERT_TRUE(signed_minimum_half);
}
static void test_owner_thread_guard_and_counter_parent_belong_to_same_sample(void)
{
    enable();calling_thread=78;actual_resource_no_draw(&engine,true);read_events();
    TEST_ASSERT_EQUAL_UINT32(0,clock_queries);TEST_ASSERT_EQUAL_UINT32(0,event_count);
    calling_thread=77;jce_trace_task_restore_id(321);actual_resource_no_draw(&engine,true);read_events();
    uint64_t id=0;
    for(uint32_t i=0;i<event_count;++i){
        if(events[i].type==JCE_TRACE_EVENT_TASK_SUBMIT){id=events[i].id;TEST_ASSERT_EQUAL_UINT64(321,events[i].parent_id);}
        if(events[i].type==JCE_TRACE_EVENT_COUNTER){TEST_ASSERT_EQUAL_UINT64(id,events[i].id);TEST_ASSERT_EQUAL_UINT64(321,events[i].parent_id);}
    }
    TEST_ASSERT_TRUE(id!=0);TEST_ASSERT_EQUAL_UINT64(77,latest_u64("resource.owner_thread_id.hi32","resource.owner_thread_id.lo32"));
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.pid_available"));TEST_ASSERT_EQUAL_INT64(0,latest("resource.host_role_available"));
    jce_trace_task_restore_id(0);
}
static void test_zero_owner_normal_wire_preserves_raw_owner_and_native_trace_identity(void)
{
    calling_thread=0;enable();
    TEST_ASSERT_EQUAL_UINT64(0,engine.resource_observation.owner_thread);
    actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_UINT32(1,samples());TEST_ASSERT_EQUAL_UINT32(1,memory_queries);
    TEST_ASSERT_EQUAL_UINT32(1,gpu_queries);TEST_ASSERT_EQUAL_UINT32(0,outside_gpu_queries);
    TEST_ASSERT_EQUAL_UINT64(0,latest_u64("resource.owner_thread_id.hi32","resource.owner_thread_id.lo32"));
    /* The header's thread port models SDL's zero owner. The real native trace
     * producer remains untouched; no replacement native TID is invented. */
    const uint64_t native_thread=jce_thread_current_id();
    for(uint32_t i=0;i<event_count;++i)TEST_ASSERT_EQUAL_UINT64(native_thread,events[i].thread_id);
    calling_thread=1;clock_ns=1000000000;actual_resource_normal(&engine);
    TEST_ASSERT_EQUAL_UINT32(1,memory_queries);TEST_ASSERT_EQUAL_UINT32(1,gpu_queries);
    TEST_ASSERT_EQUAL_UINT32(1,clock_queries);
}
static void test_actual_chrome_encoder_preserves_explicit_zero_thread_packet(void)
{
    /* An explicit encoder input port, not an altered native producer event. */
    const JceTraceEvent packet={.sequence=1,.type=JCE_TRACE_EVENT_COUNTER,
        .category=JCE_TRACE_CATEGORY_CORE,.timestamp_ns=1,.thread_id=0,.id=1,
        .name="resource.owner_thread_id.lo32"};
    TraceJsonBuffer json={0};bool first=true;
    const bool encoded=jce_trace_chrome_encode_batch_into(&packet,1,&first,&json);
    const bool zero_thread=json.data && strstr(json.data,"\"tid\":0")!=NULL;
    const bool zero_value=json.data && strstr(json.data,"\"value\":0")!=NULL;
    jce_allocator_t allocator=jce_allocator_default();JCE_AFREE(allocator,json.data);
    TEST_ASSERT_TRUE(encoded);TEST_ASSERT_TRUE(zero_thread);TEST_ASSERT_TRUE(zero_value);
}
static void test_interval_clamps_and_normal_path_is_not_preconsumed_by_no_draw_wire(void)
{
    env_enabled="1";env_interval="1";actual_resource_init(&engine);TEST_ASSERT_EQUAL_UINT32(250,engine.resource_observation.interval_ms);
    env_interval="900000";actual_resource_init(&engine);TEST_ASSERT_EQUAL_UINT32(60000,engine.resource_observation.interval_ms);
    env_interval="-1";actual_resource_init(&engine);TEST_ASSERT_EQUAL_UINT32(1000,engine.resource_observation.interval_ms);
    env_interval=NULL;actual_resource_init(&engine);TEST_ASSERT_TRUE(actual_resource_bind(&engine));
    actual_resource_no_draw(&engine,false);TEST_ASSERT_EQUAL_UINT32(0,memory_queries);
    actual_resource_normal(&engine);TEST_ASSERT_EQUAL_UINT32(1,gpu_queries);read_events();TEST_ASSERT_EQUAL_UINT32(1,samples());
}
static void test_public_draw_shutdown_and_recreation_observe_current_default_safely(void)
{
    real_default=true;shutdown_on_draw=true;enable();
    actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_INT64(0,latest("resource.async.available"));
    TEST_ASSERT_EQUAL_UINT32(1,boot_queries);TEST_ASSERT_EQUAL_UINT32(1,async_queries);
    shutdown_on_draw=false;
    TEST_ASSERT_NOT_NULL(jce_async_default_executor());
    clock_ns=1000000000;actual_resource_normal(&engine);read_events();
    TEST_ASSERT_EQUAL_INT64(1,latest("resource.async.available"));
    TEST_ASSERT_EQUAL_UINT32(1,boot_queries);TEST_ASSERT_EQUAL_UINT32(2,async_queries);
    TEST_ASSERT_EQUAL_UINT32(0,outside_gpu_queries);
}
int main(void)
{
    jce_thread_mark_main();
    UNITY_BEGIN();
    RUN_TEST(test_default_off_has_no_clock_stats_events_or_engine_allocations);
    RUN_TEST(test_enabled_but_trace_off_performs_no_observation);
    RUN_TEST(test_zero_clock_deadline_and_jump_never_catch_up);
    RUN_TEST(test_backward_clock_rebases_without_unsigned_wrap);
    RUN_TEST(test_current_default_snapshot_never_creates_executor);
    RUN_TEST(test_public_draw_shutdown_and_recreation_observe_current_default_safely);
    RUN_TEST(test_no_draw_headless_and_fallback_keep_cpu_but_never_query_gpu);
    RUN_TEST(test_normal_wire_reads_gpu_after_draw_before_end_and_restoration_waits_cadence);
    RUN_TEST(test_failed_memory_copy_and_zero_values_are_explicit_unknown);
    RUN_TEST(test_context_missing_and_invalid_snapshot_never_inherit_timer_availability);
    RUN_TEST(test_all_wide_values_and_signed_gpu_bits_survive_exact_trace_halves);
    RUN_TEST(test_owner_thread_guard_and_counter_parent_belong_to_same_sample);
    RUN_TEST(test_zero_owner_normal_wire_preserves_raw_owner_and_native_trace_identity);
    RUN_TEST(test_actual_chrome_encoder_preserves_explicit_zero_thread_packet);
    RUN_TEST(test_interval_clamps_and_normal_path_is_not_preconsumed_by_no_draw_wire);
    return UNITY_END();
}
