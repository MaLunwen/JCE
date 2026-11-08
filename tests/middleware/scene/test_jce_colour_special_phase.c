/* Actual private types, staging owner, dispatch prefix and complete flush
 * orchestration. Typed ports observe CPU calls; no device or window starts. */
#include "unity.h"
#include "middleware/scene/jce_sr_grass_mask.h"
#include <jce/os/core/jce_env.h>
#include "jce_sr_internal.h"
#include "jce_sr_nodraw.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct JceScene { int unused; };
enum { N = 12, SCATTER=1, CLUSTER=2, GRASS=4, LINE=8, TRAIL=16, WATER=32,
    TERRAIN=64, TILEMAP=128, MESH=256 };
enum { EV_MODEL=1, EV_TERRAIN, EV_TILEMAP, EV_PRIM, EV_TEX, EV_CROWD, EV_BINDPOSE,
    EV_INST, EV_IMPOSTOR, EV_OPAQUE_RQ, EV_SCATTER, EV_CLUSTER, EV_GRASS,
    EV_PUBLISH, EV_LINE, EV_TRAIL, EV_WATER, EV_TRANSPARENT_RQ };
static JceScene scene;
static const unsigned char camera_token;
static JceSceneRenderer renderer;
static SrEntityCull cull[N];
static JceEntity entities[N];
static uint8_t fast_bytes[N], visible_bytes[N];
static struct {
    unsigned kinds;
    bool enabled, components, transform, mesh_resolves;
    uint8_t layer;
    JceTerrainComponent terrain;
    JceTilemapComponent tilemap;
    JceMeshRenderer mesh;
} data[N];
static struct { unsigned kind; JceEntity entity; uint16_t view; } events[128];
static unsigned event_count, resource_calls, requests, begin_calls, publish_calls;
static unsigned registry_count, clock_calls;
static bool cache_on, all_visible, use_fast_bytes, initial_registry_current;
static JceSceneRenderConfig config;
static JceRenderQueue *const opaque_queue = (JceRenderQueue *)(uintptr_t)1;
static JceRenderQueue *const transparent_queue = (JceRenderQueue *)(uintptr_t)2;
static unsigned index_of(JceEntity e) { TEST_ASSERT_TRUE(e > 0 && e <= N); return (unsigned)e - 1; }
static void event(unsigned kind, JceEntity e, uint16_t view)
{
    TEST_ASSERT_LESS_THAN_UINT(128, event_count);
    events[event_count].kind=kind; events[event_count].entity=e;
    events[event_count++].view=view;
}
static bool has(const JceScene *s,JceEntity e,unsigned kind) { TEST_ASSERT_TRUE(s==&scene); return (data[index_of(e)].kinds & kind)!=0; }
static bool enabled_port(JceScene *s,JceEntity e) { (void)s;return data[index_of(e)].enabled; }
static uint32_t layer_port(const JceScene *s,JceEntity e) { (void)s;return data[index_of(e)].layer; }
static int component_count_port(void) { return (int)registry_count; }
static int component_find_port(const char *name) { TEST_ASSERT_NOT_NULL(name);return 1; }
static bool component_port(const JceScene *s,JceEntity e,int id) { (void)s;(void)id;return data[index_of(e)].components; }
static bool legacy_component_port(const JceScene *s,JceEntity e,uint64_t flag) { (void)flag;return component_port(s,e,0); }
static bool transform_port(const JceScene *s,JceEntity e) { (void)s;return data[index_of(e)].transform; }
static JceTerrainComponent *terrain_port(JceScene *s,JceEntity e) { (void)s;return &data[index_of(e)].terrain; }
static JceTilemapComponent *tilemap_port(JceScene *s,JceEntity e) { (void)s;return &data[index_of(e)].tilemap; }
static JceMeshRenderer *mesh_port(JceScene *s,JceEntity e) { (void)s;return &data[index_of(e)].mesh; }
static jce_mat4 world_port(const JceScene *s,JceEntity e) { (void)s;(void)e;return jce_m4_identity(); }
static JceMesh *resolve_port(JceSceneRenderer *sr,const JceMeshRenderer *mr)
{ (void)sr;++resource_calls;for(unsigned i=0;i<N;++i)if(mr==&data[i].mesh)return data[i].mesh_resolves?(JceMesh *)(uintptr_t)1:NULL;return NULL; }
static int terrain_load_port(JceSceneRenderer *sr,JceScene *s,const char *path) { (void)sr;(void)s;TEST_ASSERT_NOT_NULL(path);++resource_calls;return 0; }
static int tilemap_load_port(JceSceneRenderer *sr,const JceTilemapComponent *t) { (void)sr;TEST_ASSERT_NOT_NULL(t);++resource_calls;return 0; }
static void tilemap_build_port(JceSceneRenderer *sr,int slot,uint32_t rgba) { (void)sr;(void)rgba;TEST_ASSERT_EQUAL_INT(0,slot);++resource_calls; }
static uint32_t tilemap_px_port(const JceTilemapAsset *map) { (void)map;return 16; }
static uint32_t tilemap_color_port(const float *color) { TEST_ASSERT_NOT_NULL(color);return 0xffffffffu; }
static void terrain_draw_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,
    const JceCamera *c,uint16_t view,int slot,JceTerrainComponent *tc,const jce_mat4 *m,
    const JcePbrMaterial *p,uint32_t layer,JceSceneViewModeKind mode)
{ (void)sr;(void)s;(void)l;(void)e;(void)c;(void)slot;(void)tc;(void)m;(void)p;(void)layer;
  TEST_ASSERT_EQUAL_INT(JCE_SCENE_VIEW_SHADED,mode);event(EV_TERRAIN,0,view); }
static void tilemap_draw_port(JceSceneRenderer *sr,int slot,const JceCamera *c,uint16_t view,const jce_mat4 *m,uint32_t sort)
{ (void)sr;(void)slot;(void)c;(void)m;(void)sort;event(EV_TILEMAP,0,view); }
static void scatter_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,uint16_t view,const JceCamera *c)
{ (void)s;(void)l;(void)c;TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(SCATTER|CLUSTER|GRASS));TEST_ASSERT_EQUAL_UINT(data[index_of(e)].layer,sr->model_receiver_layer);event(EV_SCATTER,e,view); }
static void cluster_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,uint16_t view,const JceCamera *camera)
{ (void)s;(void)l;TEST_ASSERT_EQUAL_PTR(&camera_token,camera);TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(SCATTER|CLUSTER|GRASS));event(EV_CLUSTER,e,view); }
static void grass_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,uint16_t view,const JceCamera *c,const JceSceneRenderConfig *cfg,SrGrassGatePass *observation,SrGrassMaskBatch *batch)
{ (void)s;(void)l;(void)c;TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(SCATTER|CLUSTER|GRASS));TEST_ASSERT_TRUE(cfg->draw_opaque);TEST_ASSERT_NOT_NULL(observation);TEST_ASSERT_NOT_NULL(batch);++requests;event(EV_GRASS,e,view); }
static void line_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,const JceCamera *c,uint16_t view)
{ (void)s;(void)l;(void)c;TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(LINE|TRAIL|WATER));event(EV_LINE,e,view); }
static void trail_port(JceSceneRenderer *sr,JceScene *s,EntityList *l,JceEntity e,const JceCamera *c,uint16_t view)
{ (void)s;(void)l;(void)c;TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(LINE|TRAIL|WATER));event(EV_TRAIL,e,view); }
static void water_port(JceSceneRenderer *sr,JceScene *s,const JceCamera *c,EntityList *l,JceEntity e,uint16_t view,uint32_t *water_vertices)
{ (void)s;(void)c;(void)l;TEST_ASSERT_NOT_NULL(water_vertices);TEST_ASSERT_EQUAL_UINT(0,sr->ecull[index_of(e)].colour_special&(LINE|TRAIL|WATER));event(EV_WATER,e,view); }
static SrGrassGatePass *begin_port(SrGrassGatePass *p,const void *s,bool opaque)
{ (void)s;(void)opaque;++begin_calls;return p; }
static void publish_port(SrGrassGateTrace *t,const SrGrassGatePass *p,const void *owner,uint16_t view,uint64_t generation)
{ (void)t;(void)p;(void)owner;(void)generation;++publish_calls;event(EV_PUBLISH,0,view); }
static uint64_t clock_port(void) { return ++clock_calls; }
static double elapsed_port(uint64_t a,uint64_t b) { return (double)(b-a); }
static int perf_port(void) { return 0; }
static void phase_port(const char *name,double value) { (void)name;(void)value; }
static const char *env_port(const char *name) { TEST_ASSERT_EQUAL_STRING("JCE_NO_FOLIAGE",name);return NULL; }
static uint32_t rq_count_port(const JceRenderQueue *q) { TEST_ASSERT_TRUE(q==opaque_queue||q==transparent_queue);return 1; }
static void binder_port(JceRenderQueue *q,JceRqBindMaterialFn cb,void *user) { (void)q;TEST_ASSERT_NOT_NULL(cb);TEST_ASSERT_TRUE(user==&renderer); }
static void bind_callback_port(uint32_t key,void *user) { (void)key;(void)user; }
static void sort_port(JceRenderQueue *q,JceSortMode mode)
{ TEST_ASSERT_EQUAL_INT(q==opaque_queue?JCE_SORT_FOR_INSTANCING:JCE_SORT_BACK_TO_FRONT,mode); }
static void rq_opaque_port(JceSceneRenderer *sr) { (void)sr;event(EV_OPAQUE_RQ,0,renderer.frame_view_id); }
static void rq_transparent_port(JceSceneRenderer *sr,JceRenderQueue *q)
{ (void)sr;TEST_ASSERT_TRUE(q==transparent_queue);event(EV_TRANSPARENT_RQ,0,renderer.frame_view_id); }

#define entity_enabled enabled_port
#define jce_scene_get_entity_layer layer_port
#define jce_component_count component_count_port
#define jce_component_find component_find_port
#define jce_scene_comp_enabled component_port
#define jce_scene_component_enabled legacy_component_port
#define jce_scene_has_terrain(s,e) has(s,e,TERRAIN)
#define jce_scene_has_tilemap(s,e) has(s,e,TILEMAP)
#define jce_scene_has_mesh_renderer(s,e) has(s,e,MESH)
#define jce_scene_has_vegetation_scatter(s,e) has(s,e,SCATTER)
#define jce_scene_has_foliage_cluster(s,e) has(s,e,CLUSTER)
#define jce_scene_has_grass_field(s,e) has(s,e,GRASS)
#define jce_scene_has_line_renderer(s,e) has(s,e,LINE)
#define jce_scene_has_trail_renderer(s,e) has(s,e,TRAIL)
#define jce_scene_has_water(s,e) has(s,e,WATER)
#define jce_scene_has_transform transform_port
#define jce_scene_get_terrain terrain_port
#define jce_scene_get_tilemap tilemap_port
#define jce_scene_get_mesh_renderer mesh_port
#define jce_scene_get_world_matrix world_port
#define sr_resolve_mesh resolve_port
#define sr_terrain_find_or_load_slot terrain_load_port
#define sr_tilemap_find_or_load_slot tilemap_load_port
#define sr_tilemap_build_chunks tilemap_build_port
#define jce_tilemap_cell_px tilemap_px_port
#define sr_tilemap_color_abgr tilemap_color_port
#define sr_draw_terrain_chunks terrain_draw_port
#define sr_draw_tilemap_chunks tilemap_draw_port
#define sr_draw_foliage scatter_port
#define sr_draw_foliage_cluster cluster_port
#define sr_grass_draw_colour_request grass_port
#define sr_draw_line_renderer line_port
#define sr_draw_trail_renderer trail_port
#define sr_draw_water water_port
#define sr_grass_gate_pass_begin begin_port
#define sr_grass_gate_pass_publish publish_port
#define jce_time_perf_counter clock_port
#define jce_time_perf_to_ms elapsed_port
#define jce_perf_phase_enabled perf_port
#define jce_perf_phase_add phase_port
#define getenv env_port
#define SL_TSC() UINT64_C(0)
#undef LOG_TAG
#define LOG_TAG "colour-phase-fixture"
#undef LOG_WARN
#define LOG_WARN(...) ((void)0)
#define sr_prim_inst_flush(sr,v) event(EV_PRIM,0,v)
#define sr_tex_inst_flush(sr,v) event(EV_TEX,0,v)
#define sr_crowd_flush(sr,v) event(EV_CROWD,0,v)
#define sr_bindpose_flush(sr,v) event(EV_BINDPOSE,0,v)
#define sr_inst_flush(sr,v) event(EV_INST,0,v)
#define sr_impostor_flush(sr,v) event(EV_IMPOSTOR,0,v)
#define jce_rq_count rq_count_port
#define jce_rq_set_material_binder binder_port
#define sr_bind_material_cb bind_callback_port
#define jce_rq_sort sort_port
#define sr_rq_flush_and_collect rq_opaque_port
#define sr_rq_flush_queue_and_collect rq_transparent_port
#include JCE_TEST_COLOUR_LAYER_OWNER
#include JCE_TEST_COLOUR_SPECIAL_OWNER

static void run_pass(int count,uint16_t view_id,uint16_t transparent_view,bool use_rq)
{
    JceSceneRenderer *sr=&renderer;JceScene *scene_arg=&scene;
    EntityList storage={entities,count,N};EntityList *list=&storage;
    const JceCamera *camera=(const JceCamera *)&camera_token;const JceSceneRenderConfig *cfg=&config;
    const bool kc=cache_on;bool nodraw_current=initial_registry_current;
    const uint8_t *visible=visible_bytes;
    const int slp=0;uint64_t sl_c_skip=0,sl_n_skip=0;
    renderer.frame_view_id=view_id;
    renderer.ecull_fast_byte=use_fast_bytes?fast_bytes:NULL;
#define scene scene_arg
#include JCE_TEST_COLOUR_BEGIN_OWNER
#include JCE_TEST_COLOUR_DISPATCH_OWNER
        event(EV_MODEL,e,view_id); /* Rest of generic mesh path is outside scope. */
    }
#include JCE_TEST_COLOUR_FLUSH_OWNER
#undef scene
    (void)_t0_flush;(void)sl_c_skip;(void)sl_n_skip;
    for(int i=0;i<count;++i)TEST_ASSERT_EQUAL_UINT(0,cull[i].colour_special);
}
static unsigned count_kind(unsigned kind)
{ unsigned n=0;for(unsigned i=0;i<event_count;++i)n+=events[i].kind==kind;return n; }
static int position(unsigned kind)
{ for(unsigned i=0;i<event_count;++i)if(events[i].kind==kind)return (int)i;return -1; }
static void clear_events(void) { event_count=0;requests=begin_calls=publish_calls=resource_calls=clock_calls=0; }
void setUp(void)
{
    memset(&renderer,0,sizeof renderer);memset(cull,0,sizeof cull);memset(data,0,sizeof data);
    memset(&config,0,sizeof config);config.draw_opaque=config.draw_transparent=config.draw_sprites=true;
    for(unsigned i=0;i<N;++i){entities[i]=i+1;data[i].enabled=data[i].components=data[i].transform=true;
        cull[i].render_kind=SR_RK_OTHER;visible_bytes[i]=1;fast_bytes[i]=0;}
    renderer.ecull=cull;renderer.render_queue=opaque_queue;renderer.transparent_queue=transparent_queue;
    renderer.nodraw_registry_count=2;registry_count=2;cache_on=false;all_visible=false;
    use_fast_bytes=false;initial_registry_current=true;clear_events();
}
void tearDown(void) {}
static void test_actual_private_layout_uses_existing_tail_padding(void)
{
    struct Alignment { char first;SrEntityCull value; };
    const size_t alignment=offsetof(struct Alignment,value);
    const size_t old_end=offsetof(SrEntityCull,layer)+sizeof(cull[0].layer);
    const size_t old_rounded=(old_end+alignment-1)/alignment*alignment;
    printf("SrEntityCull layout: sizeof=%zu alignment=%zu layer=%zu colour_special=%zu old_rounded=%zu\n",
        sizeof(SrEntityCull),alignment,offsetof(SrEntityCull,layer),offsetof(SrEntityCull,colour_special),old_rounded);
    TEST_ASSERT_EQUAL_UINT64(old_end,offsetof(SrEntityCull,colour_special));
    TEST_ASSERT_EQUAL_UINT64(old_rounded,sizeof(SrEntityCull));
}
static void test_actual_dispatch_and_flush_order_and_single_requests(void)
{
    data[0].kinds=GRASS;data[1].kinds=WATER;data[2].kinds=SCATTER;data[3].kinds=CLUSTER;
    data[4].kinds=LINE|TRAIL;run_pass(5,30,125,true);
    TEST_ASSERT_TRUE(position(EV_OPAQUE_RQ)<position(EV_GRASS));
    TEST_ASSERT_TRUE(position(EV_OPAQUE_RQ)<position(EV_SCATTER));
    TEST_ASSERT_TRUE(position(EV_OPAQUE_RQ)<position(EV_CLUSTER));
    TEST_ASSERT_TRUE(position(EV_CLUSTER)<position(EV_WATER));
    TEST_ASSERT_TRUE(position(EV_PUBLISH)<position(EV_WATER));
    TEST_ASSERT_TRUE(position(EV_WATER)<position(EV_LINE));
    TEST_ASSERT_TRUE(position(EV_LINE)<position(EV_TRAIL));
    TEST_ASSERT_TRUE(position(EV_TRAIL)<position(EV_TRANSPARENT_RQ));
    TEST_ASSERT_EQUAL_UINT(30,events[position(EV_GRASS)].view);
    TEST_ASSERT_EQUAL_UINT(125,events[position(EV_WATER)].view);
    TEST_ASSERT_EQUAL_UINT(125,events[position(EV_LINE)].view);
    TEST_ASSERT_EQUAL_UINT(125,events[position(EV_TRAIL)].view);
    TEST_ASSERT_EQUAL_UINT(1,requests);TEST_ASSERT_EQUAL_UINT(1,begin_calls);TEST_ASSERT_EQUAL_UINT(1,publish_calls);
    clear_events();run_pass(5,120,120,false);
    TEST_ASSERT_TRUE(position(EV_IMPOSTOR)<position(EV_GRASS));
    TEST_ASSERT_TRUE(position(EV_CLUSTER)<position(EV_WATER));
    TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_OPAQUE_RQ));TEST_ASSERT_EQUAL_UINT(1,requests);
}
static void test_captured_priority_and_resource_work_are_not_replayed(void)
{
    data[0].kinds=TERRAIN|GRASS;data[0].terrain.visible=true;strcpy(data[0].terrain.terrain_path,"terrain");
    data[1].kinds=TILEMAP|GRASS;data[1].tilemap.visible=true;strcpy(data[1].tilemap.tilemap_path,"tilemap");
    data[2].kinds=SCATTER|LINE|WATER|CLUSTER|GRASS;
    data[3].kinds=WATER|CLUSTER|GRASS;data[4].kinds=CLUSTER|GRASS;data[5].kinds=LINE|GRASS;
    run_pass(6,30,30,true);
    TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_TERRAIN));TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_TILEMAP));
    TEST_ASSERT_EQUAL_UINT(4,resource_calls); /* Resolve(NULL), terrain load, tilemap load/build. */
    TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_SCATTER));TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_CLUSTER));
    TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_WATER));TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_LINE));
    TEST_ASSERT_EQUAL_UINT(1,requests);TEST_ASSERT_EQUAL_UINT64(6,events[position(EV_GRASS)].entity);
}
static void test_configuration_and_component_gates_keep_existing_priority(void)
{
    data[0].kinds=WATER|GRASS;data[1].kinds=SCATTER|LINE;data[2].kinds=CLUSTER;
    data[2].components=false;config.draw_transparent=false;run_pass(3,30,30,false);
    TEST_ASSERT_EQUAL_UINT(1,requests);TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_WATER));
    TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_CLUSTER));TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_SCATTER));
    clear_events();config.draw_opaque=false;config.draw_transparent=true;run_pass(3,30,30,false);
    TEST_ASSERT_EQUAL_UINT(0,requests);TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_WATER));
    TEST_ASSERT_EQUAL_UINT(1,count_kind(EV_LINE));TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_SCATTER));
    clear_events();config.draw_transparent=false;run_pass(3,30,30,false);
    TEST_ASSERT_EQUAL_UINT(0,requests);TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_LINE));
}
static void test_full_pass_consumes_marks_before_later_view_gates(void)
{
    for(unsigned i=0;i<6;++i)data[i].kinds=GRASS;
    run_pass(6,30,30,false);TEST_ASSERT_EQUAL_UINT(6,requests);
    for(unsigned i=0;i<6;++i)TEST_ASSERT_EQUAL_UINT(0,cull[i].colour_special);
    clear_events();
    data[0].enabled=false;data[1].layer=1;renderer.frame_culling_mask=1;
    cull[2].render_kind=SR_RK_NO_DRAW;cull[3].render_kind=SR_RK_PRIM_MESH;
    cull[4].render_kind=SR_RK_PRIM_MESH;fast_bytes[4]=1;visible_bytes[4]=0;
    data[3].kinds=data[4].kinds=MESH; /* Grass was actually removed for FAST rows. */
    cache_on=true;use_fast_bytes=true;cull[0].render_kind=SR_RK_DISABLED;
    run_pass(6,120,120,false);
    TEST_ASSERT_EQUAL_UINT(1,requests);TEST_ASSERT_EQUAL_UINT64(6,events[position(EV_GRASS)].entity);
    for(unsigned i=0;i<6;++i)TEST_ASSERT_EQUAL_UINT(0,cull[i].colour_special);
    clear_events();cache_on=false;data[0].enabled=false;run_pass(6,30,30,false);
    TEST_ASSERT_EQUAL_UINT(2,requests); /* current enabled/layer gates; kind cache disabled. */
    for(unsigned i=0;i<6;++i)TEST_ASSERT_EQUAL_UINT(0,cull[i].colour_special);
}
static void test_stale_no_draw_registry_fails_open_and_special_ignores_generic_aabb(void)
{
    data[0].kinds=GRASS;cull[0].render_kind=SR_RK_NO_DRAW;cull[0].world_valid=false;
    visible_bytes[0]=0;cache_on=true;initial_registry_current=false;
    run_pass(1,30,30,false);TEST_ASSERT_EQUAL_UINT(1,requests);
    clear_events();initial_registry_current=true;registry_count=3;
    run_pass(1,120,120,false);TEST_ASSERT_EQUAL_UINT(1,requests);
}
static void test_no_special_stages_do_not_visit_stale_membership(void)
{
    SrColourSpecialPass pass={0};EntityList list={entities,2,N};
    cull[0].colour_special=SR_COLOUR_GRASS|SR_COLOUR_LINE;
    sr_colour_special_submit_opaque(&renderer,&scene,NULL,&list,30,&config,&pass,NULL);
    sr_colour_special_submit_transparent(&renderer,&scene,NULL,&list,30,&pass);
    TEST_ASSERT_EQUAL_UINT(0,event_count);TEST_ASSERT_EQUAL_UINT(0,requests);
    /* Synthetic guard-only input is not a reachable completed-pass state. */
    cull[0].colour_special=0;
    data[0].kinds=MESH;cull[0].render_kind=SR_RK_PRIM_MESH;cache_on=true;
    run_pass(1,30,30,false);TEST_ASSERT_EQUAL_UINT(0,requests);TEST_ASSERT_EQUAL_UINT(0,count_kind(EV_LINE));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_private_layout_uses_existing_tail_padding);
    RUN_TEST(test_actual_dispatch_and_flush_order_and_single_requests);
    RUN_TEST(test_captured_priority_and_resource_work_are_not_replayed);
    RUN_TEST(test_configuration_and_component_gates_keep_existing_priority);
    RUN_TEST(test_full_pass_consumes_marks_before_later_view_gates);
    RUN_TEST(test_stale_no_draw_registry_fails_open_and_special_ignores_generic_aabb);
    RUN_TEST(test_no_special_stages_do_not_visit_stale_membership);
    return UNITY_END();
}
