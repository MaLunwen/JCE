/* Exact display-selection header + extracted actual init prefix/startup log.
 * Typed counted ports prove policy/wiring/diagnostics, not real GPU selection. */
#include "unity.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_trace_scope_internal.h"
#ifndef JCE_RENDERER_DISPLAY_ADAPTER_OWNER
#define JCE_RENDERER_DISPLAY_ADAPTER_OWNER "renderer/jce_renderer_display_adapter_internal.h"
#endif
#include JCE_RENDERER_DISPLAY_ADAPTER_OWNER
static unsigned char window_tag;
static JceWindow *window(void){return (JceWindow *)&window_tag;}
static struct {
    JceWindowAdapterObservation observation,performance;
    bool success,performance_success;
    unsigned observations,performance_observations,ctors,begins,logs;
    uint16_t preference;
    bgfx_renderer_type_t begin_type;
    JceLogLevel level;
    char log[768],performance_log[256];
} port;
static bool fixture_observe(JceWindow *win,JceWindowAdapterObservation *out)
{TEST_ASSERT_EQUAL_PTR(window(),win);++port.observations;*out=port.observation;return port.success;}
static bool fixture_performance(JceWindow *win,JceWindowAdapterObservation *out)
{
    TEST_ASSERT_EQUAL_PTR(window(),win);
    ++port.performance_observations;
    *out=port.performance;
    return port.performance_success;
}
static void fixture_ctor(bgfx_init_t *init)
{++port.ctors;memset(init,0,sizeof *init);init->type=BGFX_RENDERER_TYPE_COUNT;}
static bgfx_renderer_type_t fixture_backend(bgfx_renderer_type_t type){return type;}
static void fixture_begin(bgfx_renderer_type_t type){++port.begins;port.begin_type=type;}
static void apply_adapter_preference(bgfx_init_t *init){init->vendorId=port.preference;}
#define bgfx_init_ctor fixture_ctor
#define from_bgfx_type fixture_backend
#define jce_renderer_caps_api_begin_attempt fixture_begin
#define jce_window_observe_display_adapter fixture_observe
#define jce_window_observe_performance_adapter fixture_performance
static bgfx_init_t actual_attempt_prefix(bgfx_renderer_type_t type,JceRendererDisplayAdapterState *display_adapter)
{
    bgfx_init_t init;
    JceWindow *window=(JceWindow *)&window_tag;
#include JCE_RENDERER_DISPLAY_ADAPTER_INIT_PREFIX
    return init;
}
#undef bgfx_init_ctor
#undef from_bgfx_type
#undef jce_renderer_caps_api_begin_attempt
#undef jce_window_observe_display_adapter
#undef jce_window_observe_performance_adapter
static uint16_t s_adapter_vendor_pref;
static void JCE_CALL counted_log(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{
    (void)tag;(void)file;(void)line;
    const bool performance=strncmp(format,"GPU render adapter:",sizeof "GPU render adapter:"-1)==0;
    char *log=performance ? port.performance_log : port.log;
    const size_t capacity=performance ? sizeof port.performance_log : sizeof port.log;
    port.level=level;++port.logs;
    va_list args;va_start(args,format);
    const int length=vsnprintf(log,capacity,format,args);
    va_end(args);
    TEST_ASSERT_TRUE(length>=0 && (size_t)length<capacity);
}
#define LOG_TAG "jce_renderer"
#define jce_log_write counted_log
#include JCE_RENDERER_DISPLAY_ADAPTER_STARTUP_LOG
#undef jce_log_write
void setUp(void)
{
    memset(&port,0,sizeof port);port.success=true;s_adapter_vendor_pref=0;
    port.observation=(JceWindowAdapterObservation){0x8086,0x9a49,0xabcdef01,-5,1,JCE_WINDOW_ADAPTER_MATCH};
    port.observation.source_valid=true;port.observation.source_luid_low=0xabcdef01;port.observation.source_luid_high=-5;
    port.observation.source_id=3;port.observation.path_count=1;port.observation.mode_count=1;
    port.observation.adapter_count=3;port.observation.luid_match_count=1;port.observation.key_match_count=2;
    port.observation.backend_expected_valid=true;port.observation.backend_expected_luid_low=0xabcdef01;
    port.observation.backend_expected_luid_high=-5;port.observation.backend_expected_index=1;
    port.performance=(JceWindowAdapterObservation){0x10de,0x28e0,0x2468ace0,2,0,
        JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE};
    port.performance.source_valid=true;
    port.performance.source_luid_low=0x2468ace0;
    port.performance.source_luid_high=2;
    port.performance.adapter_count=3;
    port.performance.luid_match_count=1;
    port.performance.key_match_count=1;
    port.performance.backend_expected_valid=true;
    port.performance.backend_expected_luid_low=0x2468ace0;
    port.performance.backend_expected_luid_high=2;
    jce_trace_set_enabled(false);jce_trace_reset();
}
void tearDown(void){jce_trace_set_enabled(false);jce_trace_thread_unregister();}
static bgfx_init_t initial(bgfx_renderer_type_t type)
{bgfx_init_t init;memset(&init,0,sizeof init);init.type=type;return init;}
static void auto_d3d_uses_both_actual_keys_and_queries_once_across_fallback_attempts(void)
{
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=initial(BGFX_RENDERER_TYPE_DIRECT3D11);
    TEST_ASSERT_TRUE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_HEX16(0x8086,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x9a49,init.deviceId);
    TEST_ASSERT_TRUE(state.applied);TEST_ASSERT_EQUAL_UINT(1,port.observations);
    init=initial(BGFX_RENDERER_TYPE_DIRECT3D12);
    TEST_ASSERT_TRUE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_HEX16(0x8086,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x9a49,init.deviceId);
    TEST_ASSERT_EQUAL_UINT(1,port.observations);
    JceRendererDisplayAdapterState next={0};init=initial(BGFX_RENDERER_TYPE_DIRECT3D11);
    TEST_ASSERT_TRUE(jce_renderer_apply_display_adapter(&init,window(),&next,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_UINT(2,port.observations); /* no stale process-global observation */
}
static void explicit_d3d_vendor_and_device_observe_once_without_changing_selection(void)
{
    const bgfx_renderer_type_t types[]={BGFX_RENDERER_TYPE_DIRECT3D11,BGFX_RENDERER_TYPE_DIRECT3D12};
    port.performance_success=true;port.performance.reason=JCE_WINDOW_ADAPTER_MATCH;
    const uint16_t vendors[]={0x10de,0,0x10de},devices[]={0,0x2507,0x28e0};
    for(unsigned i=0;i<2;++i)for(unsigned k=0;k<3;++k){
        JceRendererDisplayAdapterState state={0};
        port.observation.adapter_index=k==2
            ? (uint32_t)(sizeof(((bgfx_caps_t *)0)->gpu)/sizeof(bgfx_caps_gpu_t)) : 1u;
        for(unsigned attempt=0;attempt<2;++attempt){
            bgfx_init_t init=initial(types[(i+attempt)%2]);init.vendorId=vendors[k];init.deviceId=devices[k];
            TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,fixture_performance));
            TEST_ASSERT_FALSE(state.performance_queried);TEST_ASSERT_FALSE(state.performance_applied);
            TEST_ASSERT_EQUAL_UINT(0,port.performance_observations);
            TEST_ASSERT_EQUAL_HEX16(vendors[k],init.vendorId);TEST_ASSERT_EQUAL_HEX16(devices[k],init.deviceId);
            TEST_ASSERT_EQUAL_HEX16(vendors[k],state.effective_vendor);TEST_ASSERT_EQUAL_HEX16(devices[k],state.effective_device);
            TEST_ASSERT_TRUE(state.queried);TEST_ASSERT_FALSE(state.applied);TEST_ASSERT_EQUAL_UINT(i*3+k+1,port.observations);
            TEST_ASSERT_EQUAL_HEX16(0x8086,state.observation.vendor_id);TEST_ASSERT_EQUAL_HEX16(0x9a49,state.observation.device_id);
            TEST_ASSERT_TRUE(state.observation.source_valid);TEST_ASSERT_EQUAL_HEX32(0xabcdef01,state.observation.source_luid_low);
            TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_MATCH,state.observation.reason);
            TEST_ASSERT_TRUE(state.observation.backend_expected_valid);
            TEST_ASSERT_EQUAL_UINT32(port.observation.adapter_index,state.observation.adapter_index);
        }
    }
}
static void other_backends_keep_original_policy_without_query(void)
{
    JceRendererDisplayAdapterState state={0};bgfx_init_t init;
    const bgfx_renderer_type_t other[]={BGFX_RENDERER_TYPE_VULKAN,BGFX_RENDERER_TYPE_OPENGL,BGFX_RENDERER_TYPE_NOOP};
    port.performance_success=true;port.performance.reason=JCE_WINDOW_ADAPTER_MATCH;
    for(unsigned i=0;i<sizeof other/sizeof other[0];++i)for(unsigned explicit_request=0;explicit_request<2;++explicit_request){
        init=initial(other[i]);init.vendorId=explicit_request ? 0x10de : 0;init.deviceId=explicit_request ? 0x28e0 : 0;
        TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,fixture_performance));
        TEST_ASSERT_EQUAL_HEX16(explicit_request ? 0x10de : 0,init.vendorId);
        TEST_ASSERT_EQUAL_HEX16(explicit_request ? 0x28e0 : 0,init.deviceId);}
    TEST_ASSERT_FALSE(state.queried);TEST_ASSERT_FALSE(state.applied);TEST_ASSERT_EQUAL_UINT(0,port.observations);
    TEST_ASSERT_FALSE(state.performance_queried);TEST_ASSERT_FALSE(state.performance_applied);
    TEST_ASSERT_EQUAL_UINT(0,port.performance_observations);
}
static void explicit_d3d_failed_observation_keeps_request_and_caches_reason(void)
{
    const JceWindowAdapterReason reasons[]={JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE,
        JCE_WINDOW_ADAPTER_NO_MATCH,JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH};
    for(unsigned i=0;i<sizeof reasons/sizeof reasons[0];++i){
        JceRendererDisplayAdapterState state={0};port.success=false;port.observation.reason=reasons[i];
        for(unsigned j=0;j<2;++j){bgfx_init_t init=initial(j ? BGFX_RENDERER_TYPE_DIRECT3D12 : BGFX_RENDERER_TYPE_DIRECT3D11);
            init.vendorId=0x10de;init.deviceId=0x28e0;
            TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
            TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x28e0,init.deviceId);
            TEST_ASSERT_EQUAL_HEX16(0x10de,state.effective_vendor);TEST_ASSERT_EQUAL_HEX16(0x28e0,state.effective_device);
            TEST_ASSERT_TRUE(state.queried);TEST_ASSERT_FALSE(state.applied);
            TEST_ASSERT_EQUAL_INT(reasons[i],state.observation.reason);
            TEST_ASSERT_TRUE(state.observation.source_valid);
            TEST_ASSERT_EQUAL_HEX32(0xabcdef01,state.observation.source_luid_low);
            TEST_ASSERT_EQUAL_HEX32((uint32_t)-5,(uint32_t)state.observation.source_luid_high);}
        TEST_ASSERT_EQUAL_UINT(i+1,port.observations);
    }
}
static void unavailable_ambiguous_and_backend_limit_are_cached_fallbacks(void)
{
    const JceWindowAdapterReason reasons[]={JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE,JCE_WINDOW_ADAPTER_AMBIGUOUS,
        JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,JCE_WINDOW_ADAPTER_NO_MATCH};
    for(unsigned i=0;i<sizeof reasons/sizeof reasons[0];++i){
        JceRendererDisplayAdapterState state={0};port.success=false;port.observation.reason=reasons[i];
        for(unsigned j=0;j<2;++j){bgfx_init_t init=initial(BGFX_RENDERER_TYPE_DIRECT3D11);
            TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
            TEST_ASSERT_EQUAL_UINT(0,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);}
        TEST_ASSERT_EQUAL_INT(reasons[i],state.observation.reason);
    }
    TEST_ASSERT_EQUAL_UINT(4,port.observations);port.success=true;port.observation.reason=JCE_WINDOW_ADAPTER_MATCH;
    port.observation.adapter_index=(uint32_t)(sizeof(((bgfx_caps_t *)0)->gpu)/sizeof(bgfx_caps_gpu_t))-1;
    port.observation.backend_expected_index=port.observation.adapter_index;
    JceRendererDisplayAdapterState boundary={0};bgfx_init_t boundary_init=initial(BGFX_RENDERER_TYPE_DIRECT3D12);
    TEST_ASSERT_TRUE(jce_renderer_apply_display_adapter(&boundary_init,window(),&boundary,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_HEX16(0x8086,boundary_init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x9a49,boundary_init.deviceId);
    port.observation.adapter_index=(uint32_t)(sizeof(((bgfx_caps_t *)0)->gpu)/sizeof(bgfx_caps_gpu_t));
    port.observation.backend_expected_index=port.observation.adapter_index;
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=initial(BGFX_RENDERER_TYPE_DIRECT3D12);
    TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_BACKEND_LIMIT,state.observation.reason);
    TEST_ASSERT_FALSE(state.observation.backend_expected_valid);
    TEST_ASSERT_EQUAL_UINT(0,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);
}
static void invalid_keys_failed_observer_and_null_inputs_never_publish_partial_request(void)
{
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=initial(BGFX_RENDERER_TYPE_DIRECT3D11);
    TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(NULL,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),NULL,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_UINT(0,port.observations);port.observation.device_id=0;
    TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_UNREPRESENTABLE,state.observation.reason);
    TEST_ASSERT_EQUAL_UINT(0,init.vendorId);state=(JceRendererDisplayAdapterState){0};
    port.success=false;port.observation.reason=JCE_WINDOW_ADAPTER_MATCH;
    TEST_ASSERT_FALSE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,NULL));
    TEST_ASSERT_EQUAL_INT(JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,state.observation.reason);
}
static void actual_init_prefers_performance_and_reuses_it_across_d3d_attempts(void)
{
    port.performance_success=true;port.performance.reason=JCE_WINDOW_ADAPTER_MATCH;
    JceRendererDisplayAdapterState state={0};
    const bgfx_renderer_type_t types[]={BGFX_RENDERER_TYPE_DIRECT3D11,BGFX_RENDERER_TYPE_DIRECT3D12};
    for(unsigned attempt=0;attempt<sizeof types/sizeof types[0];++attempt) {
        bgfx_init_t init=actual_attempt_prefix(types[attempt],&state);
        TEST_ASSERT_EQUAL_INT(types[attempt],init.type);
        TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x28e0,init.deviceId);
        TEST_ASSERT_EQUAL_HEX16(0x10de,state.effective_vendor);TEST_ASSERT_EQUAL_HEX16(0x28e0,state.effective_device);
        TEST_ASSERT_TRUE(state.performance_queried);TEST_ASSERT_TRUE(state.performance_applied);
        TEST_ASSERT_TRUE(state.queried);TEST_ASSERT_FALSE(state.applied);
        TEST_ASSERT_EQUAL_HEX32(0x2468ace0,state.performance.luid_low);
        TEST_ASSERT_EQUAL_HEX16(0x8086,state.observation.vendor_id);
        TEST_ASSERT_EQUAL_HEX32(0xabcdef01,state.observation.luid_low);
        TEST_ASSERT_EQUAL_UINT(1,port.performance_observations);TEST_ASSERT_EQUAL_UINT(1,port.observations);
    }
    bgfx_caps_t selected={0};selected.vendorId=0x10de;selected.deviceId=0x28e0;
    log_display_adapter(window(),&state,&selected);
    TEST_ASSERT_EQUAL_UINT(2,port.logs);
    TEST_ASSERT_NOT_NULL(strstr(port.performance_log,"observed=0x10DE:0x28E0 luid=00000002:2468ACE0"));
    TEST_ASSERT_NOT_NULL(strstr(port.performance_log,"selected=0x10DE:0x28E0 reason=match applied=1 exact_match=1 init_fallback=0"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"source=ccd_active_source observed=0x8086:0x9A49"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"requested=0x10DE:0x28E0 selected=0x10DE:0x28E0"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"fallback=performance_policy"));

    port.success=false;port.observation.reason=JCE_WINDOW_ADAPTER_MONITOR_UNAVAILABLE;
    JceRendererDisplayAdapterState next={0};
    const bgfx_init_t init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&next);
    TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x28e0,init.deviceId);
    TEST_ASSERT_TRUE(next.performance_applied);TEST_ASSERT_FALSE(next.applied);
    TEST_ASSERT_EQUAL_UINT(2,port.performance_observations);
    TEST_ASSERT_EQUAL_UINT(2,port.observations);
}

static void rejected_performance_is_cached_and_uses_existing_display_route(void)
{
    const uint32_t cap_count=(uint32_t)(sizeof(((bgfx_caps_t *)0)->gpu)/sizeof(bgfx_caps_gpu_t));
    const struct {
        bool success;
        JceWindowAdapterReason reason,expected_reason;
        uint16_t device;
        uint32_t index;
    } cases[]={
        {false,JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE,JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE,0x28e0,0},
        {false,JCE_WINDOW_ADAPTER_MATCH,JCE_WINDOW_ADAPTER_ENUMERATION_FAILED,0x28e0,0},
        {true,JCE_WINDOW_ADAPTER_MATCH,JCE_WINDOW_ADAPTER_UNREPRESENTABLE,0,0},
        {true,JCE_WINDOW_ADAPTER_MATCH,JCE_WINDOW_ADAPTER_BACKEND_LIMIT,0x28e0,cap_count},
    };
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i) {
        port.performance_success=cases[i].success;
        port.performance.reason=cases[i].reason;
        port.performance.device_id=cases[i].device;
        port.performance.adapter_index=cases[i].index;
        port.performance.backend_expected_index=cases[i].index;
        JceRendererDisplayAdapterState state={0};
        for(unsigned attempt=0;attempt<2;++attempt) {
            bgfx_init_t init=initial(attempt ? BGFX_RENDERER_TYPE_DIRECT3D12 : BGFX_RENDERER_TYPE_DIRECT3D11);
            TEST_ASSERT_TRUE(jce_renderer_apply_display_adapter(&init,window(),&state,fixture_observe,fixture_performance));
            TEST_ASSERT_EQUAL_HEX16(0x8086,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x9a49,init.deviceId);
            TEST_ASSERT_TRUE(state.performance_queried);TEST_ASSERT_FALSE(state.performance_applied);
            TEST_ASSERT_EQUAL_INT(cases[i].expected_reason,state.performance.reason);
            TEST_ASSERT_TRUE(state.applied);
            TEST_ASSERT_EQUAL_UINT(i+1,port.performance_observations);TEST_ASSERT_EQUAL_UINT(i+1,port.observations);
        }
    }
}

static void actual_init_ctor_backend_and_preference_order_consume_display_owner(void)
{
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&state);
    TEST_ASSERT_EQUAL_HEX16(0x8086,init.vendorId);TEST_ASSERT_EQUAL_HEX16(0x9a49,init.deviceId);
    TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_DIRECT3D11,init.type);
    TEST_ASSERT_EQUAL_INT(init.type,port.begin_type);TEST_ASSERT_EQUAL_UINT(1,port.begins);TEST_ASSERT_EQUAL_UINT(1,port.ctors);
    TEST_ASSERT_EQUAL_UINT(1,port.observations);
    port.preference=0x10de;init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D12,&state);
    TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);
    TEST_ASSERT_EQUAL_UINT(1,port.observations);TEST_ASSERT_FALSE(state.applied);
}
static void actual_startup_log_records_observed_requested_selected_and_mismatch_once(void)
{
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&state);
    bgfx_caps_t selected={0};selected.vendorId=init.vendorId;selected.deviceId=init.deviceId;
    log_display_adapter(window(),&state,&selected);
    TEST_ASSERT_EQUAL_UINT(2,port.logs);TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_NOT_NULL(strstr(port.performance_log,"reason=performance_unavailable applied=0 exact_match=0 init_fallback=0"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"source=ccd_active_source"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"ccd_valid=1 ccd_luid=FFFFFFFB:ABCDEF01 ccd_source=3"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"adapter_count=3 luid_matches=1 key_matches=2 backend_expected_valid=1"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"observed=0x8086:0x9A49"));TEST_ASSERT_NOT_NULL(strstr(port.log,"policy=0x0000"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"requested=0x8086:0x9A49 selected=0x8086:0x9A49"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"reason=match applied=1 exact_match=1 fallback=none"));
    selected.vendorId=0x10de;selected.deviceId=0x2507;log_display_adapter(window(),&state,&selected);
    TEST_ASSERT_EQUAL_UINT(4,port.logs);TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_WARN,port.level);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"exact_match=0 fallback=backend_default"));
    JceTraceStats stats;jce_trace_get_stats(&stats);TEST_ASSERT_EQUAL_UINT64(0,stats.recorded_events);
}
static void actual_explicit_init_and_log_keep_observed_requested_and_selected_separate(void)
{
    JceRendererDisplayAdapterState state={0};port.preference=0x10de;s_adapter_vendor_pref=0x10de;
    bgfx_init_t init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&state);
    TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);
    TEST_ASSERT_TRUE(state.queried);TEST_ASSERT_FALSE(state.applied);TEST_ASSERT_EQUAL_UINT(1,port.observations);
    bgfx_caps_t selected={0};selected.vendorId=0x10de;selected.deviceId=0x28e0;
    log_display_adapter(window(),&state,&selected);
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"source=ccd_active_source observed=0x8086:0x9A49"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"ccd_valid=1 ccd_luid=FFFFFFFB:ABCDEF01"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"policy=0x10DE requested=0x10DE:0x0000 selected=0x10DE:0x28E0"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"reason=match applied=0 exact_match=0 fallback=original_policy"));
    init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D12,&state);
    TEST_ASSERT_EQUAL_HEX16(0x10de,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);
    TEST_ASSERT_EQUAL_UINT(1,port.observations);TEST_ASSERT_FALSE(state.applied);
}
static void rejected_key_keeps_ccd_and_backend_prediction_separate_from_selected(void)
{
    port.success=false;port.observation.reason=JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH;
    port.observation.vendor_id=port.observation.device_id=0;port.observation.luid_low=0;port.observation.luid_high=0;
    port.observation.backend_expected_luid_low=0x29360;port.observation.backend_expected_luid_high=0;
    JceRendererDisplayAdapterState state={0};bgfx_init_t init=actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&state);
    TEST_ASSERT_EQUAL_UINT(0,init.vendorId);TEST_ASSERT_EQUAL_UINT(0,init.deviceId);TEST_ASSERT_FALSE(state.applied);
    bgfx_caps_t selected={0};selected.vendorId=0x10de;selected.deviceId=0x28e0;log_display_adapter(window(),&state,&selected);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"observed=0x0000:0x0000 luid=00000000:00000000"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"ccd_valid=1 ccd_luid=FFFFFFFB:ABCDEF01"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"backend_expected_luid=00000000:00029360"));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"selected=0x10DE:0x28E0 reason=backend_key_mismatch applied=0 exact_match=0"));
}
static void actual_startup_trace_has_correlated_keys_and_failed_create_state(void)
{
    jce_trace_set_enabled(true);jce_trace_thread_register("adapter-test");
    JceRendererDisplayAdapterState state={0};(void)actual_attempt_prefix(BGFX_RENDERER_TYPE_DIRECT3D11,&state);
    log_display_adapter(window(),&state,NULL);
    TEST_ASSERT_EQUAL_UINT(2,port.logs);TEST_ASSERT_NOT_NULL(strstr(port.log,"fallback=backend_init_failed"));
    JceTraceCursor cursor;jce_trace_cursor_init(&cursor);JceTraceEvent events[32];
    uint32_t count=jce_trace_read(&cursor,events,32);unsigned starts=0,ends=0,keys=0;uint64_t scope=0;
    for(uint32_t i=0;i<count;++i){JceTraceEvent *event=&events[i];
        if(!strcmp(event->name,"renderer.adapter.startup") && event->type==JCE_TRACE_EVENT_TASK_BEGIN){scope=event->id;++starts;}
        if(!strcmp(event->name,"renderer.adapter.startup") && event->type==JCE_TRACE_EVENT_TASK_END){
            ++ends;TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED,event->value_u32[0]);TEST_ASSERT_EQUAL_UINT64(scope,event->id);}
        if(event->type==JCE_TRACE_EVENT_COUNTER && !strncmp(event->name,"renderer.adapter.",17)){
            ++keys;TEST_ASSERT_EQUAL_UINT64(scope,event->id);TEST_ASSERT_EQUAL_UINT64(scope,event->parent_id);
            if(!strcmp(event->name,"renderer.adapter.performance_reason"))
                TEST_ASSERT_EQUAL_UINT64(JCE_WINDOW_ADAPTER_PERFORMANCE_UNAVAILABLE,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.performance_applied"))TEST_ASSERT_EQUAL_UINT64(0,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.performance_blocked"))TEST_ASSERT_EQUAL_UINT64(0,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.observed"))TEST_ASSERT_EQUAL_HEX32(0x80869a49,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.requested"))TEST_ASSERT_EQUAL_HEX32(0x80869a49,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.selected"))TEST_ASSERT_EQUAL_UINT64(0,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.luid_low"))TEST_ASSERT_EQUAL_HEX32(0xabcdef01,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.luid_high"))TEST_ASSERT_EQUAL_HEX32((uint32_t)-5,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.index"))TEST_ASSERT_EQUAL_UINT64(1,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.source_valid"))TEST_ASSERT_EQUAL_UINT64(1,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.source_luid_low"))TEST_ASSERT_EQUAL_HEX32(0xabcdef01,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.source_luid_high"))TEST_ASSERT_EQUAL_HEX32((uint32_t)-5,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.source_id"))TEST_ASSERT_EQUAL_UINT64(3,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.key_matches"))TEST_ASSERT_EQUAL_UINT64(2,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.backend_expected_valid"))TEST_ASSERT_EQUAL_UINT64(1,event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.backend_expected_luid_low"))TEST_ASSERT_EQUAL_HEX32(0xabcdef01,(uint32_t)event->value_u64);
            if(!strcmp(event->name,"renderer.adapter.backend_expected_index"))TEST_ASSERT_EQUAL_UINT64(1,event->value_u64);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1,starts);TEST_ASSERT_EQUAL_UINT(1,ends);TEST_ASSERT_EQUAL_UINT(25,keys);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(auto_d3d_uses_both_actual_keys_and_queries_once_across_fallback_attempts);
    RUN_TEST(explicit_d3d_vendor_and_device_observe_once_without_changing_selection);
    RUN_TEST(other_backends_keep_original_policy_without_query);
    RUN_TEST(explicit_d3d_failed_observation_keeps_request_and_caches_reason);
    RUN_TEST(unavailable_ambiguous_and_backend_limit_are_cached_fallbacks);
    RUN_TEST(invalid_keys_failed_observer_and_null_inputs_never_publish_partial_request);
    RUN_TEST(actual_init_prefers_performance_and_reuses_it_across_d3d_attempts);
    RUN_TEST(rejected_performance_is_cached_and_uses_existing_display_route);
    RUN_TEST(actual_init_ctor_backend_and_preference_order_consume_display_owner);
    RUN_TEST(actual_startup_log_records_observed_requested_selected_and_mismatch_once);
    RUN_TEST(actual_explicit_init_and_log_keep_observed_requested_and_selected_separate);
    RUN_TEST(rejected_key_keeps_ccd_and_backend_prediction_separate_from_selected);
    RUN_TEST(actual_startup_trace_has_correlated_keys_and_failed_create_state);
    return UNITY_END();
}
