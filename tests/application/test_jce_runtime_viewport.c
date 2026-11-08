/* Actual runtime + Lua host: presentation is available before on_start. */
#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include "unity.h"
#include <math.h>
#include <string.h>
#ifndef JCE_VIEWPORT_TEST_SCRIPT_FILE
#define JCE_VIEWPORT_TEST_SCRIPT_FILE "jce_runtime_viewport_probe.lua"
#endif
#define SCRIPT_FILE JCE_VIEWPORT_TEST_SCRIPT_FILE
static const char source[] =
    "local M = {}\n"
    "function M:on_start() self.resizes=0; self.updates=0; self:publish() end\n"
    "function M:publish() local w,h=jce.get_viewport_size(); jce.set_position(self.entity,w,h,self.resizes); jce.set_scale(self.entity,self.updates+1,1,1) end\n"
    "function M:on_viewport_resize() self.resizes=self.resizes+1; self:publish() end\n"
    "function M:on_update(dt) self.updates=self.updates+1; local x,y,w,h=jce.get_pointer_position(); assert(x==0 and y==0 and w==0 and h==0); self:publish() end\n"
    "return M\n";
static JceScene *scene;
static JceEntity entity;
static JceRuntime *active_runtime;
void setUp(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(SCRIPT_FILE,source,sizeof source-1u));
    scene=jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    entity=jce_scene_create_entity(scene,"viewport-probe");
    JceTransform transform={0}; transform.rotation.w=1;
    transform.scale.x=transform.scale.y=transform.scale.z=1;
    transform.position.x=12; transform.position.y=13; transform.position.z=14;
    jce_scene_set_transform(scene,entity,&transform);
    JceScriptComponent script={0};
    strncpy(script.script_path,SCRIPT_FILE,sizeof script.script_path-1u);
    jce_scene_set_script(scene,entity,&script);
    jce_scene_set_component_enabled(scene,entity,JCE_COMP_FLAG_SCRIPT,true);
}
void tearDown(void)
{
    if (active_runtime) {jce_runtime_destroy(active_runtime);active_runtime=NULL;}
    jce_scene_destroy(scene); scene=NULL; jce_fs_host_remove_file(SCRIPT_FILE);
}
static JceRuntime *create(const JceRuntimeCreateOptions *options,size_t size)
{
    JceRuntimeDesc desc={0};desc.scene=scene;desc.enable_physics=false;
    active_runtime=jce_runtime_create_with_options(&desc,NULL,options,size);
    return active_runtime;
}
static void observed(float width,float height,float notifications,float updates)
{
    const JceTransform *t=jce_scene_get_transform(scene,entity);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_FLOAT(width,t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(height,t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(notifications,t->position.z);
    TEST_ASSERT_EQUAL_FLOAT(updates+1,t->scale.x);
}
static void initial_extent_precedes_on_start_and_outside_pointer_steps(void)
{
    JceRuntimeCreateOptions initial={909,650};JceRuntime *rt=create(&initial,sizeof initial);
    TEST_ASSERT_NOT_NULL(rt);observed(909,650,0,0);
    jce_runtime_set_pointer_position(rt,0,0,0,0);
    jce_runtime_step(rt,1.0f/60.0f);observed(909,650,0,1);
    jce_runtime_step(rt,1.0f/60.0f);observed(909,650,0,2);
    jce_runtime_destroy(rt);active_runtime=NULL;
}
static void resize_notifies_layout_without_a_simulation_tick(void)
{
    JceRuntimeCreateOptions initial={100,80};JceRuntime *rt=create(&initial,sizeof initial);
    TEST_ASSERT_NOT_NULL(rt);observed(100,80,0,0);
    TEST_ASSERT_TRUE(jce_runtime_set_viewport_size(rt,500,250));observed(500,250,1,0);
    TEST_ASSERT_TRUE(jce_runtime_set_viewport_size(rt,500,250));observed(500,250,1,0);
    TEST_ASSERT_TRUE(jce_runtime_set_viewport_size(rt,0,0));observed(0,0,2,0);
    jce_runtime_destroy(rt);active_runtime=NULL;
}
static void invalid_values_clear_extent_without_making_pointer_valid(void)
{
    JceRuntimeCreateOptions initial={640,360};JceRuntime *rt=create(&initial,sizeof initial);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_FALSE(jce_runtime_set_viewport_size(rt,NAN,360));observed(0,0,1,0);
    TEST_ASSERT_FALSE(jce_runtime_set_viewport_size(rt,640,0));observed(0,0,1,0);
    TEST_ASSERT_FALSE(jce_runtime_set_viewport_size(rt,65536,360));observed(0,0,1,0);
    TEST_ASSERT_TRUE(jce_runtime_set_viewport_size(rt,320,180));observed(320,180,2,0);
    jce_runtime_step(rt,1.0f/60.0f);observed(320,180,2,1);
    TEST_ASSERT_FALSE(jce_runtime_set_viewport_size(NULL,320,180));
    jce_runtime_destroy(rt);active_runtime=NULL;
}
static void short_or_invalid_options_fail_before_on_start(void)
{
    JceRuntimeCreateOptions initial={640,360};
    TEST_ASSERT_NULL(create(&initial,sizeof(float)));
    TEST_ASSERT_NULL(create(NULL,sizeof initial));
    initial.viewport_width=INFINITY;TEST_ASSERT_NULL(create(&initial,sizeof initial));
    initial.viewport_width=-1;TEST_ASSERT_NULL(create(&initial,sizeof initial));
    initial.viewport_width=640;initial.viewport_height=0;TEST_ASSERT_NULL(create(&initial,sizeof initial));
    const JceTransform *t=jce_scene_get_transform(scene,entity);
    TEST_ASSERT_EQUAL_FLOAT(12,t->position.x);TEST_ASSERT_EQUAL_FLOAT(13,t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(14,t->position.z);
}
static void legacy_and_unknown_options_tail_keep_their_declared_contract(void)
{
    struct {JceRuntimeCreateOptions known;unsigned char poison[32];} longer;
    memset(&longer,0xff,sizeof longer);longer.known=(JceRuntimeCreateOptions){1024,512};
    JceRuntime *rt=create(&longer.known,sizeof longer);
    TEST_ASSERT_NOT_NULL(rt);observed(1024,512,0,0);jce_runtime_destroy(rt);active_runtime=NULL;
    rt=create(NULL,0);TEST_ASSERT_NOT_NULL(rt);observed(0,0,0,0);jce_runtime_destroy(rt);active_runtime=NULL;
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(initial_extent_precedes_on_start_and_outside_pointer_steps);
    RUN_TEST(resize_notifies_layout_without_a_simulation_tick);
    RUN_TEST(invalid_values_clear_extent_without_making_pointer_valid);
    RUN_TEST(short_or_invalid_options_fail_before_on_start);
    RUN_TEST(legacy_and_unknown_options_tail_keep_their_declared_contract);
    return UNITY_END();
}
