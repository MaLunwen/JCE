#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include <string.h>

typedef struct {
	int calls;
} AssetRecorder;

static AssetRecorder g_rec;

static void *mock_read_file(void *user, const char *path, uint64_t *out_size)
{
	AssetRecorder *rec = (AssetRecorder *)user;
	const char payload[] = "{\"schema\":1}";
	void *bytes;

	rec->calls++;
	if (strcmp(path, "mission/tour.json") == 0) {
		bytes = jce_malloc(sizeof(payload) - 1u);
		TEST_ASSERT_NOT_NULL(bytes);
		memcpy(bytes, payload, sizeof(payload) - 1u);
		*out_size = sizeof(payload) - 1u;
		return bytes;
	}
	if (strcmp(path, "oversize.txt") == 0) {
		*out_size = JCE_SCRIPT_TEXT_ASSET_MAX_BYTES + 1u;
		return jce_malloc((size_t)*out_size);
	}
	*out_size = 0;
	return NULL;
}

void setUp(void)
{
	memset(&g_rec, 0, sizeof(g_rec));
}

void tearDown(void) {}

static void test_asset_read_text_contract(void)
{
	JceScriptHost host = {0};
	JceScript *vm;
	JceScriptInstance inst;

	host.user = &g_rec;
	host.read_file = mock_read_file;
	vm = jce_script_create(&host);
	TEST_ASSERT_NOT_NULL(vm);

	inst = jce_script_instantiate_source(vm, "@asset_text",
		"assert(jce.asset_read_text('mission/tour.json') == '{\"schema\":1}')\n"
		"assert(jce.asset_read_text('missing.json') == nil)\n"
		"assert(jce.asset_read_text('../secret.txt') == nil)\n"
		"assert(jce.asset_read_text('C:/secret.txt') == nil)\n"
		"assert(jce.asset_read_text('/root.txt') == nil)\n"
		"assert(jce.asset_read_text('bad\\\\path.txt') == nil)\n"
		"assert(jce.asset_read_text('a//b.txt') == nil)\n"
		"assert(jce.asset_read_text('a/./b.txt') == nil)\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	TEST_ASSERT_EQUAL_INT(2, g_rec.calls);
	jce_script_destroy(vm);
}

static void test_asset_read_text_rejects_oversize_payload(void)
{
	JceScriptHost host = {0};
	JceScript *vm;
	JceScriptInstance inst;

	host.user = &g_rec;
	host.read_file = mock_read_file;
	vm = jce_script_create(&host);
	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate_source(vm, "@asset_oversize",
		"assert(jce.asset_read_text('oversize.txt') == nil)\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	TEST_ASSERT_EQUAL_INT(1, g_rec.calls);
	jce_script_destroy(vm);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_asset_read_text_contract);
	RUN_TEST(test_asset_read_text_rejects_oversize_payload);
	return UNITY_END();
}
