/* Complete observer helper + moved actual request/admission bodies and exact
 * draw-loop begin/branch/empty/tail wires. Typed CPU ports, no scene/GPU boot. */
/* Select the actual option-on draw macros locally; counted profiler ports do
 * not claim that the linked Core/Tracy bridge was compiled or executed on. */
#ifdef JCE_TRACY_ENABLED
#undef JCE_TRACY_ENABLED
#endif
#define JCE_TRACY_ENABLED 1
#include "unity.h"
#include <jce/os/core/jce_profiler.h>
#include "os/core/jce_trace_scope_internal.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <string.h>
#include "middleware/scene/jce_sr_grass_mask.h"
static bool refuse_scope;
static unsigned scope_refusals;
static JceTraceScope fixture_scope_begin(const char *name)
{ if (refuse_scope) { ++scope_refusals; return (JceTraceScope){0}; } return jce_trace_scope_begin(name); }
#define jce_trace_scope_begin fixture_scope_begin
#include "middleware/scene/jce_sr_grass_gate_internal.h"
#undef jce_trace_scope_begin
typedef uint64_t JceEntity;
typedef struct { bool visible; char density_mask_path[8]; } JceGrassFieldComponent;
typedef struct JceScene { bool has[5], enabled[5], component[5]; JceGrassFieldComponent fields[5]; } JceScene;
typedef struct { int unused; } JceCamera;
typedef struct { int count; JceEntity *entities; } EntityList;
typedef struct { bool draw_opaque; } JceSceneRenderConfig;
typedef struct JceSceneRenderer {
    bool grass_enabled, grass_prog_tried, fp_active_frame;
    struct { uint16_t idx; } prog_grass;
    uint32_t grass_gate_reported_reasons;
    uint64_t grass_resource_generation;
    SrGrassGateTrace grass_gate_trace;
    struct { uint8_t colour_special, layer; } ecull[5];
    uint8_t model_receiver_layer;
    void *renderer;
} JceSceneRenderer;
static JceSceneRenderer renderer;
static JceScene scene;
static JceSceneRenderConfig config;
static JceEntity entities[] = {UINT64_C(0x1234567800000001), UINT64_C(0x1234567800000002), UINT64_C(0x1234567800000003)};
static bool kill, perf_enabled;
static uint32_t caps;
static JceGpuTier tier;
static JceRendererBackend backend;
static unsigned draws, logs, perf_samples, clock_reads;
static unsigned zone_begins, zone_ends;
static bool zone_active;
static JceProfileZone profile_begin_port(const char *name,uint32_t color,
    const char *file,uint32_t line,const char *function)
{
    TEST_ASSERT_EQUAL_STRING("SceneDraw::Prepare",name);TEST_ASSERT_EQUAL_UINT32(0,color);
    TEST_ASSERT_NOT_NULL(file);TEST_ASSERT_GREATER_THAN_UINT32(0,line);TEST_ASSERT_NOT_NULL(function);
    TEST_ASSERT_FALSE(zone_active);zone_active=true;++zone_begins;
    return (JceProfileZone){17u,1};
}
static void profile_end_port(JceProfileZone zone)
{
    TEST_ASSERT_TRUE(zone_active);TEST_ASSERT_EQUAL_UINT32(17,zone.id);TEST_ASSERT_EQUAL_INT(1,zone.active);
    zone_active=false;++zone_ends;
}
static unsigned index_of(JceEntity entity) { unsigned i = (unsigned)(entity & 0xffu); TEST_ASSERT_TRUE(i < 5); return i; }
static bool sr_grass_kill_enabled(void) { return kill; }
static bool sr_grass_expanded_mode(void) { return false; }
static int jce_component_find(const char *name) { TEST_ASSERT_EQUAL_STRING("GrassField", name); return 13; }
static bool jce_scene_comp_enabled(JceScene *s,JceEntity e,int id) { TEST_ASSERT_EQUAL_INT(13,id);return s->component[index_of(e)]; }
static bool entity_enabled(JceScene *s,JceEntity e) { return s->enabled[index_of(e)]; }
static JceGrassFieldComponent *jce_scene_get_grass_field(JceScene *s,JceEntity e)
{ return s->has[index_of(e)] ? &s->fields[index_of(e)] : NULL; }
static bool jce_scene_has_grass_field(JceScene *s,JceEntity e) { return s->has[index_of(e)]; }
static uint32_t fixture_caps(void) { return caps; }
static JceGpuTier fixture_tier(void) { return tier; }
static JceRendererBackend fixture_backend(void) { return backend; }
static int jce_perf_phase_enabled(void) { return perf_enabled; }
static uint64_t fixture_clock(void) { return ++clock_reads; }
static double fixture_ms(uint64_t a,uint64_t b) { TEST_ASSERT_TRUE(b >= a); return (double)(b-a); }
static void jce_perf_phase_add(const char *name,double ms) { TEST_ASSERT_EQUAL_STRING("sr_grass",name);TEST_ASSERT_TRUE(ms>=0);++perf_samples; }
static void sr_draw_grass(JceSceneRenderer *sr,JceScene *s,EntityList *list,JceEntity e,
    uint16_t view,const JceCamera *camera,const JceSceneRenderConfig *cfg,SrGrassMaskBatch *batch)
{ (void)sr;(void)s;(void)list;(void)e;(void)view;(void)camera;TEST_ASSERT_NOT_NULL(batch);TEST_ASSERT_TRUE(cfg->draw_opaque);++draws; }
void sr_grass_mask_batch_end(SrGrassMaskBatch *batch) { TEST_ASSERT_NULL(batch->entry); }
static void sr_draw_foliage(JceSceneRenderer *sr,JceScene *s,EntityList *list,JceEntity e,
    uint16_t view,const JceCamera *camera)
{ (void)sr;(void)s;(void)list;(void)e;(void)view;(void)camera;TEST_FAIL_MESSAGE("Unexpected scatter in grass-only fixture"); }
static void sr_draw_foliage_cluster(JceSceneRenderer *sr,JceScene *s,EntityList *list,JceEntity e,uint16_t view,const JceCamera *camera)
{ (void)sr;(void)s;(void)list;(void)e;(void)view;(void)camera;TEST_FAIL_MESSAGE("Unexpected cluster in grass-only fixture"); }
static void sr_draw_line_renderer(JceSceneRenderer *sr,JceScene *s,EntityList *list,JceEntity e,
    const JceCamera *camera,uint16_t view)
{ (void)sr;(void)s;(void)list;(void)e;(void)camera;(void)view;TEST_FAIL_MESSAGE("Unexpected line in grass-only fixture"); }
static void sr_draw_trail_renderer(JceSceneRenderer *sr,JceScene *s,EntityList *list,JceEntity e,
    const JceCamera *camera,uint16_t view)
{ (void)sr;(void)s;(void)list;(void)e;(void)camera;(void)view;TEST_FAIL_MESSAGE("Unexpected trail in grass-only fixture"); }
static void sr_draw_water(JceSceneRenderer *sr,JceScene *s,const JceCamera *camera,
    EntityList *list,JceEntity e,uint16_t view,uint32_t *water_vertices)
{ (void)sr;(void)s;(void)camera;(void)list;(void)e;(void)view;(void)water_vertices;TEST_FAIL_MESSAGE("Unexpected water in grass-only fixture"); }
static void fixture_forwardplus(void *r,bool enabled) { (void)r;TEST_ASSERT_FALSE(enabled); }
#define jce_renderer_get_caps fixture_caps
#define jce_renderer_get_tier fixture_tier
#define jce_renderer_get_active_backend fixture_backend
#define jce_renderer_set_forwardplus_program_active fixture_forwardplus
#define jce_time_perf_counter fixture_clock
#define jce_time_perf_to_ms fixture_ms
#define LOG_TAG "scene_renderer"
#define LOG_INFO(...) (++logs)
#define jce_profile_zone_begin profile_begin_port
#define jce_profile_zone_end profile_end_port
#include JCE_TEST_GRASS_GATE_ZONES
#include JCE_TEST_GRASS_GATE_OWNERS
#include JCE_TEST_GRASS_GATE_PHASE
static void run_pass(JceSceneRenderer *sr,JceScene *scene,uint16_t view_id,int count)
{
    EntityList data={count,entities}; EntityList *list=&data;
    const JceCamera *camera=NULL; const JceSceneRenderConfig *cfg=&config;
#include JCE_TEST_GRASS_GATE_ENTRY
#include JCE_TEST_GRASS_GATE_BEGIN
#include JCE_TEST_GRASS_GATE_EMPTY
#include JCE_TEST_GRASS_GATE_PRE_END
    for (int i=0;i<list->count;++i) {
        JceEntity e=list->entities[i];
#include JCE_TEST_GRASS_GATE_BRANCH
    }
#include JCE_TEST_GRASS_GATE_SUBMIT
#include JCE_TEST_GRASS_GATE_PUBLISH
}
#undef jce_time_perf_counter
#undef jce_time_perf_to_ms
static JceTraceEvent events[2048];
static uint32_t event_count;
static void drain(void)
{
    JceTraceCursor c;jce_trace_cursor_init(&c);event_count=jce_trace_read(&c,events,2048);
    TEST_ASSERT_TRUE(event_count<2048);TEST_ASSERT_EQUAL_UINT64(0,c.lost_events);
    JceTraceStats stats;jce_trace_get_stats(&stats);TEST_ASSERT_EQUAL_UINT32(0,stats.active_work_items);
}
static unsigned packets(const char *name)
{ unsigned n=0;for(uint32_t i=0;i<event_count;++i)if(events[i].type==JCE_TRACE_EVENT_TASK_END&&!strcmp(events[i].name,name))++n;return n; }
static int64_t value(uint64_t id,const char *name)
{
    for(uint32_t i=0;i<event_count;++i)if(events[i].type==JCE_TRACE_EVENT_COUNTER&&events[i].id==id&&!strcmp(events[i].name,name))return (int64_t)events[i].value_u64;
    TEST_FAIL_MESSAGE(name);return INT64_MIN;
}
static uint64_t latest(uint16_t view,const char *name)
{
    for(uint32_t i=event_count;i>0;--i)if(events[i-1].type==JCE_TRACE_EVENT_TASK_END&&!strcmp(events[i-1].name,name)){
        uint64_t id=events[i-1].id;if(value(id,"grass.early.view")==view)return id;
    }
    TEST_FAIL_MESSAGE("Missing actual current-capture grass packet");return 0;
}
static void outcome(uint64_t id,JceTraceTaskState state)
{ for(uint32_t i=0;i<event_count;++i)if(events[i].type==JCE_TRACE_EVENT_TASK_END&&events[i].id==id){TEST_ASSERT_EQUAL_UINT32(state,events[i].value_u32[0]);return;}TEST_FAIL_MESSAGE("Missing terminal"); }
void setUp(void)
{
    jce_trace_set_enabled(false);jce_trace_reset();jce_trace_task_restore_id(0);
    memset(&renderer,0,sizeof(renderer));memset(&scene,0,sizeof(scene));
    for(unsigned i=0;i<5;++i){scene.has[i]=scene.enabled[i]=scene.component[i]=scene.fields[i].visible=true;}
    renderer.grass_enabled=true;renderer.prog_grass.idx=UINT16_MAX;
    config.draw_opaque=true;kill=perf_enabled=refuse_scope=false;
    caps=JCE_CAP_INSTANCING;tier=JCE_GPU_TIER_MEDIUM;backend=JCE_BACKEND_NOOP;
    draws=logs=perf_samples=clock_reads=scope_refusals=0;
    zone_begins=zone_ends=0;zone_active=false;
}
void tearDown(void) { jce_trace_set_enabled(false);jce_trace_task_restore_id(0); }
static void first_capture_after_lifetime_history_still_has_current_failure(void)
{
    renderer.grass_enabled=false;run_pass(&renderer,&scene,2,1);
    TEST_ASSERT_EQUAL_UINT32(16,renderer.grass_gate_reported_reasons);TEST_ASSERT_EQUAL_UINT(1,logs);
    jce_trace_set_enabled(true);jce_trace_task_restore_id(91);run_pass(&renderer,&scene,2,1);drain();
    TEST_ASSERT_EQUAL_UINT(1,packets("sr.grass.early_gate_pass"));
    uint64_t id=latest(2,"sr.grass.early_gate_pass");
    TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.project"));TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.refused"));
    TEST_ASSERT_EQUAL_INT(jce_trace_capture_generation(),value(id,"grass.early.capture_generation"));
    TEST_ASSERT_EQUAL_UINT64(91,jce_trace_task_current_id());outcome(id,JCE_TRACE_TASK_FAILED);
    TEST_ASSERT_EQUAL_UINT(1,logs);TEST_ASSERT_EQUAL_UINT(0,draws);
}
static void each_actual_gate_and_combination_is_current_not_first_reason_only(void)
{
    const char *names[]={"grass.early.kill","grass.early.component","grass.early.caps","grass.early.tier","grass.early.project"};
    jce_trace_set_enabled(true);
    for(unsigned bit=0;bit<5;++bit){kill=bit==0;scene.component[1]=bit!=1;caps=bit==2?0:JCE_CAP_INSTANCING;
        tier=bit==3?JCE_GPU_TIER_LOW:JCE_GPU_TIER_MEDIUM;renderer.grass_enabled=bit!=4;
        run_pass(&renderer,&scene,2,1);drain();uint64_t id=latest(2,"sr.grass.early_gate_pass");
        for(unsigned at=0;at<5;++at)TEST_ASSERT_EQUAL_INT(at==bit,value(id,names[at]));
        TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.admitted"));outcome(id,JCE_TRACE_TASK_FAILED);
    }
    kill=true;caps=0;tier=JCE_GPU_TIER_LOW;scene.component[1]=false;renderer.grass_enabled=false;
    run_pass(&renderer,&scene,2,3);drain();uint64_t id=latest(2,"sr.grass.early_gate_pass");
    TEST_ASSERT_EQUAL_INT(3,value(id,"grass.early.branch_requests"));TEST_ASSERT_EQUAL_INT(3,value(id,"grass.early.project"));
    TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.component"));TEST_ASSERT_EQUAL_INT(3,value(id,"grass.early.kill"));
    TEST_ASSERT_EQUAL_UINT32(0x12345678u,(uint32_t)value(id,"grass.early.project_first_hi"));
    TEST_ASSERT_EQUAL_UINT32(1,(uint32_t)value(id,"grass.early.project_first_lo"));
}
static void unchanged_views_are_silent_and_changed_counts_witness_scene_are_exact(void)
{
    renderer.grass_enabled=false;jce_trace_set_enabled(true);
    for(unsigned repeat=0;repeat<16;++repeat){run_pass(&renderer,&scene,2,1);run_pass(&renderer,&scene,3,1);}
    drain();TEST_ASSERT_EQUAL_UINT(2,packets("sr.grass.early_gate_pass"));
    run_pass(&renderer,&scene,2,2);drain();TEST_ASSERT_EQUAL_UINT(3,packets("sr.grass.early_gate_pass"));
    SrGrassGatePass a={0},b={0};a.scene=b.scene=&scene;
    sr_grass_gate_pass_add(&a,entities[0],16,false);sr_grass_gate_pass_add(&b,entities[1],16,false);
    TEST_ASSERT_FALSE(sr_grass_gate_pass_same(&a,&b));
    sr_grass_gate_pass_publish(&renderer.grass_gate_trace,&b,&renderer,2,0);drain();
    TEST_ASSERT_EQUAL_UINT(4,packets("sr.grass.early_gate_pass"));
    JceScene other=scene;run_pass(&renderer,&other,2,1);drain();
    TEST_ASSERT_EQUAL_UINT(5,packets("sr.grass.early_gate_pass"));
}
static void actual_empty_and_opaque_branches_publish_without_inventing_requests(void)
{
    jce_trace_set_enabled(true);run_pass(&renderer,&scene,2,1);run_pass(&renderer,&scene,2,0);drain();
    uint64_t id=latest(2,"sr.grass.early_gate_pass");TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.branch_requests"));
    TEST_ASSERT_EQUAL_UINT(2,packets("sr.grass.early_gate_pass"));
    config.draw_opaque=false;run_pass(&renderer,&scene,2,3);drain();id=latest(2,"sr.grass.early_gate_pass");
    TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.opaque_pass"));TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.branch_requests"));
    TEST_ASSERT_EQUAL_UINT(1,draws);
}
static void other_refusal_is_not_misattributed_and_gles_exception_keeps_draw_policy(void)
{
    scene.fields[1].visible=false;jce_trace_set_enabled(true);run_pass(&renderer,&scene,2,1);drain();
    uint64_t id=latest(2,"sr.grass.early_gate_pass");TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.other_refused"));
    TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.project"));TEST_ASSERT_EQUAL_UINT(0,logs);
    scene.fields[1].visible=true;tier=JCE_GPU_TIER_LOW;backend=JCE_BACKEND_OPENGLES;
    run_pass(&renderer,&scene,2,1);drain();id=latest(2,"sr.grass.early_gate_pass");
    TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.admitted"));TEST_ASSERT_EQUAL_INT(0,value(id,"grass.early.tier"));
    TEST_ASSERT_EQUAL_UINT(1,draws);outcome(id,JCE_TRACE_TASK_SUCCEEDED);
}
static void generations_rebaseline_without_clearing_lifetime_log_history(void)
{
    renderer.grass_enabled=false;jce_trace_set_enabled(true);run_pass(&renderer,&scene,2,1);drain();
    TEST_ASSERT_EQUAL_UINT(1,packets("sr.grass.early_gate_pass"));
    ++renderer.grass_resource_generation;run_pass(&renderer,&scene,2,1);drain();
    TEST_ASSERT_EQUAL_UINT(2,packets("sr.grass.early_gate_pass"));TEST_ASSERT_EQUAL_UINT(1,logs);
    jce_trace_reset();run_pass(&renderer,&scene,2,1);drain();TEST_ASSERT_EQUAL_UINT(1,packets("sr.grass.early_gate_pass"));
    TEST_ASSERT_EQUAL_INT(1,value(latest(2,"sr.grass.early_gate_pass"),"grass.early.resource_generation_lo"));
}
static void disabled_suppressed_and_refused_keep_first_or_changed_packet_pending(void)
{
    run_pass(&renderer,&scene,2,1);TEST_ASSERT_FALSE(renderer.grass_gate_trace.epoch_published);
    jce_trace_set_enabled(true);refuse_scope=true;run_pass(&renderer,&scene,2,1);
    TEST_ASSERT_FALSE(renderer.grass_gate_trace.epoch_published);TEST_ASSERT_EQUAL_UINT(1,scope_refusals);
    refuse_scope=false;run_pass(&renderer,&scene,2,1);drain();TEST_ASSERT_EQUAL_UINT(1,packets("sr.grass.early_gate_pass"));
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());renderer.grass_enabled=false;run_pass(&renderer,&scene,2,1);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());run_pass(&renderer,&scene,2,1);drain();
    TEST_ASSERT_EQUAL_UINT(2,packets("sr.grass.early_gate_pass"));
    jce_trace_set_enabled(false);run_pass(&renderer,&scene,2,1);jce_trace_set_enabled(true);
    run_pass(&renderer,&scene,2,1);drain();TEST_ASSERT_EQUAL_UINT(3,packets("sr.grass.early_gate_pass"));
}
static void eight_views_then_once_only_explicit_overflow_and_fresh_epoch(void)
{
    jce_trace_set_enabled(true);for(uint16_t view=0;view<8;++view)run_pass(&renderer,&scene,view,1);
    refuse_scope=true;run_pass(&renderer,&scene,8,1);TEST_ASSERT_FALSE(renderer.grass_gate_trace.overflow_published);
    refuse_scope=false;for(uint16_t view=8;view<16;++view)run_pass(&renderer,&scene,view,1);
    drain();TEST_ASSERT_EQUAL_UINT(8,packets("sr.grass.early_gate_pass"));TEST_ASSERT_EQUAL_UINT(1,packets("sr.grass.early_gate_overflow"));
    uint64_t id=latest(8,"sr.grass.early_gate_overflow");TEST_ASSERT_EQUAL_INT(1,value(id,"grass.early.coverage_truncated"));
    TEST_ASSERT_EQUAL_INT(8,value(id,"grass.early.covered_views"));TEST_ASSERT_EQUAL_UINT(8,renderer.grass_gate_trace.used);
    ++renderer.grass_resource_generation;run_pass(&renderer,&scene,15,1);drain();
    TEST_ASSERT_EQUAL_UINT(9,packets("sr.grass.early_gate_pass"));TEST_ASSERT_EQUAL_UINT(1,renderer.grass_gate_trace.used);
    TEST_ASSERT_FALSE(renderer.grass_gate_trace.overflow_published);
}
static void trace_off_keeps_draw_log_and_perf_order_without_observer_clock(void)
{
    perf_enabled=true;run_pass(&renderer,&scene,2,1);TEST_ASSERT_EQUAL_UINT(1,draws);
    TEST_ASSERT_EQUAL_UINT(2,clock_reads);TEST_ASSERT_EQUAL_UINT(1,perf_samples);
    renderer.grass_enabled=false;run_pass(&renderer,&scene,2,1);run_pass(&renderer,&scene,2,1);
    TEST_ASSERT_EQUAL_UINT(1,draws);TEST_ASSERT_EQUAL_UINT(1,logs);TEST_ASSERT_EQUAL_UINT(2,clock_reads);
    drain();TEST_ASSERT_EQUAL_UINT32(0,event_count);TEST_ASSERT_FALSE(renderer.grass_gate_trace.epoch_published);
}
static void actual_empty_colour_return_closes_prepare_zone_once(void)
{
    for(unsigned i=0;i<2;++i){
        run_pass(&renderer,&scene,2,0);
        TEST_ASSERT_EQUAL_UINT(i+1,zone_begins);TEST_ASSERT_EQUAL_UINT(i+1,zone_ends);
        TEST_ASSERT_FALSE(zone_active);TEST_ASSERT_EQUAL_UINT(0,draws);
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(actual_empty_colour_return_closes_prepare_zone_once);
    RUN_TEST(first_capture_after_lifetime_history_still_has_current_failure);
    RUN_TEST(each_actual_gate_and_combination_is_current_not_first_reason_only);
    RUN_TEST(unchanged_views_are_silent_and_changed_counts_witness_scene_are_exact);
    RUN_TEST(actual_empty_and_opaque_branches_publish_without_inventing_requests);
    RUN_TEST(other_refusal_is_not_misattributed_and_gles_exception_keeps_draw_policy);
    RUN_TEST(generations_rebaseline_without_clearing_lifetime_log_history);
    RUN_TEST(disabled_suppressed_and_refused_keep_first_or_changed_packet_pending);
    RUN_TEST(eight_views_then_once_only_explicit_overflow_and_fresh_epoch);
    RUN_TEST(trace_off_keeps_draw_log_and_perf_order_without_observer_clock);
    return UNITY_END();
}
