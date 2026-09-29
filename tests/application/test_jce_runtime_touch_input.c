#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>

#include "unity.h"

#include <math.h>
#include <string.h>

#define TOUCH_SCRIPT_FILE "jce_rt_touch_input_test.lua"

static const char g_script[] =
	"local M = {}\n"
	"function M:on_start() self.step = 0 end\n"
	"function M:on_update(dt)\n"
	"  self.step = self.step + 1\n"
	"  local n = jce.get_touch_count()\n"
	"  if self.step == 1 then\n"
	"    assert(n == 5)\n"
	"    for i=1,5 do\n"
	"      local id,x,y,p = jce.get_touch(i)\n"
	"      assert(id == 100+i)\n"
	"      assert(math.abs(x-i/10) < 0.000001)\n"
	"      assert(math.abs(y-i/20) < 0.000001)\n"
	"      assert(math.abs(p-i/5) < 0.000001)\n"
	"    end\n"
	"  elseif self.step == 2 then\n"
	"    local id = jce.get_touch(1)\n"
	"    assert(n == 2 and id == 201)\n"
	"  elseif self.step == 3 then\n"
	"    local id = jce.get_touch(1)\n"
	"    assert(n == 1 and id == 301)\n"
	"  elseif self.step == 4 then assert(n == 0)\n"
	"  elseif self.step == 5 then assert(n == 1)\n"
	"  elseif self.step == 6 then assert(n == 0) end\n"
	"  jce.set_position(self.entity, self.step, n, 0)\n"
	"end\n"
	"return M\n";

void setUp(void)
{
	TEST_ASSERT_TRUE(jce_fs_host_write_all(TOUCH_SCRIPT_FILE, g_script,
										  sizeof(g_script) - 1u));
}

void tearDown(void)
{
	jce_fs_host_remove_file(TOUCH_SCRIPT_FILE);
}

static JceRuntime *make_runtime(JceScene **out_scene, JceEntity *out_entity)
{
	JceScene *scene = jce_scene_create();
	JceEntity entity;
	JceTransform transform = {0};
	JceScriptComponent script = {0};
	JceRuntimeDesc desc = {0};

	TEST_ASSERT_NOT_NULL(scene);
	entity = jce_scene_create_entity(scene, "touch-probe");
	transform.rotation.w = 1.0f;
	transform.scale.x = transform.scale.y = transform.scale.z = 1.0f;
	jce_scene_set_transform(scene, entity, &transform);
	strncpy(script.script_path, TOUCH_SCRIPT_FILE, sizeof(script.script_path) - 1u);
	jce_scene_set_script(scene, entity, &script);
	jce_scene_set_component_enabled(scene, entity, JCE_COMP_FLAG_SCRIPT, true);
	desc.scene = scene;
	desc.enable_physics = false;
	*out_scene = scene;
	*out_entity = entity;
	return jce_runtime_create(&desc);
}

static void assert_completed_step(JceScene *scene, JceEntity entity, int step,
								  int touch_count)
{
	const JceTransform *transform = jce_scene_get_transform(scene, entity);
	TEST_ASSERT_NOT_NULL(transform);
	TEST_ASSERT_EQUAL_FLOAT((float)step, transform->position.x);
	TEST_ASSERT_EQUAL_FLOAT((float)touch_count, transform->position.y);
}

static void test_runtime_touch_samples_replace_clamp_validate_and_clear(void)
{
	JceScene *scene;
	JceEntity entity;
	JceRuntime *rt = make_runtime(&scene, &entity);
	JceRuntimeTouch seven[7] = {0};
	JceRuntimeTouch two[2] = {0};
	JceRuntimeTouch mixed[3] = {0};
	JceRuntimeTouch one = {401, 0.4f, 0.5f, 0.6f};

	TEST_ASSERT_NOT_NULL(rt);
	for (int i = 0; i < 7; ++i) {
		seven[i].id = (uint64_t)(101 + i);
		seven[i].x = (float)(i + 1) / 10.0f;
		seven[i].y = (float)(i + 1) / 20.0f;
		seven[i].pressure = (float)(i + 1) / 5.0f;
	}
	jce_runtime_set_touch_input(rt, seven, 7);
	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 1, 5);

	two[0] = (JceRuntimeTouch){201, 0.1f, 0.2f, 0.3f};
	two[1] = (JceRuntimeTouch){202, 0.4f, 0.5f, 0.6f};
	jce_runtime_set_touch_input(rt, two, 2);
	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 2, 2);

	mixed[0] = (JceRuntimeTouch){301, 0.1f, 0.2f, 0.3f};
	mixed[1] = (JceRuntimeTouch){302, NAN, 0.2f, 0.3f};
	mixed[2] = (JceRuntimeTouch){303, 0.1f, 0.2f, INFINITY};
	jce_runtime_set_touch_input(rt, mixed, 3);
	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 3, 1);

	jce_runtime_set_touch_input(rt, NULL, 0);
	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 4, 0);

	jce_runtime_set_touch_input(rt, &one, 1);
	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 5, 1);

	jce_runtime_step(rt, 1.0f / 60.0f);
	assert_completed_step(scene, entity, 6, 0);

	jce_runtime_destroy(rt);
	jce_scene_destroy(scene);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_runtime_touch_samples_replace_clamp_validate_and_clear);
	return UNITY_END();
}
