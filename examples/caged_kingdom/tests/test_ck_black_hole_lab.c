#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include "unity.h"

#include <string.h>

#ifndef CK_BLACK_HOLE_SCRIPT_PATH
#error "CK_BLACK_HOLE_SCRIPT_PATH must name black_hole_lab.lua"
#endif

enum {
	ENTITY_ROOT = 9100,
	ENTITY_CAMERA = 9101,
	ENTITY_ANCHOR = 9110,
	ENTITY_EFFECT = 9120,
	ENTITY_READOUT = 9150,
	ENTITY_TERMINOLOGY = 9151,
	ENTITY_REFERENCE = 9160
};

typedef enum ConfigMode {
	CONFIG_VALID,
	CONFIG_BAD_SCHEMA,
	CONFIG_BAD_HASH,
	CONFIG_BAD_RANGE
} ConfigMode;

typedef struct BlackHoleRecorder {
	ConfigMode config_mode;
	int effect_writes;
	int camera_writes;
	int camera_pose_writes;
	int disable_calls;
	int ui_writes;
	bool last_enabled;
	char pressed_action[64];
	char axis_action[64];
	float axis_value;
	float pointer_delta[2];
	float pointer_wheel;
	bool pointer_down;
	char effect_json[16384];
	char readout[2048];
	char reference[2048];
	char terminology[2048];
	char log[4096];
} BlackHoleRecorder;

static BlackHoleRecorder g_rec;

static const char s_valid_config[] =
	"{\"contract\":{\"name\":\"ck.kerr-lab\",\"major\":1,\"minor\":0},"
	"\"identity\":{\"config_hash\":\"sha256:ck-kerr-test-v1\",\"revision\":8},"
	"\"metric\":{\"mass_solar\":100000000,\"spin_chi\":0.8},"
	"\"observer\":{\"model\":\"zamo\",\"radius_over_m\":40,"
	"\"inclination_deg\":60,\"azimuth_deg\":0,\"fov_deg\":60},"
	"\"disk\":{\"model\":\"page_thorne_thin\",\"outer_radius_over_m\":18,"
	"\"luminosity_eddington_ratio\":0.1,"
	"\"spectrum\":\"bolometric_false_color\",\"limb_darkening\":0.5},"
	"\"integration\":{\"preset\":\"scientific_realtime\",\"max_steps\":640,"
	"\"escape_radius_over_m\":600,\"capture_epsilon\":0.002,"
	"\"residual_limit\":0.0005,\"history_weight\":0.75,"
	"\"step_fraction_r\":0.04,\"step_fraction_theta\":0.035,"
	"\"step_fraction_phi\":0.035,\"edge_threshold\":0.04},"
	"\"spectrum\":{\"band_min_nm\":380,\"band_max_nm\":780,"
	"\"temperature_min_kelvin\":2200,\"temperature_max_kelvin\":18000},"
	"\"celestial_background\":{\"orientation\":[0,0,0,1]},"
	"\"flux_lut\":{\"spin_min\":-0.998,\"spin_step\":0.01584375,"
	"\"radial_count\":512,\"spin_count\":127},"
	"\"presentation\":{\"exposure_ev\":0,\"diagnostic\":\"radiance\","
	"\"compare_split\":-1}}";

static const char s_bad_schema_config[] =
	"{\"contract\":{\"name\":\"ck.kerr-lab\",\"major\":2,\"minor\":0},"
	"\"identity\":{\"config_hash\":\"sha256:ck-kerr-test-v1\",\"revision\":7}}";

static const char s_bad_range_config[] =
	"{\"contract\":{\"name\":\"ck.kerr-lab\",\"major\":1,\"minor\":0},"
	"\"identity\":{\"config_hash\":\"sha256:ck-kerr-test-v1\",\"revision\":7},"
	"\"metric\":{\"mass_solar\":100000000,\"spin_chi\":1.1},"
	"\"observer\":{\"model\":\"zamo\",\"radius_over_m\":100,"
	"\"inclination_deg\":60,\"azimuth_deg\":0,\"fov_deg\":20},"
	"\"disk\":{\"model\":\"page_thorne_thin\",\"outer_radius_over_m\":40,"
	"\"luminosity_eddington_ratio\":0.1,\"spectrum\":\"bolometric_false_color\"},"
	"\"integration\":{\"preset\":\"scientific_realtime\",\"max_steps\":320,"
	"\"escape_radius_over_m\":1000,\"residual_limit\":0.0005},"
	"\"presentation\":{\"diagnostic\":\"radiance\"}}";

static const char s_valid_reference[] =
	"{\"contract\":{\"name\":\"ck.kerr-reference-summary\",\"major\":1,\"minor\":0},"
	"\"config_hash\":\"sha256:ck-kerr-test-v1\","
	"\"source_corpus_hash\":\"sha256:ck-kerr-corpus-v1\","
	"\"validation\":{\"status\":\"passed\",\"max_residual\":1e-9}}";

static const char s_bad_hash_reference[] =
	"{\"contract\":{\"name\":\"ck.kerr-reference-summary\",\"major\":1,\"minor\":0},"
	"\"config_hash\":\"sha256:different\","
	"\"source_corpus_hash\":\"sha256:ck-kerr-corpus-v1\","
	"\"validation\":{\"status\":\"passed\",\"max_residual\":1e-9}}";

static void *copy_payload(const char *text, uint64_t *out_size)
{
	size_t size = strlen(text);
	void *bytes = jce_malloc(size ? size : 1u);

	TEST_ASSERT_NOT_NULL(bytes);
	if (size) memcpy(bytes, text, size);
	*out_size = (uint64_t)size;
	return bytes;
}

static void *mock_read_file(void *user, const char *path, uint64_t *out_size)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;

	if (strcmp(path, "scripts/black_hole_lab.lua") == 0)
		return jce_fs_host_read_all(CK_BLACK_HOLE_SCRIPT_PATH, out_size);
	if (strcmp(path, "black_hole/kerr_lab.json") == 0) {
		if (rec->config_mode == CONFIG_BAD_SCHEMA)
			return copy_payload(s_bad_schema_config, out_size);
		if (rec->config_mode == CONFIG_BAD_RANGE)
			return copy_payload(s_bad_range_config, out_size);
		return copy_payload(s_valid_config, out_size);
	}
	if (strcmp(path, "black_hole/reference_summary.json") == 0) {
		return copy_payload(rec->config_mode == CONFIG_BAD_HASH
			? s_bad_hash_reference : s_valid_reference, out_size);
	}
	*out_size = 0;
	return NULL;
}

static void append_text(char *dst, size_t cap, const char *src)
{
	size_t used = strlen(dst);
	size_t left = used < cap ? cap - used - 1u : 0u;
	size_t n = strlen(src);

	if (!left) return;
	if (n > left) n = left;
	memcpy(dst + used, src, n);
	dst[used + n] = '\0';
}

static void mock_log(void *user, const char *msg)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	append_text(rec->log, sizeof(rec->log), msg);
	append_text(rec->log, sizeof(rec->log), "\n");
}

static int mock_find_by_name(void *user, const char *name,
					 JceScriptEntity *out, int max)
{
	(void)user;
	if (!out || max <= 0) return 0;
	if (strcmp(name, "BlackHoleLabRoot") == 0) *out = ENTITY_ROOT;
	else if (strcmp(name, "KerrObserverCamera") == 0) *out = ENTITY_CAMERA;
	else if (strcmp(name, "KerrCoordinateAnchor") == 0) *out = ENTITY_ANCHOR;
	else if (strcmp(name, "KerrFullscreenEffect") == 0) *out = ENTITY_EFFECT;
	else if (strcmp(name, "CriticalCurveOverlay") == 0) *out = ENTITY_READOUT;
	else if (strcmp(name, "KerrTerminology") == 0) *out = ENTITY_TERMINOLOGY;
	else if (strcmp(name, "ReferenceRayProbe") == 0) *out = ENTITY_REFERENCE;
	else return 0;
	return 1;
}

static bool mock_get_scale(void *user, JceScriptEntity entity, float out[3])
{
	(void)user;
	(void)entity;
	out[0] = 1.0f;
	out[1] = 1.0f;
	out[2] = 1.0f;
	return true;
}

static JceScriptEntity mock_get_parent(void *user, JceScriptEntity entity)
{
	(void)user;
	if (entity == ENTITY_EFFECT) return ENTITY_ANCHOR;
	if (entity == ENTITY_ANCHOR || entity == ENTITY_CAMERA) return ENTITY_ROOT;
	return 0;
}

static void mock_set_position(void *user, JceScriptEntity entity,
						  float x, float y, float z)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	(void)x;
	(void)y;
	(void)z;
	if (entity == ENTITY_CAMERA) rec->camera_pose_writes++;
}

static void mock_set_rotation(void *user, JceScriptEntity entity,
						  float x, float y, float z)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	(void)x;
	(void)y;
	(void)z;
	if (entity == ENTITY_CAMERA) rec->camera_pose_writes++;
}

static bool mock_comp_set(void *user, JceScriptEntity entity,
					  const char *type, const char *json)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;

	if (entity == ENTITY_EFFECT && strcmp(type, "FullscreenEffect") == 0) {
		rec->effect_writes++;
		strncpy(rec->effect_json, json, sizeof(rec->effect_json) - 1u);
	} else if (entity == ENTITY_CAMERA && strcmp(type, "Camera") == 0) {
		rec->camera_writes++;
	}
	return true;
}

static void mock_set_component_enabled(void *user, JceScriptEntity entity,
							   const char *type, bool on)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	if (entity == ENTITY_EFFECT && strcmp(type, "FullscreenEffect") == 0) {
		rec->disable_calls++;
		rec->last_enabled = on;
	}
}

static bool mock_action_pressed(void *user, const char *name)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	return rec->pressed_action[0] && strcmp(rec->pressed_action, name) == 0;
}

static bool mock_action_down(void *user, const char *name)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	return strcmp(name, "black_hole.orbit") == 0 && rec->pointer_down;
}

static float mock_action_axis(void *user, const char *name)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	return rec->axis_action[0] && strcmp(rec->axis_action, name) == 0
		? rec->axis_value : 0.0f;
}

static void mock_pointer_delta(void *user, float out[2])
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	out[0] = rec->pointer_delta[0];
	out[1] = rec->pointer_delta[1];
}

static float mock_pointer_wheel(void *user)
{
	return ((BlackHoleRecorder *)user)->pointer_wheel;
}

static bool mock_pointer_button(void *user, int button)
{
	return button == 1 && ((BlackHoleRecorder *)user)->pointer_down;
}

static void mock_ui_set_text(void *user, JceScriptEntity entity,
						 const char *text)
{
	BlackHoleRecorder *rec = (BlackHoleRecorder *)user;
	char *dst = NULL;
	size_t cap = 0;

	rec->ui_writes++;
	if (entity == ENTITY_READOUT) {
		dst = rec->readout;
		cap = sizeof(rec->readout);
	} else if (entity == ENTITY_REFERENCE) {
		dst = rec->reference;
		cap = sizeof(rec->reference);
	} else if (entity == ENTITY_TERMINOLOGY) {
		dst = rec->terminology;
		cap = sizeof(rec->terminology);
	}
	if (dst) {
		strncpy(dst, text, cap - 1u);
		dst[cap - 1u] = '\0';
	}
}

static JceScript *create_vm(void)
{
	JceScriptHost host = {0};
	host.user = &g_rec;
	host.log = mock_log;
	host.read_file = mock_read_file;
	host.find_by_name = mock_find_by_name;
	host.get_scale = mock_get_scale;
	host.set_position = mock_set_position;
	host.set_rotation = mock_set_rotation;
	host.comp_set_json = mock_comp_set;
	host.set_component_enabled = mock_set_component_enabled;
	host.action_pressed = mock_action_pressed;
	host.action_down = mock_action_down;
	host.action_axis = mock_action_axis;
	host.pointer_delta = mock_pointer_delta;
	host.pointer_wheel = mock_pointer_wheel;
	host.pointer_button = mock_pointer_button;
	host.ui_set_text = mock_ui_set_text;
	host.get_parent = mock_get_parent;
	return jce_script_create(&host);
}

static JceScriptInstance start_lab(JceScript *vm)
{
	JceScriptInstance instance = jce_script_instantiate(
		vm, "scripts/black_hole_lab.lua", ENTITY_ROOT);
	TEST_ASSERT_NOT_EQUAL_MESSAGE(0, instance, g_rec.log);
	jce_script_call_start(vm, instance);
	return instance;
}

static JceJson *parse_effect(void)
{
	JceJson *root = jce_json_parse_strict(g_rec.effect_json, 0);
	TEST_ASSERT_NOT_NULL_MESSAGE(root, g_rec.effect_json);
	return root;
}

static JceJson *param_row(JceJson *effect, int row)
{
	JceJson *params = jce_json_get(effect, "params");
	TEST_ASSERT_TRUE(jce_json_is_array(params));
	TEST_ASSERT_EQUAL_INT(16, jce_json_array_size(params));
	return jce_json_array_at(params, row);
}

static double row_value(JceJson *effect, int row, int col)
{
	JceJson *value = jce_json_array_at(param_row(effect, row), col);
	TEST_ASSERT_TRUE(jce_json_is_number(value));
	return jce_json_number_value(value, -9999.0);
}

void setUp(void)
{
	memset(&g_rec, 0, sizeof(g_rec));
	g_rec.config_mode = CONFIG_VALID;
}

void tearDown(void) {}

static void test_config_packs_deterministic_kerr_abi(void)
{
	JceScript *vm = create_vm();
	JceJson *effect;

	TEST_ASSERT_NOT_NULL(vm);
	(void)start_lab(vm);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rec.effect_writes, g_rec.log);
	effect = parse_effect();
	TEST_ASSERT_TRUE(jce_json_get_bool(effect, "enabled", false));
	TEST_ASSERT_TRUE(jce_json_get_bool(effect, "required", false));
	TEST_ASSERT_FALSE(jce_json_get_bool(effect, "useHistory", true));
	TEST_ASSERT_EQUAL_STRING("ck_kerr_lensing_640",
		jce_json_get_string(effect, "shader", ""));
	TEST_ASSERT_EQUAL_INT(1, jce_json_get_int(effect, "outputFormat", -1));
	TEST_ASSERT_EQUAL_INT(4,
		jce_json_array_size(jce_json_get(effect, "textures")));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f, (float)row_value(effect, 0, 0));
	TEST_ASSERT_FLOAT_WITHIN(1.0f, 100000000.0f,
		(float)row_value(effect, 0, 1));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 18.0f, (float)row_value(effect, 1, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 640.0f, (float)row_value(effect, 2, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.75f, (float)row_value(effect, 2, 3));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2200.0f, (float)row_value(effect, 3, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 18000.0f, (float)row_value(effect, 3, 1));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.6f, (float)row_value(effect, 7, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.9066439f,
		(float)row_value(effect, 7, 1));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.0f, (float)row_value(effect, 7, 2));
	for (int row = 0; row < 8; ++row)
		for (int col = 0; col < 4; ++col)
			TEST_ASSERT_FLOAT_WITHIN(1e-6f,
				(float)row_value(effect, row, col),
				(float)row_value(effect, row + 8, col));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.reference, "REFERENCE_DATA"));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.reference, "GPU validation"));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.terminology, "Event horizon"));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.terminology, "Critical curve"));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.terminology, "Shadow"));
	jce_json_free(effect);
	jce_script_destroy(vm);
}

static void test_presets_diagnostics_and_history_invalidation(void)
{
	JceScript *vm = create_vm();
	JceScriptInstance instance;
	JceJson *effect;

	TEST_ASSERT_NOT_NULL(vm);
	instance = start_lab(vm);
	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	effect = parse_effect();
	TEST_ASSERT_TRUE(jce_json_get_bool(effect, "useHistory", false));
	jce_json_free(effect);

	strncpy(g_rec.pressed_action, "black_hole.preset_1",
		sizeof(g_rec.pressed_action) - 1u);
	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	g_rec.pressed_action[0] = '\0';
	effect = parse_effect();
	TEST_ASSERT_FALSE(jce_json_get_bool(effect, "useHistory", true));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, (float)row_value(effect, 0, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, (float)row_value(effect, 7, 0));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, (float)row_value(effect, 7, 1));
	jce_json_free(effect);

	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	effect = parse_effect();
	TEST_ASSERT_TRUE(jce_json_get_bool(effect, "useHistory", false));
	jce_json_free(effect);

	strncpy(g_rec.pressed_action, "black_hole.diagnostic",
		sizeof(g_rec.pressed_action) - 1u);
	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	effect = parse_effect();
	TEST_ASSERT_FALSE(jce_json_get_bool(effect, "useHistory", true));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, (float)row_value(effect, 2, 1));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.readout, "Ray outcome"));
	jce_json_free(effect);
	jce_script_destroy(vm);
}

static void test_interaction_clamps_and_selects_quality_tier(void)
{
	JceScript *vm = create_vm();
	JceScriptInstance instance;
	JceJson *effect;

	TEST_ASSERT_NOT_NULL(vm);
	instance = start_lab(vm);
	strncpy(g_rec.axis_action, "black_hole.spin",
		sizeof(g_rec.axis_action) - 1u);
	g_rec.axis_value = 100.0f;
	jce_script_call_update(vm, instance, 1.0f);
	effect = parse_effect();
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.998f, (float)row_value(effect, 0, 0));
	TEST_ASSERT_FALSE(jce_json_get_bool(effect, "useHistory", true));
	jce_json_free(effect);

	g_rec.axis_action[0] = '\0';
	strncpy(g_rec.pressed_action, "black_hole.quality_up",
		sizeof(g_rec.pressed_action) - 1u);
	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	effect = parse_effect();
	TEST_ASSERT_EQUAL_STRING("ck_kerr_lensing_768",
		jce_json_get_string(effect, "shader", ""));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 768.0f, (float)row_value(effect, 2, 0));
	jce_json_free(effect);

	g_rec.pressed_action[0] = '\0';
	g_rec.pointer_down = true;
	g_rec.pointer_delta[0] = 20000.0f;
	g_rec.pointer_delta[1] = 20000.0f;
	g_rec.pointer_wheel = 1000.0f;
	jce_script_call_update(vm, instance, 1.0f / 60.0f);
	TEST_ASSERT_GREATER_THAN(2, g_rec.camera_pose_writes);
	TEST_ASSERT_NOT_NULL(strstr(g_rec.readout, "inclination=89.000"));
	TEST_ASSERT_NOT_NULL(strstr(g_rec.readout, "observer=12.000M"));
	jce_script_destroy(vm);
}

static void test_canvas_handlers_share_controller_state(void)
{
	JceScript *vm = create_vm();
	JceScriptInstance instance;
	JceJson *effect;

	TEST_ASSERT_NOT_NULL(vm);
	instance = start_lab(vm);
	TEST_ASSERT_TRUE(jce_script_call_named_num(vm,
		"ck_black_hole_set_spin", 9181, -0.8));
	TEST_ASSERT_TRUE(jce_script_call_named_num(vm,
		"ck_black_hole_set_quality", 9185, 1.0));
	TEST_ASSERT_TRUE(jce_script_call_named(vm,
		"ck_black_hole_toggle_overlay", 9180));
	jce_script_call_update(vm, instance, 1.0f / 60.0f);

	effect = parse_effect();
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.8f,
		(float)row_value(effect, 0, 0));
	TEST_ASSERT_EQUAL_STRING("ck_kerr_lensing_128",
		jce_json_get_string(effect, "shader", ""));
	TEST_ASSERT_FALSE(jce_json_get_bool(effect, "useHistory", true));
	TEST_ASSERT_EQUAL_STRING("", g_rec.terminology);
	jce_json_free(effect);
	jce_script_destroy(vm);
}

static void assert_invalid_config_refused(ConfigMode mode,
							 const char *expected_code)
{
	JceScript *vm;

	g_rec.config_mode = mode;
	vm = create_vm();
	TEST_ASSERT_NOT_NULL(vm);
	(void)start_lab(vm);
	TEST_ASSERT_EQUAL_INT(0, g_rec.effect_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.disable_calls);
	TEST_ASSERT_FALSE(g_rec.last_enabled);
	TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_rec.log, expected_code), g_rec.log);
	jce_script_destroy(vm);
}

static void test_schema_hash_and_range_mismatch_refuse_activation(void)
{
	assert_invalid_config_refused(CONFIG_BAD_SCHEMA, "CONFIG_SCHEMA_MISMATCH");
	setUp();
	assert_invalid_config_refused(CONFIG_BAD_HASH, "REFERENCE_HASH_MISMATCH");
	setUp();
	assert_invalid_config_refused(CONFIG_BAD_RANGE, "CONFIG_RANGE_INVALID");
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_config_packs_deterministic_kerr_abi);
	RUN_TEST(test_presets_diagnostics_and_history_invalidation);
	RUN_TEST(test_interaction_clamps_and_selects_quality_tier);
	RUN_TEST(test_canvas_handlers_share_controller_state);
	RUN_TEST(test_schema_hash_and_range_mismatch_refuse_activation);
	return UNITY_END();
}
