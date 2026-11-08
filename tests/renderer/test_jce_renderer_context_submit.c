/* Complete actual renderer admission/central-submit bodies with typed CPU
 * backend ports. These inspect host work/ordinals, never GPU completion. */
#include "unity.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_renderer_caps_internal.h"
#include "renderer/jce_instance_upload.h"
#include "os/core/jce_trace_scope_internal.h"
#include "renderer/jce_renderer_submission_trace_internal.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <bgfx/c99/bgfx.h>
#include <string.h>
struct JceRenderer {
    bool is_fallback,headless;
    JceInstanceUploadOwner instance_upload;
    JceSubmittedPayloadStats submitted_payload;
};
static struct { uint32_t frame_index,offscreen_submission_index; } s_device;
static bool current;
static unsigned observations,frames,retirements,uploads,readback_collections;
static JceRenderer renderer;
bool jce_renderer_caps_api_web_context_current(void) { ++observations;return current; }
static uint32_t counted_frame(bool capture)
{ TEST_ASSERT_FALSE(capture);++frames;return s_device.frame_index+1u; }
static void counted_retirement(const JceRenderer *r,uint32_t ordinal)
{ TEST_ASSERT_EQUAL_PTR(&renderer,r);TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);++retirements; }
static void counted_upload(JceInstanceUploadOwner *owner,uint32_t ordinal)
{ TEST_ASSERT_EQUAL_PTR(&renderer.instance_upload,owner);TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);++uploads; }
static void counted_readback(const JceRenderer *r,uint32_t frame)
{ TEST_ASSERT_EQUAL_PTR(&renderer,r);TEST_ASSERT_EQUAL_UINT32(s_device.frame_index,frame);++readback_collections; }
#define bgfx_frame counted_frame
#define jce_offscreen_retirement_collect counted_retirement
#define jce_instance_upload_owner_advance counted_upload
#define jce_render_readback_collect counted_readback
#include JCE_TEST_RENDERER_CONTEXT_SUBMIT_OWNER
#undef bgfx_frame
#undef jce_offscreen_retirement_collect
#undef jce_instance_upload_owner_advance
#undef jce_render_readback_collect
void setUp(void)
{
    memset(&renderer,0,sizeof renderer);memset(&s_device,0,sizeof s_device);
    observations=frames=retirements=uploads=readback_collections=0;current=true;
    jce_trace_set_enabled(true);jce_trace_reset();jce_trace_task_restore_id(0);
    jce_perf_phase_set_enabled(false);
}
void tearDown(void) { jce_trace_set_enabled(false);TEST_ASSERT_EQUAL_UINT(frames,readback_collections); }
static void test_current_submission_advances_one_owner_and_consumers(void)
{
    TEST_ASSERT_EQUAL_UINT32(1,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(1,frames);TEST_ASSERT_EQUAL_UINT32(1,retirements);
    TEST_ASSERT_EQUAL_UINT32(1,uploads);TEST_ASSERT_EQUAL_UINT32(1,s_device.offscreen_submission_index);
    TEST_ASSERT_EQUAL_UINT32(1,observations);
}
static void test_loss_refuses_driver_and_all_ordinal_consumers(void)
{
    current=false;s_device.frame_index=17;s_device.offscreen_submission_index=29;
    TEST_ASSERT_EQUAL_UINT32(17,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(0,frames);TEST_ASSERT_EQUAL_UINT32(0,retirements);
    TEST_ASSERT_EQUAL_UINT32(0,uploads);TEST_ASSERT_EQUAL_UINT32(29,s_device.offscreen_submission_index);
    JceTraceCursor cursor;JceTraceEvent events[3];jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(3,jce_trace_read(&cursor,events,3));
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED,events[2].value_u32[0]);
    TEST_ASSERT_EQUAL_STRING("renderer.submit_refused",events[0].name);
}
static void test_null_and_fallback_do_not_call_device_or_consume_context(void)
{
    TEST_ASSERT_EQUAL_UINT32(0,jce_renderer_submit_frame(NULL));
    TEST_ASSERT_EQUAL_UINT32(0,observations);
    renderer.is_fallback=true;
    TEST_ASSERT_TRUE(jce_renderer_frame_admission_current_internal(&renderer));
    TEST_ASSERT_EQUAL_UINT32(0,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(0,observations);TEST_ASSERT_EQUAL_UINT32(0,frames);
}
static void test_headless_keeps_noop_cpu_submission_policy(void)
{
    current=false;renderer.headless=true;
    TEST_ASSERT_TRUE(jce_renderer_frame_admission_current_internal(&renderer));
    TEST_ASSERT_EQUAL_UINT32(1,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(0,observations);TEST_ASSERT_EQUAL_UINT32(1,frames);
}
static void test_refusal_does_not_fabricate_wrapped_submission_progress(void)
{
    s_device.frame_index=UINT32_MAX;s_device.offscreen_submission_index=UINT32_MAX;
    current=false;TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,s_device.offscreen_submission_index);
    current=true;TEST_ASSERT_EQUAL_UINT32(0,jce_renderer_submit_frame(&renderer));
    TEST_ASSERT_EQUAL_UINT32(0,s_device.offscreen_submission_index);
    TEST_ASSERT_EQUAL_UINT32(1,frames);TEST_ASSERT_EQUAL_UINT32(1,uploads);
}
int main(void)
{
    UNITY_BEGIN();RUN_TEST(test_current_submission_advances_one_owner_and_consumers);
    RUN_TEST(test_loss_refuses_driver_and_all_ordinal_consumers);
    RUN_TEST(test_null_and_fallback_do_not_call_device_or_consume_context);
    RUN_TEST(test_headless_keeps_noop_cpu_submission_policy);
    RUN_TEST(test_refusal_does_not_fabricate_wrapped_submission_progress);
    return UNITY_END();
}
