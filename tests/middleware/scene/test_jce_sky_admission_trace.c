/* Actual sky gate, allocation prefix, uniform-observation and submit tail.
 * Unrelated sky uniform math is a controlled payload port, not GPU evidence. */
#include "unity.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_scene_renderer.h>
#include "os/core/jce_trace_scope_internal.h"

#include "os/core/jce_trace_capture_internal.h"
#include <bgfx/c99/bgfx.h>
#include <string.h>

typedef enum SrSkySubmitResult {
    SR_SKY_CONFIG_DISABLED = 1, SR_SKY_WIREFRAME_DISABLED,
    SR_SKY_TEXTURED_WIREFRAME_NO_HDR, SR_SKY_PROGRAM_SENTINEL_INVALID,
    SR_SKY_TRANSIENT_REFUSED, SR_SKY_SUBMIT_RETURNED
} SrSkySubmitResult;
struct JceSceneRenderer {
    bgfx_program_handle_t prog_sky;
    bgfx_program_handle_t prog_sky_physical_compat;
    bgfx_vertex_layout_t sky_layout;
    bgfx_uniform_handle_t u_sky_params;
    bool skybox_active;
    bool sky_legacy_gl21;
    int sky_mode;
    uint64_t resource_prepared_context;
    uint32_t resource_prepared_frame;
};
static JceSceneRenderer renderer;
static JceSceneRenderConfig config;
static bool diag, refuse_allocation;
static unsigned policy_reads, allocations, uniforms, vertex_binds, index_binds, states, transforms, submits;
static float vertices[12], payload_params[4], payload_colors[12];
static uint16_t indices[6], submitted_view;
static bgfx_program_handle_t submitted_program;
static uint64_t submitted_state;
static bool policy_port(void) { ++policy_reads; return diag; }
static bool alloc_port(bgfx_transient_vertex_buffer_t *v,const bgfx_vertex_layout_t *layout,
    uint32_t nv,bgfx_transient_index_buffer_t *i,uint32_t ni,bool index32)
{
    TEST_ASSERT_EQUAL_PTR(&renderer.sky_layout,layout); TEST_ASSERT_EQUAL_UINT32(4,nv);
    TEST_ASSERT_EQUAL_UINT32(6,ni); TEST_ASSERT_FALSE(index32); ++allocations;
    if (refuse_allocation) return false;
    memset(v,0,sizeof(*v)); memset(i,0,sizeof(*i));
    v->data=(uint8_t *)vertices; i->data=(uint8_t *)indices; return true;
}
static void uniform_port(bgfx_uniform_handle_t handle,const void *value,uint16_t count)
{ TEST_ASSERT_EQUAL_UINT16(renderer.u_sky_params.idx,handle.idx); TEST_ASSERT_EQUAL_UINT16(1,count); TEST_ASSERT_EQUAL_FLOAT_ARRAY(payload_params,value,4); ++uniforms; }
static void vertex_port(uint8_t stream,const bgfx_transient_vertex_buffer_t *data,uint32_t start,uint32_t count)
{ TEST_ASSERT_EQUAL_UINT8(0,stream); TEST_ASSERT_EQUAL_PTR(vertices,data->data); TEST_ASSERT_EQUAL_UINT32(0,start); TEST_ASSERT_EQUAL_UINT32(4,count); ++vertex_binds; }
static void index_port(const bgfx_transient_index_buffer_t *data,uint32_t start,uint32_t count)
{ TEST_ASSERT_EQUAL_PTR(indices,data->data); TEST_ASSERT_EQUAL_UINT32(0,start); TEST_ASSERT_EQUAL_UINT32(6,count); ++index_binds; }
static void state_port(uint64_t state,uint32_t rgba)
{ TEST_ASSERT_EQUAL_UINT32(0,rgba); submitted_state=state; ++states; }
static uint32_t transform_port(const void *matrix,uint16_t count)
{ jce_mat4 identity=jce_m4_identity(); TEST_ASSERT_EQUAL_UINT16(1,count); TEST_ASSERT_EQUAL_MEMORY(&identity,matrix,sizeof identity); ++transforms; return 0; }
static void submit_port(bgfx_view_id_t view,bgfx_program_handle_t program,uint32_t depth,uint8_t flags)
{
    TEST_ASSERT_EQUAL_UINT32(0,depth); TEST_ASSERT_EQUAL_UINT8(BGFX_DISCARD_ALL,flags);
    submitted_view=view; submitted_program=program; ++submits;
}
#define jce_renderer_shader_provenance_policy_internal policy_port
#define bgfx_alloc_transient_buffers alloc_port
#define bgfx_set_uniform uniform_port
#define bgfx_set_transient_vertex_buffer vertex_port
#define bgfx_set_transient_index_buffer index_port
#define bgfx_set_state state_port
#define bgfx_set_transform transform_port
#define bgfx_submit submit_port
#include JCE_TEST_SKY_HELPERS
#include JCE_TEST_SKY_DRAW
#include JCE_TEST_SKY_GATE
#undef bgfx_submit
#undef bgfx_set_transform
#undef bgfx_set_state
#undef bgfx_set_transient_index_buffer
#undef bgfx_set_transient_vertex_buffer
#undef bgfx_set_uniform
#undef bgfx_alloc_transient_buffers
#undef jce_renderer_shader_provenance_policy_internal
static JceTraceEvent events[2048];
static uint32_t event_count;
static void drain(void)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    event_count=jce_trace_read(&cursor,events,2048);
    TEST_ASSERT_LESS_THAN_UINT32(2048,event_count); TEST_ASSERT_EQUAL_UINT64(0,cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats); TEST_ASSERT_EQUAL_UINT32(0,stats.active_work_items);
}
static uint64_t packet(void)
{
    for(uint32_t i=event_count;i>0;--i) if(events[i-1].type==JCE_TRACE_EVENT_TASK_END && !strcmp(events[i-1].name,"sr.sky.admission")) return events[i-1].id;
    TEST_FAIL_MESSAGE("Missing actual sky admission terminal"); return 0;
}
static uint64_t value(uint64_t id,const char *name)
{
    for(uint32_t i=0;i<event_count;++i) if(events[i].type==JCE_TRACE_EVENT_COUNTER && events[i].id==id && !strcmp(events[i].name,name)) return events[i].value_u64;
    TEST_FAIL_MESSAGE(name); return UINT64_MAX;
}
static void outcome(SrSkySubmitResult expected,JceTraceTaskState state)
{
    drain(); uint64_t id=packet(); TEST_ASSERT_EQUAL_INT(expected,value(id,"sky.outcome"));
    TEST_ASSERT_EQUAL_UINT64(0,value(id,"sky.gpu_completion_known"));
    for(uint32_t i=0;i<event_count;++i) if(events[i].type==JCE_TRACE_EVENT_TASK_END && events[i].id==id) {
        TEST_ASSERT_EQUAL_UINT32(state,events[i].value_u32[0]); return;
    }
    TEST_FAIL_MESSAGE("Missing terminal state");
}
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_task_restore_id(0);
    memset(&renderer,0,sizeof renderer); memset(&config,0,sizeof config);
    renderer.prog_sky.idx=0; renderer.prog_sky_physical_compat.idx=UINT16_MAX;
    renderer.u_sky_params.idx=3; renderer.sky_mode=JCE_SCENE_SKY_STYLIZED;
    renderer.resource_prepared_context=UINT64_C(0x123456789abcdef0); renderer.resource_prepared_frame=UINT32_MAX;
    config.draw_skybox=true; config.view_mode=JCE_SCENE_VIEW_SHADED; config.viewport_id=2; config.scene_frame_buffer=233;
    diag=true; refuse_allocation=false; policy_reads=allocations=uniforms=vertex_binds=index_binds=states=transforms=submits=0;
    memset(vertices,0,sizeof vertices); memset(indices,0,sizeof indices);
    const float p[4]={3.0f,1.0f,-0.0f,0.25f}; memcpy(payload_params,p,sizeof p);
    for(unsigned i=0;i<12;++i) payload_colors[i]=(float)(i+1)/32.0f;
    jce_trace_set_enabled(true);
}
void tearDown(void) { jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_task_restore_id(0); }
static void caller_disabled_is_a_successful_policy_skip(void)
{
    config.draw_skybox=false; TEST_ASSERT_FALSE(run_pass(&renderer,120,&config,3));
    outcome(SR_SKY_CONFIG_DISABLED,JCE_TRACE_TASK_SUCCEEDED); TEST_ASSERT_EQUAL_UINT(0,allocations); TEST_ASSERT_EQUAL_UINT(0,submits);
    TEST_ASSERT_EQUAL_UINT64(0,value(packet(),"sky.caller_enabled"));
}
static void both_actual_wireframe_gates_are_distinct(void)
{
    config.view_mode=JCE_SCENE_VIEW_WIREFRAME; TEST_ASSERT_FALSE(run_pass(&renderer,3,&config,3));
    outcome(SR_SKY_WIREFRAME_DISABLED,JCE_TRACE_TASK_SUCCEEDED);
    config.view_mode=JCE_SCENE_VIEW_WIREFRAME_TEXTURED; TEST_ASSERT_FALSE(run_pass(&renderer,3,&config,3));
    outcome(SR_SKY_TEXTURED_WIREFRAME_NO_HDR,JCE_TRACE_TASK_SUCCEEDED); TEST_ASSERT_EQUAL_UINT(0,submits);
}
static void textured_wireframe_with_hdr_keeps_legacy_submit(void)
{
    renderer.skybox_active=true; config.view_mode=JCE_SCENE_VIEW_WIREFRAME_TEXTURED;
    TEST_ASSERT_TRUE(run_pass(&renderer,3,&config,3)); outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT(1,submits);
}
static void invalid_sentinel_is_not_gpu_lifetime_or_success(void)
{
    renderer.prog_sky.idx=UINT16_MAX; TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3));
    outcome(SR_SKY_PROGRAM_SENTINEL_INVALID,JCE_TRACE_TASK_FAILED); TEST_ASSERT_EQUAL_UINT(0,allocations); TEST_ASSERT_EQUAL_UINT(0,submits);
    TEST_ASSERT_EQUAL_UINT64(0,value(packet(),"sky.raw_idx_admitted"));
}
static void transient_refusal_retains_legacy_sky_drawn_without_false_submit(void)
{
    refuse_allocation=true; TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3));
    outcome(SR_SKY_TRANSIENT_REFUSED,JCE_TRACE_TASK_FAILED); TEST_ASSERT_EQUAL_UINT(1,allocations); TEST_ASSERT_EQUAL_UINT(0,submits);
}
static void successful_actual_tail_keeps_geometry_state_and_exact_uniform_bits(void)
{
    jce_trace_task_restore_id(91); TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3)); outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    uint64_t id=packet(); TEST_ASSERT_EQUAL_UINT64(91,jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_UINT64(0x12345678,value(id,"sky.context_hi")); TEST_ASSERT_EQUAL_UINT64(0x9abcdef0,value(id,"sky.context_lo"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX,value(id,"sky.backend_cpu_frame")); TEST_ASSERT_EQUAL_UINT64(120,value(id,"sky.view"));
    TEST_ASSERT_EQUAL_UINT64(2,value(id,"sky.viewport")); TEST_ASSERT_EQUAL_UINT64(233,value(id,"sky.requested_fbo"));
    TEST_ASSERT_EQUAL_UINT64(3,value(id,"sky.authored_mode")); TEST_ASSERT_EQUAL_UINT64(3,value(id,"sky.effective_mode"));
    TEST_ASSERT_EQUAL_UINT64(0,value(id,"sky.raw_program_idx")); TEST_ASSERT_EQUAL_UINT64(1,value(id,"sky.raw_idx_admitted"));
    TEST_ASSERT_EQUAL_UINT64(0x40400000,value(id,"sky.mode_bits")); TEST_ASSERT_EQUAL_UINT64(0x3f800000,value(id,"sky.exposure_bits"));
    TEST_ASSERT_EQUAL_UINT64(0x80000000,value(id,"sky.rotation_bits")); TEST_ASSERT_EQUAL_UINT64(0x3d000000,value(id,"sky.zenith_r_bits"));
    TEST_ASSERT_EQUAL_UINT64(0x3e900000,value(id,"sky.ground_r_bits"));
    TEST_ASSERT_EQUAL_UINT(1,allocations); TEST_ASSERT_EQUAL_UINT(1,uniforms); TEST_ASSERT_EQUAL_UINT(1,vertex_binds);
    TEST_ASSERT_EQUAL_UINT(1,index_binds); TEST_ASSERT_EQUAL_UINT(1,states); TEST_ASSERT_EQUAL_UINT(1,transforms); TEST_ASSERT_EQUAL_UINT(1,submits);
    TEST_ASSERT_EQUAL_UINT16(120,submitted_view); TEST_ASSERT_EQUAL_UINT16(0,submitted_program.idx);
    TEST_ASSERT_EQUAL_UINT64(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_MSAA,submitted_state);
    const float v[12]={-1,-1,0,1,-1,0,1,1,0,-1,1,0}; const uint16_t i[6]={0,1,2,0,2,3};
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(v,vertices,12); TEST_ASSERT_EQUAL_UINT16_ARRAY(i,indices,6);
}
static void expect_program(int authored_mode, uint16_t program)
{
    TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,authored_mode));
    outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT16(program,submitted_program.idx);
    TEST_ASSERT_EQUAL_UINT64(program,value(packet(),"sky.raw_program_idx"));
}
static void only_gl21_authored_physical_selects_the_dedicated_program(void)
{
    renderer.prog_sky_physical_compat.idx=17u;
    renderer.sky_mode=JCE_SCENE_SKY_PREETHAM;
    renderer.sky_legacy_gl21=true;
    expect_program(JCE_SCENE_SKY_PHYSICAL,17u);
    expect_program(JCE_SCENE_SKY_PREETHAM,0u);
    renderer.sky_mode=JCE_SCENE_SKY_STYLIZED;
    expect_program(JCE_SCENE_SKY_STYLIZED,0u);

    renderer.sky_legacy_gl21=false;
    renderer.sky_mode=JCE_SCENE_SKY_PHYSICAL;
    expect_program(JCE_SCENE_SKY_PHYSICAL,0u);

    renderer.sky_legacy_gl21=true;
    renderer.sky_mode=JCE_SCENE_SKY_PREETHAM;
    renderer.prog_sky_physical_compat.idx=UINT16_MAX;
    expect_program(JCE_SCENE_SKY_PHYSICAL,0u);
    TEST_ASSERT_EQUAL_UINT(5,submits);
}
static void default_diag_off_keeps_same_backend_work_without_events(void)
{
    diag=false; TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3)); drain();
    TEST_ASSERT_EQUAL_UINT32(0,event_count); TEST_ASSERT_EQUAL_UINT(1,allocations); TEST_ASSERT_EQUAL_UINT(1,uniforms); TEST_ASSERT_EQUAL_UINT(1,submits);
}
static void trace_off_keeps_same_backend_work_without_events(void)
{
    jce_trace_set_enabled(false); TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3)); drain();
    TEST_ASSERT_EQUAL_UINT32(0,event_count); TEST_ASSERT_EQUAL_UINT(1,allocations); TEST_ASSERT_EQUAL_UINT(1,submits);
}
static void capture_and_view_identities_remain_distinct_without_pointers(void)
{
    TEST_ASSERT_TRUE(run_pass(&renderer,3,&config,-1)); outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    uint64_t first=packet(),gen=value(first,"sky.capture_generation");
    TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3)); outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_TRUE(packet()!=first); TEST_ASSERT_EQUAL_UINT64(120,value(packet(),"sky.view"));
    jce_trace_reset(); TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3)); outcome(SR_SKY_SUBMIT_RETURNED,JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_TRUE(value(packet(),"sky.capture_generation")!=gen); TEST_ASSERT_EQUAL_UINT(3,submits);
}
static void suppressed_trace_does_not_change_drawing_or_leak_a_scope(void)
{
    TEST_ASSERT_TRUE(jce_trace_suppress_begin()); TEST_ASSERT_TRUE(run_pass(&renderer,120,&config,3));
    TEST_ASSERT_TRUE(jce_trace_suppress_end()); drain(); TEST_ASSERT_EQUAL_UINT32(0,event_count); TEST_ASSERT_EQUAL_UINT(1,submits);
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(caller_disabled_is_a_successful_policy_skip); RUN_TEST(both_actual_wireframe_gates_are_distinct);
    RUN_TEST(textured_wireframe_with_hdr_keeps_legacy_submit); RUN_TEST(invalid_sentinel_is_not_gpu_lifetime_or_success);
    RUN_TEST(transient_refusal_retains_legacy_sky_drawn_without_false_submit); RUN_TEST(successful_actual_tail_keeps_geometry_state_and_exact_uniform_bits);
    RUN_TEST(only_gl21_authored_physical_selects_the_dedicated_program);
    RUN_TEST(default_diag_off_keeps_same_backend_work_without_events); RUN_TEST(trace_off_keeps_same_backend_work_without_events);
    RUN_TEST(capture_and_view_identities_remain_distinct_without_pointers); RUN_TEST(suppressed_trace_does_not_change_drawing_or_leak_a_scope);
    return UNITY_END();
}
