/* Actual complete pipeline + real trace ring; hardware providers controlled. */
#include "unity.h"
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>

static JceGpuTier tier;
static unsigned observer_calls;
static bool observer_reentry_armed;
JceRendererBackend jce_renderer_get_active_backend(void) { return JCE_BACKEND_D3D11; }
JceGpuTier jce_renderer_get_tier(void) { return tier; }
uint32_t jce_renderer_get_caps(void) { return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT; }
JceRenderRecommendation jce_renderer_get_recommendation(void)
{ JceRenderRecommendation r = {0}; r.has_discrete_gpu = true; return r; }
const char *jce_gpu_tier_name(JceGpuTier value) { (void)value; return "controlled"; }
void setUp(void)
{
    jce_render_pipeline_set_observer(NULL,NULL);
    jce_trace_set_enabled(false); jce_trace_reset();
    tier = JCE_GPU_TIER_HIGH; observer_calls = 0; observer_reentry_armed = false;
    jce_render_pipeline_apply(NULL); jce_trace_set_enabled(true);
    jce_trace_thread_register("pipeline-publish");
}
void tearDown(void)
{
    jce_render_pipeline_set_observer(NULL,NULL);
    jce_trace_thread_unregister(); jce_trace_set_enabled(false); jce_trace_reset();
}
static void packets(uint64_t outer, const int64_t *values, unsigned expected_count)
{
    static const char *names[] = {"rp.volfog","rp.csm","rp.ssao","rp.ssr","rp.taa",
        "rp.bloom","rp.mblur","rp.scale_milli","rp.shadow","rp.cascades",
        "rp.shadow_filter","rp.backbuffer_msaa","rp.post_quality","rp.hdr",
        "rp.depth_prepass","rp.ssgi"};
    const unsigned field_count = (unsigned)(sizeof(names) / sizeof(names[0]));
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    JceTraceEvent events[256]; uint32_t n = jce_trace_read(&cursor,events,256);
    unsigned count = 0, counters = 0; uint64_t current = 0;
    for(uint32_t i=0;i<n;++i) {
        const JceTraceEvent *e=&events[i];
        if(!strcmp(e->name,"render_pipeline.publish")) {
            if(e->type==JCE_TRACE_EVENT_TASK_SUBMIT) {
                TEST_ASSERT_EQUAL_UINT64(outer,e->parent_id);
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_SERVICE,e->value_u32[0]);
            } else if(e->type==JCE_TRACE_EVENT_TASK_BEGIN) {
                TEST_ASSERT_EQUAL_UINT64(0,current); current=e->id; counters=0;
                TEST_ASSERT_EQUAL_UINT64(current,e->parent_id);
            } else if(e->type==JCE_TRACE_EVENT_TASK_END) {
                TEST_ASSERT_EQUAL_UINT64(current,e->id);
                TEST_ASSERT_EQUAL_UINT64(current,e->parent_id);
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_SUCCEEDED,e->value_u32[0]);
                TEST_ASSERT_EQUAL_UINT(field_count,counters); ++count; current=0;
            }
        } else if(e->type==JCE_TRACE_EVENT_COUNTER && strncmp(e->name,"rp.",3)==0) {
            TEST_ASSERT_NOT_EQUAL_UINT64(0,current);
            TEST_ASSERT_EQUAL_UINT64(current,e->id); TEST_ASSERT_EQUAL_UINT64(current,e->parent_id);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_CORE,e->category);
            TEST_ASSERT_LESS_THAN_UINT(field_count,counters); TEST_ASSERT_EQUAL_STRING(names[counters],e->name);
            if(values) {
                TEST_ASSERT_LESS_THAN_UINT(expected_count,count);
                TEST_ASSERT_EQUAL_INT64(values[count*field_count+counters],(int64_t)e->value_u64);
            }
            ++counters;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0,current); TEST_ASSERT_EQUAL_UINT64(0,cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT(expected_count,count);
}
static void test_publish_observes_final_clamps_and_restores_parent(void)
{
    const uint64_t outer = jce_trace_next_id();
    jce_trace_task_begin(outer,outer,"outer",0);
    JceRenderPipelineDesc d; jce_render_pipeline_preset_high(&d);
    d.enable_volumetric_fog=true; d.enable_csm=true; d.enable_ssao=true;
    d.enable_ssr=true; d.enable_taa=true; d.enable_bloom=false; d.enable_motion_blur=true;
    d.render_scale=0.75f; d.shadow_resolution=2048; d.csm_cascade_count=4;
    d.shadow_filter_quality=2; d.msaa_samples=4; d.post_quality=JCE_RP_QUALITY_HIGH;
    d.hdr_color=true; d.depth_prepass=true; d.enable_ssgi=true;
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_EQUAL_UINT64(outer,jce_trace_task_current_id());
    tier=JCE_GPU_TIER_LOW; jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_UINT64(outer,jce_trace_task_current_id());
    const int64_t values[]={
        1,1,1,1,1,0,1,750,2048,4,2,4,JCE_RP_QUALITY_HIGH,1,1,1,
        0,1,1,0,0,0,0,750,2048,1,2,4,JCE_RP_QUALITY_LOW,0,0,0,
    };
    packets(outer,values,2);
    jce_trace_task_end(outer,outer,"outer",JCE_TRACE_TASK_SUCCEEDED,0);
    jce_trace_task_restore_id(0);
}
static void observe_reentry(const JceRenderPipelineDesc *desc,void *user)
{
    (void)desc; (void)user;
    ++observer_calls;
    if(observer_reentry_armed) {
        observer_reentry_armed = false;
        JceRenderPipelineDesc d; jce_render_pipeline_preset_low(&d);
        d.enable_volumetric_fog=false; jce_render_pipeline_apply(&d);
    }
}
static void test_reentrant_observer_publishes_newer_policy_last(void)
{
    jce_render_pipeline_set_observer(observe_reentry,NULL);
    /* Registration synchronizes an already-active descriptor immediately.
     * Observe that separate contract before arming the measured publication. */
    TEST_ASSERT_EQUAL_UINT(1,observer_calls); packets(0,NULL,0);
    observer_calls = 0; observer_reentry_armed = true;
    JceRenderPipelineDesc d; jce_render_pipeline_preset_high(&d); d.enable_volumetric_fog=true;
    jce_render_pipeline_apply(&d); TEST_ASSERT_EQUAL_UINT(2,observer_calls);
    packets(0,NULL,2);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor); JceTraceEvent events[256];
    uint32_t n=jce_trace_read(&cursor,events,256); int last=-1;
    for(uint32_t i=0;i<n;++i) if(!strcmp(events[i].name,"rp.volfog")) last=(int)events[i].value_u64;
    TEST_ASSERT_EQUAL_INT(0,last);
}
static void test_disabled_and_queries_do_not_publish(void)
{
    jce_trace_set_enabled(false); jce_render_pipeline_apply(NULL);
    jce_trace_set_enabled(true);
    JceRenderPipelineDesc d; jce_render_pipeline_get(&d); jce_render_pipeline_get_requested(&d);
    for(int i=0;i<100;++i) (void)jce_render_pipeline_is_feature_enabled("volumetric_fog");
    jce_render_pipeline_end_frame(); packets(0,NULL,0);
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(test_publish_observes_final_clamps_and_restores_parent);
    RUN_TEST(test_reentrant_observer_publishes_newer_policy_last);
    RUN_TEST(test_disabled_and_queries_do_not_publish); return UNITY_END();
}
