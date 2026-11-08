/* Complete GPU stats owner and actual device-context getter body. Typed
 * bgfx ports run no device and cannot certify physical GPU timings/fences. */
#include "unity.h"
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <bgfx/c99/bgfx.h>
#include "renderer/jce_gpu_caps.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_renderer_caps_internal.h"
#include "renderer/jce_upload_metrics.h"
#include <string.h>
#include <stdint.h>
static struct { JceUploadMetricsBridge payload; uint32_t frame_index; } s_device;
static JceSubmittedPayloadStats payload;
static bgfx_stats_t stats;
static bool no_stats,web_current;
static unsigned stats_queries;
static bool controlled_web_current(void) {return web_current;}
static const bgfx_stats_t *controlled_stats(void) {++stats_queries;return no_stats ? NULL : &stats;}
static const bgfx_caps_t *controlled_caps(void) {return NULL;}
static const char *controlled_backend_name(void) {return "CPU port";}
#define jce_renderer_caps_api_web_context_current controlled_web_current
#include JCE_TEST_GPU_CONTEXT_OWNER
#define bgfx_get_stats controlled_stats
#define bgfx_get_caps controlled_caps
#define jce_renderer_running_backend_name controlled_backend_name
#include JCE_TEST_GPU_CAPS_OWNER
#undef bgfx_get_stats
#undef bgfx_get_caps
#undef jce_renderer_running_backend_name
#undef jce_renderer_caps_api_web_context_current
void setUp(void)
{
    memset(&s_device,0,sizeof(s_device));memset(&payload,0,sizeof(payload));memset(&stats,0,sizeof(stats));
    payload.valid=true;payload.context_generation=9;s_device.payload.active=&payload;s_device.frame_index=24;
    stats.gpuTimerFreq=1000;stats.gpuTimeBegin=5;stats.gpuTimeEnd=8;stats.gpuFrameNum=14;
    no_stats=false;web_current=true;stats_queries=0;
}
void tearDown(void) {}
static void test_null_rejects_without_backend_query(void)
{TEST_ASSERT_FALSE(jce_renderer_get_gpu_timing_observation(NULL,sizeof(JceGpuTimingObservation)));TEST_ASSERT_EQUAL_UINT32(0,stats_queries);}
static void test_all_undersizes_preserve_storage_and_skip_backend(void)
{
    JceGpuTimingObservation out,original;memset(&original,0xa5,sizeof(original));
    for(size_t n=0;n<sizeof(out);++n) {
        out=original;TEST_ASSERT_FALSE(jce_renderer_get_gpu_timing_observation(&out,n));
        TEST_ASSERT_EQUAL_MEMORY(&original,&out,sizeof(out));
    }
    TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
}
static void test_oversize_writes_only_current_struct(void)
{
    struct {JceGpuTimingObservation out;unsigned char canary[32];} buffer;
    unsigned char expected[32];memset(expected,0xa5,sizeof(expected));memset(&buffer,0xa5,sizeof(buffer));
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&buffer.out,sizeof(buffer)));
    TEST_ASSERT_EQUAL_MEMORY(expected,buffer.canary,sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(sizeof(buffer.out),buffer.out.struct_size);TEST_ASSERT_EQUAL_UINT32(1,buffer.out.version);
}
static void test_no_context_skips_backend_and_reports_reason(void)
{
    JceGpuTimingObservation out;s_device.payload.active=NULL;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_FALSE(out.context_available);TEST_ASSERT_FALSE(out.backend_available);TEST_ASSERT_FALSE(out.timer_available);
    TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_CONTEXT_UNAVAILABLE,out.unavailable_reason);TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
}
static void test_backend_missing_preserves_read_identity(void)
{
    JceGpuTimingObservation out;no_stats=true;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_TRUE(out.context_available);TEST_ASSERT_FALSE(out.backend_available);TEST_ASSERT_FALSE(out.timer_available);
    TEST_ASSERT_EQUAL_UINT64(9,out.context_generation);TEST_ASSERT_EQUAL_UINT32(24,out.read_host_frame_index);
    TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_BACKEND_UNAVAILABLE,out.unavailable_reason);TEST_ASSERT_EQUAL_UINT32(1,stats_queries);
}
static void test_frequency_zero_negative_keep_raw_unavailable(void)
{
    for(int64_t frequency=0;frequency>=-1;--frequency) {
        JceGpuTimingObservation out;stats.gpuTimerFreq=frequency;
        TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
        TEST_ASSERT_TRUE(out.backend_available);TEST_ASSERT_FALSE(out.timer_available);
        TEST_ASSERT_EQUAL_INT64(frequency,out.timer_frequency);TEST_ASSERT_EQUAL_INT64(5,out.raw_begin);TEST_ASSERT_EQUAL_INT64(8,out.raw_end);
        TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE,out.unavailable_reason);TEST_ASSERT_TRUE(out.duration_ms==0);
    }
}
static void test_nonforward_negative_ticks_are_unavailable(void)
{
    const int64_t values[][2]={{0,0},{2,2},{8,5},{-1,8},{-2,-1}};
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        JceGpuTimingObservation out;stats.gpuTimeBegin=values[i][0];stats.gpuTimeEnd=values[i][1];
        TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
        TEST_ASSERT_FALSE(out.timer_available);TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_INTERVAL_UNAVAILABLE,out.unavailable_reason);
        TEST_ASSERT_EQUAL_INT64(values[i][0],out.raw_begin);TEST_ASSERT_EQUAL_INT64(values[i][1],out.raw_end);
    }
}
static void test_forward_ticks_convert_actual_interval(void)
{
    JceGpuTimingObservation out;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_TRUE(out.timer_available);TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_AVAILABLE,out.unavailable_reason);
    TEST_ASSERT_EQUAL_INT64(1000,out.timer_frequency);TEST_ASSERT_EQUAL_INT64(5,out.raw_begin);TEST_ASSERT_EQUAL_INT64(8,out.raw_end);
    TEST_ASSERT_TRUE(out.duration_ms==3);TEST_ASSERT_EQUAL_UINT32(14,out.gpu_source_frame);
    TEST_ASSERT_EQUAL_UINT32(24,out.read_host_frame_index);TEST_ASSERT_EQUAL_UINT64(9,out.context_generation);
}
static void test_adjacent_big_ticks_do_not_round_interval_to_zero(void)
{
    JceGpuTimingObservation out;stats.gpuTimeBegin=INT64_C(9007199254740992);stats.gpuTimeEnd=stats.gpuTimeBegin+1;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_TRUE(out.timer_available);TEST_ASSERT_TRUE(out.duration_ms==1);
}
static void test_repeated_latched_sample_has_separate_read_identity(void)
{
    JceGpuTimingObservation before,after;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&before,sizeof(before)));s_device.frame_index=25;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&after,sizeof(after)));
    TEST_ASSERT_EQUAL_UINT32(before.gpu_source_frame,after.gpu_source_frame);TEST_ASSERT_EQUAL_INT64(before.raw_begin,after.raw_begin);
    TEST_ASSERT_EQUAL_UINT32(24,before.read_host_frame_index);TEST_ASSERT_EQUAL_UINT32(25,after.read_host_frame_index);
}
static void test_unbound_teardown_does_not_touch_old_backend(void)
{
    JceGpuTimingObservation out;s_device.payload.active=NULL;payload.valid=false;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_FALSE(out.context_available);TEST_ASSERT_EQUAL_UINT64(0,out.context_generation);TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
}
static void test_context_loss_refuses_bound_but_stale_identity(void)
{
    JceGpuTimingObservation out;web_current=false;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_timing_observation(&out,sizeof(out)));
    TEST_ASSERT_FALSE(out.context_available);TEST_ASSERT_FALSE(out.timer_available);
    TEST_ASSERT_EQUAL_INT(JCE_GPU_TIMING_CONTEXT_UNAVAILABLE,out.unavailable_reason);TEST_ASSERT_EQUAL_UINT64(0,out.context_generation);
    TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
}
static void test_legacy_stats_inactive_lifetimes_skip_backend_and_zero_output(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        payload.valid = true; payload.context_generation = 9;
        s_device.payload.active = &payload; web_current = true; stats_queries = 0;
        if (mode == 0) s_device.payload.active = NULL; // Before bind.
        if (mode == 1) payload.valid = false;
        if (mode == 2) payload.context_generation = 0;
        if (mode == 3) { s_device.payload.active = NULL; payload.valid = false; } // After unbind.
        if (mode == 4) web_current = false;
        JceGpuStats out; unsigned char zero[sizeof(out)];
        memset(&out,0xa5,sizeof(out)); memset(zero,0,sizeof(zero));
        bool available = jce_renderer_get_gpu_stats(&out);
        TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
        TEST_ASSERT_FALSE(available); TEST_ASSERT_EQUAL_MEMORY(zero,&out,sizeof(out));
    }
}
static void test_legacy_stats_bound_noop_remains_valid_with_zero_timers(void)
{
    JceGpuStats out; memset(&stats,0,sizeof(stats));
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&out));
    TEST_ASSERT_TRUE(out.valid); TEST_ASSERT_EQUAL_UINT32(1,stats_queries);
    TEST_ASSERT_TRUE(out.gpu_ms==0); TEST_ASSERT_TRUE(out.cpu_frame_ms==0);
    TEST_ASSERT_EQUAL_UINT32(0,out.num_draw); TEST_ASSERT_EQUAL_UINT32(0,out.num_textures);
    TEST_ASSERT_EQUAL_UINT32(24,s_device.frame_index);
}
static void test_legacy_stats_active_normal_fields_and_missing_backend_are_preserved(void)
{
    JceGpuStats out; stats.cpuTimerFreq=1000;stats.cpuTimeFrame=5;
    stats.cpuTimeBegin=100;stats.cpuTimeEnd=104;stats.waitSubmit=1;stats.waitRender=2;
    stats.numDraw=12;stats.numCompute=3;stats.numBlit=2;stats.width=2560;stats.height=1600;
    stats.numTextures=4;stats.numFrameBuffers=2;stats.numPrograms=3;stats.numShaders=6;
    stats.rtMemoryUsed=32;stats.textureMemoryUsed=64;stats.gpuMemoryUsed=128;stats.gpuMemoryMax=1024;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&out));TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_EQUAL_UINT32(1,stats_queries);TEST_ASSERT_TRUE(out.cpu_frame_ms==5);
    TEST_ASSERT_TRUE(out.cpu_submit_ms==4);TEST_ASSERT_TRUE(out.gpu_ms==3);
    TEST_ASSERT_TRUE(out.wait_submit_ms==1);TEST_ASSERT_TRUE(out.wait_render_ms==2);
    TEST_ASSERT_EQUAL_UINT32(12,out.num_draw);TEST_ASSERT_EQUAL_UINT32(3,out.num_compute);
    TEST_ASSERT_EQUAL_UINT32(2,out.num_blit);TEST_ASSERT_EQUAL_UINT32(2560,out.backbuffer_width);
    TEST_ASSERT_EQUAL_UINT32(1600,out.backbuffer_height);TEST_ASSERT_EQUAL_UINT32(4,out.num_textures);
    TEST_ASSERT_EQUAL_UINT32(2,out.num_frame_buffers);TEST_ASSERT_EQUAL_UINT32(3,out.num_programs);
    TEST_ASSERT_EQUAL_UINT32(6,out.num_shaders);TEST_ASSERT_EQUAL_INT64(32,out.rt_memory_used);
    TEST_ASSERT_EQUAL_INT64(64,out.texture_memory_used);TEST_ASSERT_EQUAL_INT64(128,out.gpu_memory_used);
    TEST_ASSERT_EQUAL_INT64(1024,out.gpu_memory_max);TEST_ASSERT_EQUAL_UINT32(24,s_device.frame_index);
    no_stats=true;stats_queries=0;unsigned char zero[sizeof(out)];memset(zero,0,sizeof(zero));
    memset(&out,0xa5,sizeof(out));TEST_ASSERT_FALSE(jce_renderer_get_gpu_stats(&out));
    TEST_ASSERT_EQUAL_UINT32(1,stats_queries);TEST_ASSERT_EQUAL_MEMORY(zero,&out,sizeof(out));
}
static void test_legacy_stats_null_skips_backend(void)
{
    TEST_ASSERT_FALSE(jce_renderer_get_gpu_stats(NULL));TEST_ASSERT_EQUAL_UINT32(0,stats_queries);
}

int main(void)
{
    UNITY_BEGIN();RUN_TEST(test_null_rejects_without_backend_query);RUN_TEST(test_all_undersizes_preserve_storage_and_skip_backend);
    RUN_TEST(test_oversize_writes_only_current_struct);RUN_TEST(test_no_context_skips_backend_and_reports_reason);
    RUN_TEST(test_backend_missing_preserves_read_identity);RUN_TEST(test_frequency_zero_negative_keep_raw_unavailable);
    RUN_TEST(test_nonforward_negative_ticks_are_unavailable);RUN_TEST(test_forward_ticks_convert_actual_interval);
    RUN_TEST(test_adjacent_big_ticks_do_not_round_interval_to_zero);RUN_TEST(test_repeated_latched_sample_has_separate_read_identity);
    RUN_TEST(test_unbound_teardown_does_not_touch_old_backend);RUN_TEST(test_context_loss_refuses_bound_but_stale_identity);
    RUN_TEST(test_legacy_stats_inactive_lifetimes_skip_backend_and_zero_output);
    RUN_TEST(test_legacy_stats_bound_noop_remains_valid_with_zero_timers);
    RUN_TEST(test_legacy_stats_active_normal_fields_and_missing_backend_are_preserved);
    RUN_TEST(test_legacy_stats_null_skips_backend);
    return UNITY_END();
}
