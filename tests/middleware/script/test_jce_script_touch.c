#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <string.h>

typedef struct {
	int count;
	struct {
		uint64_t id;
		float x;
		float y;
		float pressure;
	} samples[2];
} TouchRecorder;

static TouchRecorder g_rec;

static int mock_touch_count(void *user)
{
	return ((TouchRecorder *)user)->count;
}

static bool mock_touch_get(void *user, int index, uint64_t *id,
					   float *x, float *y, float *pressure)
{
	TouchRecorder *rec = (TouchRecorder *)user;
	if (index < 0 || index >= rec->count)
		return false;
	*id = rec->samples[index].id;
	*x = rec->samples[index].x;
	*y = rec->samples[index].y;
	*pressure = rec->samples[index].pressure;
	return true;
}

void setUp(void)
{
	memset(&g_rec, 0, sizeof(g_rec));
	g_rec.count = 2;
	g_rec.samples[0].id = 41;
	g_rec.samples[0].x = 0.25f;
	g_rec.samples[0].y = 0.75f;
	g_rec.samples[0].pressure = 0.5f;
	g_rec.samples[1].id = 99;
	g_rec.samples[1].x = 0.8f;
	g_rec.samples[1].y = 0.2f;
	g_rec.samples[1].pressure = 1.0f;
}

void tearDown(void) {}

static void test_touch_marshalling(void)
{
	JceScriptHost host = {0};
	JceScript *vm;
	JceScriptInstance inst;

	host.user = &g_rec;
	host.touch_count = mock_touch_count;
	host.touch_get = mock_touch_get;
	vm = jce_script_create(&host);
	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate_source(vm, "@touch",
		"assert(jce.get_touch_count() == 2)\n"
		"local id,x,y,p = jce.get_touch(1)\n"
		"assert(id == 41 and x == 0.25 and y == 0.75 and p == 0.5)\n"
		"local id2,x2,y2,p2 = jce.get_touch(2)\n"
		"assert(id2 == 99)\n"
		"assert(math.abs(x2-0.8) < 0.000001 and math.abs(y2-0.2) < 0.000001)\n"
		"assert(math.abs(p2-1.0) < 0.000001)\n"
		"assert(jce.get_touch(0) == nil)\n"
		"assert(jce.get_touch(3) == nil)\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	jce_script_destroy(vm);
}

static void test_touch_null_host_defaults(void)
{
	JceScript *vm = jce_script_create(NULL);
	JceScriptInstance inst;

	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate_source(vm, "@touch_null",
		"assert(jce.get_touch_count() == 0)\n"
		"assert(jce.get_touch(1) == nil)\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	jce_script_destroy(vm);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_touch_marshalling);
	RUN_TEST(test_touch_null_host_defaults);
	return UNITY_END();
}
