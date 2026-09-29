#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

typedef struct AssetJsonRecorder {
	int calls;
	uint64_t last_input_cap;
} AssetJsonRecorder;

static AssetJsonRecorder g_rec;

static void *copy_payload(const char *text, uint64_t *out_size)
{
	size_t size = strlen(text);
	void *bytes = jce_malloc(size ? size : 1u);

	TEST_ASSERT_NOT_NULL(bytes);
	if (size)
		memcpy(bytes, text, size);
	*out_size = (uint64_t)size;
	return bytes;
}

static void *make_node_limit_payload(uint64_t *out_size)
{
	const int count = 16385;
	size_t cap = (size_t)count * 2u + 3u;
	char *text = (char *)jce_malloc(cap);
	size_t at = 0;

	TEST_ASSERT_NOT_NULL(text);
	text[at++] = '[';
	for (int i = 0; i < count; ++i) {
		if (i)
			text[at++] = ',';
		text[at++] = '0';
	}
	text[at++] = ']';
	text[at] = '\0';
	*out_size = (uint64_t)at;
	return text;
}

static void *mock_read_file(void *user, const char *path, uint64_t *out_size)
{
	AssetJsonRecorder *rec = (AssetJsonRecorder *)user;

	rec->calls++;
	rec->last_input_cap = *out_size;
	if (strcmp(path, "data/valid.json") == 0) {
		return copy_payload(
			"{\"schema\":1,\"name\":\"Kerr\",\"enabled\":true,"
			"\"values\":[3.0,null,false],\"missing\":null,"
			"\"nested\":{\"spin\":0.8}}",
			out_size);
	}
	if (strcmp(path, "data/duplicate.json") == 0)
		return copy_payload("{\"spin\":0.8,\"spin\":0.9}", out_size);
	if (strcmp(path, "data/trailing.json") == 0)
		return copy_payload("{\"schema\":1} trailing", out_size);
	if (strcmp(path, "data/deep.json") == 0) {
		return copy_payload(
			"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]",
			out_size);
	}
	if (strcmp(path, "data/many.json") == 0)
		return make_node_limit_payload(out_size);
	if (strcmp(path, "data/scalar.json") == 0)
		return copy_payload("42", out_size);
	if (strcmp(path, "data/oversize.json") == 0) {
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

static void test_asset_read_json_converts_structured_values_and_null(void)
{
	JceScriptHost host = {0};
	JceScript *vm;
	JceScriptInstance inst;

	host.user = &g_rec;
	host.read_file = mock_read_file;
	vm = jce_script_create(&host);
	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate_source(vm, "@asset_json_valid",
		"local d, err = jce.asset_read_json('data/valid.json')\n"
		"assert(err == nil and type(d) == 'table')\n"
		"assert(d.schema == 1 and d.name == 'Kerr' and d.enabled == true)\n"
		"assert(#d.values == 3 and d.values[1] == 3.0)\n"
		"assert(d.values[2] == jce.json_null and d.values[3] == false)\n"
		"assert(d.missing == jce.json_null and d.nested.spin == 0.8)\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	TEST_ASSERT_EQUAL_UINT64(JCE_SCRIPT_TEXT_ASSET_MAX_BYTES,
	                         g_rec.last_input_cap);
	jce_script_destroy(vm);
}

static void test_asset_read_json_returns_stable_failure_codes(void)
{
	JceScriptHost host = {0};
	JceScript *vm;
	JceScriptInstance inst;

	host.user = &g_rec;
	host.read_file = mock_read_file;
	vm = jce_script_create(&host);
	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate_source(vm, "@asset_json_errors",
		"local function expect(path, code)\n"
		"  local value, err = jce.asset_read_json(path)\n"
		"  assert(value == nil and err == code, path .. ':' .. tostring(err))\n"
		"end\n"
		"expect('../secret.json', 'invalid_path')\n"
		"expect('data/missing.json', 'not_found')\n"
		"expect('data/duplicate.json', 'duplicate_key')\n"
		"expect('data/trailing.json', 'invalid_json')\n"
		"expect('data/deep.json', 'depth_limit')\n"
		"expect('data/many.json', 'node_limit')\n"
		"expect('data/scalar.json', 'invalid_root')\n"
		"expect('data/oversize.json', 'too_large')\n"
		"return {}\n", 1);
	TEST_ASSERT_NOT_EQUAL(0, inst);
	jce_script_destroy(vm);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_asset_read_json_converts_structured_values_and_null);
	RUN_TEST(test_asset_read_json_returns_stable_failure_codes);
	return UNITY_END();
}
