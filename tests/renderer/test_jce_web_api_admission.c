/* Actual private policy + real public API-version authority. The real renderer
 * post-init decision/cleanup tail is extracted verbatim; native ports count it. */
#include "unity.h"
#include <string.h>
#ifndef JCE_WEB_API_OWNER
#define JCE_WEB_API_OWNER "renderer/jce_renderer_web_api_internal.h"
#endif
#include JCE_WEB_API_OWNER
#include "renderer/jce_renderer_bgfx_callback.h"
#include "renderer/jce_renderer_launch_policy_internal.h"
#include <bgfx/c99/bgfx.h>
static JceWebGlContextSnapshot observed;
static bool available;
static unsigned reads, shutdowns, joins, releases, active_accepts;
static JceRendererWebApiState state;
static bool read_context(JceWebGlContextSnapshot *out){++reads;*out=observed;return available;}
static bool actual_web_admit(void){return jce_renderer_web_api_admit(&state,read_context);}
static JceRendererBackend from_bgfx_type(JceRendererBackend backend){return backend;}
static bool active_accept(void){++active_accepts;return true;}
static void shutdown_port(void){++shutdowns;}
static void join_port(void *owner,int reason){(void)owner;(void)reason;++joins;}
static void release_port(void){++releases;}
/* Capability terminal publication is tested by the complete callback/attempt
 * fixtures. Keep this actual-tail dependency typed and isolated; this fixture
 * owns Web API admission, shutdown/join and context cleanup assertions. */
static void attempt_end_port(JceBgfxCpuProfilerAttemptResult result){(void)result;}
/* Full attempt fixtures own startup diagnostic forwarding and observations. */
static void launch_observer_port(const JceRendererLaunchDiagnostics *request,
    const bgfx_init_t *init, unsigned phase)
{ (void)request; (void)init; (void)phase; }
static struct {uint32_t frame_index,offscreen_submission_index;int allocation_trace;} s_device;
#define LOG_TAG "web_api_fixture"
#define JCE_BGFX_ALLOC_SUMMARY_JOINED 1
#define jce_renderer_caps_api_admit_web_context actual_web_admit
#define jce_renderer_caps_api_accept_active_backend active_accept
#define bgfx_shutdown shutdown_port
#define jce_bgfx_trace_end join_port
#define gl_ladder_release release_port
#define jce_rcb_cpu_profiler_attempt_end attempt_end_port
#define renderer_launch_diagnostics_observe launch_observer_port
static bool actual_post_init(JceRendererBackend type)
{
    JceRendererApiInfo api;
    /* This admission-only caller does not request optional presentation
     * outputs. Keep their real types for the unchanged actual success tail;
     * complete init-attempt fixtures own their publication assertions. */
    JceRendererLaunchVsync candidate = {0};
    JceRendererLaunchVsync *presentation = NULL;
    uint32_t *effective_reset_flags = NULL;
    bgfx_init_t init = {0};
    const JceRendererLaunchDiagnostics *launch = NULL;
#include JCE_WEB_API_INIT_TAIL
}
#undef jce_renderer_caps_api_admit_web_context
#undef jce_renderer_caps_api_accept_active_backend
#undef bgfx_shutdown
#undef jce_bgfx_trace_end
#undef gl_ladder_release
#undef jce_rcb_cpu_profiler_attempt_end
#undef renderer_launch_diagnostics_observe
void setUp(void)
{
    memset(&state,0,sizeof(state));memset(&observed,0,sizeof(observed));
    observed.context_id=12;observed.epoch_begin=observed.epoch_end=5;observed.webgl_major=2;
    strcpy(observed.api,"WebGL 2.0 (OpenGL ES 3.2 fixture)");
    strcpy(observed.shading,"WebGL GLSL ES 3.20 fixture");strcpy(observed.renderer,"actual provider port");
    available=true;reads=shutdowns=joins=releases=active_accepts=0;
    jce_renderer_caps_api_reset();jce_trace_set_enabled(false);jce_trace_task_restore_id(0);
}
void tearDown(void){jce_renderer_caps_api_reset();jce_trace_set_enabled(false);jce_trace_task_restore_id(0);}
static void cleared(void)
{
    JceRendererApiInfo info=jce_renderer_get_api_info();
    TEST_ASSERT_FALSE(info.runtime_version_verified);TEST_ASSERT_FALSE(info.shader_language_version_verified);
}
static void web2_uses_embedded_gles_and_actual_glsl(void)
{
    TEST_ASSERT_TRUE(actual_post_init(JCE_BACKEND_OPENGLES));
    JceRendererApiInfo info=jce_renderer_get_api_info();TEST_ASSERT_TRUE(info.runtime_version_verified);
    TEST_ASSERT_TRUE(info.shader_language_version_verified);TEST_ASSERT_EQUAL_UINT16(3,info.runtime_version.major);
    TEST_ASSERT_EQUAL_UINT16(2,info.runtime_version.minor);TEST_ASSERT_EQUAL_UINT16(2,info.shader_language_version.minor);
    TEST_ASSERT_TRUE(state.admitted);TEST_ASSERT_EQUAL_UINT(1,active_accepts);TEST_ASSERT_EQUAL_UINT(0,shutdowns);
}
static void unavailable_init_refuses_and_cleans_exactly_once(void)
{
    available=false;TEST_ASSERT_FALSE(actual_post_init(JCE_BACKEND_OPENGLES));cleared();
    TEST_ASSERT_EQUAL_UINT(0,active_accepts);TEST_ASSERT_EQUAL_UINT(1,shutdowns);
    TEST_ASSERT_EQUAL_UINT(1,joins);TEST_ASSERT_EQUAL_UINT(1,releases);
}
static void unknown_and_missing_shading_refuse_without_stale_flags(void)
{
    TEST_ASSERT_TRUE(actual_web_admit());strcpy(observed.api,"WebGL 2.0 unknown driver");
    TEST_ASSERT_FALSE(actual_web_admit());cleared();
    strcpy(observed.api,"WebGL 2.0 (OpenGL ES 3.0 fixture)");observed.shading[0]=0;
    TEST_ASSERT_FALSE(actual_web_admit());cleared();
}
static void known_web1_below_floor_cannot_reach_active_accept(void)
{
    observed.webgl_major=1;strcpy(observed.api,"WebGL 1.0 (OpenGL ES 2.0 fixture)");
    strcpy(observed.shading,"WebGL GLSL ES 1.00 fixture");
    TEST_ASSERT_FALSE(actual_post_init(JCE_BACKEND_OPENGLES));cleared();TEST_ASSERT_EQUAL_UINT(0,active_accepts);
    TEST_ASSERT_EQUAL_UINT(1,shutdowns);
}
static void malformed_or_mixed_snapshot_cannot_publish(void)
{
    observed.epoch_end=6;TEST_ASSERT_FALSE(actual_web_admit());cleared();
    observed.epoch_begin=observed.epoch_end=0;TEST_ASSERT_FALSE(actual_web_admit());cleared();
    observed.epoch_begin=observed.epoch_end=5;observed.context_id=0;TEST_ASSERT_FALSE(actual_web_admit());cleared();
}
static void bad_glsl_does_not_certify_shader_contract(void)
{
    strcpy(observed.shading,"unknown shading 999.0");TEST_ASSERT_FALSE(actual_web_admit());cleared();
    strcpy(observed.shading,"WebGL GLSL ES 1.00");TEST_ASSERT_FALSE(actual_web_admit());cleared();
}
static void loss_clears_then_restore_requires_lifecycle_readmission(void)
{
    TEST_ASSERT_TRUE(actual_web_admit());available=false;
    TEST_ASSERT_FALSE(jce_renderer_web_api_current(&state,read_context));cleared();
    available=true;observed.epoch_begin=observed.epoch_end=6;
    TEST_ASSERT_FALSE(jce_renderer_web_api_current(&state,read_context));cleared();
    TEST_ASSERT_TRUE(actual_web_admit());TEST_ASSERT_EQUAL_UINT32(6,state.epoch);
}
static void changed_epoch_or_replacement_never_blindly_carries(void)
{
    TEST_ASSERT_TRUE(actual_web_admit());observed.epoch_begin=observed.epoch_end=6;
    TEST_ASSERT_FALSE(jce_renderer_web_api_current(&state,read_context));cleared();
    TEST_ASSERT_TRUE(actual_web_admit());observed.context_id=14;
    TEST_ASSERT_FALSE(jce_renderer_web_api_current(&state,read_context));cleared();
}
static void native_or_noop_boundaries_do_not_read_web_provider(void)
{
    available=false;TEST_ASSERT_TRUE(actual_post_init(JCE_BACKEND_NOOP));
    TEST_ASSERT_EQUAL_UINT(0,reads);TEST_ASSERT_EQUAL_UINT(1,active_accepts);TEST_ASSERT_EQUAL_UINT(0,shutdowns);
}
static void repeated_current_observation_keeps_identity_without_republication(void)
{
    TEST_ASSERT_TRUE(actual_web_admit());
    TEST_ASSERT_TRUE(jce_renderer_web_api_current(&state,read_context));
    TEST_ASSERT_TRUE(jce_renderer_web_api_current(&state,read_context));
    TEST_ASSERT_EQUAL_UINT32(12,state.context_id);TEST_ASSERT_EQUAL_UINT32(5,state.epoch);
}
static void trace_failure_and_identity_close_without_gpu_claim(void)
{
    jce_trace_reset();jce_trace_task_restore_id(91);jce_trace_set_enabled(true);
    observed.epoch_end=6;TEST_ASSERT_FALSE(actual_web_admit());
    TEST_ASSERT_EQUAL_UINT64(91,jce_trace_task_current_id());
    JceTraceEvent events[64];JceTraceCursor cursor;jce_trace_cursor_init(&cursor);
    uint32_t count=jce_trace_read(&cursor,events,64);bool reason=false,end=false;
    for(uint32_t i=0;i<count;++i){
        if(strcmp(events[i].name,"renderer.web_api.reason")==0){reason=true;TEST_ASSERT_EQUAL_UINT64(JCE_WEB_API_MIXED_EPOCH,events[i].value_u64);}
        if(events[i].type==JCE_TRACE_EVENT_TASK_END && strcmp(events[i].name,"renderer.web_api.admit")==0){end=true;TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_FAILED,events[i].value_u32[0]);}
    }
    TEST_ASSERT_TRUE(reason);TEST_ASSERT_TRUE(end);
}
int main(void)
{
    UNITY_BEGIN();RUN_TEST(web2_uses_embedded_gles_and_actual_glsl);
    RUN_TEST(unavailable_init_refuses_and_cleans_exactly_once);
    RUN_TEST(unknown_and_missing_shading_refuse_without_stale_flags);
    RUN_TEST(known_web1_below_floor_cannot_reach_active_accept);
    RUN_TEST(malformed_or_mixed_snapshot_cannot_publish);
    RUN_TEST(bad_glsl_does_not_certify_shader_contract);
    RUN_TEST(loss_clears_then_restore_requires_lifecycle_readmission);
    RUN_TEST(changed_epoch_or_replacement_never_blindly_carries);
    RUN_TEST(native_or_noop_boundaries_do_not_read_web_provider);
    RUN_TEST(repeated_current_observation_keeps_identity_without_republication);
    RUN_TEST(trace_failure_and_identity_close_without_gpu_claim);return UNITY_END();
}
