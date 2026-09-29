#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_filesystem.h>

#include "unity.h"

#include <string.h>

enum {
	ENTITY_LEADER = 11,
	ENTITY_RETURN_STROKE = 12,
	ENTITY_CORE = 13,
	ENTITY_HALO = 14,
	ENTITY_UPWARD_STREAMER = 15,
	ENTITY_BRANCH_BASE = 20,
	ENTITY_IMPACT_LIGHT = 31,
	ENTITY_CLOUD_LIGHT = 32,
	ENTITY_IMPACT_GLOW = 33,
	ENTITY_CAMERA = 34,
	ENTITY_CLOUD_SCATTER_A = 35,
	ENTITY_CLOUD_SCATTER_B = 36
};

typedef struct {
	int leader_writes;
	int return_writes;
	int core_writes;
	int halo_writes;
	int upward_streamer_writes;
	int branch_writes;
	bool branch_seen[8];
	int impact_light_writes;
	int cloud_light_writes;
	int cloud_scatter_writes;
	int glow_writes;
	int render_writes;
	int shake_calls;
	int sound_calls;
	int crack_sound_calls;
	int rumble_sound_calls;
	int spatial_sound_calls;
	float sound_pos[3];
	float sound_min_distance;
	float sound_max_distance;
	float sound_rolloff;
	char sound_path[256];
	char log[1024];
} LightningRecorder;

static LightningRecorder g_rec;

static void *mock_read_file(void *user, const char *path, uint64_t *out_size)
{
	(void)user;
	if (strcmp(path, "scripts/lightning_lab.lua") != 0) {
		*out_size = 0;
		return NULL;
	}
	return jce_fs_host_read_all(CK_LIGHTNING_SCRIPT_PATH, out_size);
}

static void mock_log(void *user, const char *msg)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	size_t used = strlen(rec->log);
	if (used + 2u >= sizeof(rec->log)) return;
	strncat(rec->log, msg, sizeof(rec->log) - used - 2u);
	strncat(rec->log, "\n", sizeof(rec->log) - strlen(rec->log) - 1u);
}

static int mock_find_by_name(void *user, const char *name,
							 JceScriptEntity *out, int max)
{
	(void)user;
	if (!out || max <= 0) return 0;
	if (strcmp(name, "LightningLeader") == 0) *out = ENTITY_LEADER;
	else if (strcmp(name, "LightningReturnStroke") == 0) *out = ENTITY_RETURN_STROKE;
	else if (strcmp(name, "LightningCore") == 0) *out = ENTITY_CORE;
	else if (strcmp(name, "LightningHalo") == 0) *out = ENTITY_HALO;
	else if (strcmp(name, "LightningUpwardStreamer") == 0)
		*out = ENTITY_UPWARD_STREAMER;
	else if (strncmp(name, "LightningBranch_", 16) == 0)
		*out = ENTITY_BRANCH_BASE + (JceScriptEntity)(name[16] - '0');
	else if (strcmp(name, "LightningImpactLight") == 0) *out = ENTITY_IMPACT_LIGHT;
	else if (strcmp(name, "LightningCloudLight") == 0) *out = ENTITY_CLOUD_LIGHT;
	else if (strcmp(name, "LightningImpactGlow") == 0) *out = ENTITY_IMPACT_GLOW;
	else if (strcmp(name, "StormCamera") == 0) *out = ENTITY_CAMERA;
	else if (strcmp(name, "LightningCloudScatter_A") == 0)
		*out = ENTITY_CLOUD_SCATTER_A;
	else if (strcmp(name, "LightningCloudScatter_B") == 0)
		*out = ENTITY_CLOUD_SCATTER_B;
	else return 0;
	return 1;
}

static bool mock_get_position(void *user, JceScriptEntity e, float out[3])
{
	(void)user;
	if (e != ENTITY_CAMERA) return false;
	out[0] = 0.0f;
	out[1] = 8.0f;
	out[2] = 1030.0f;
	return true;
}

static void mock_set_position(void *user, JceScriptEntity e,
							  float x, float y, float z)
{
	(void)user;
	(void)e;
	(void)x;
	(void)y;
	(void)z;
}

static bool mock_comp_set(void *user, JceScriptEntity e, const char *type,
						  const char *json)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	bool nonempty_line = strstr(json, "\"positionCount\":0") == NULL &&
		strstr(json, "\"positionCount\": 0") == NULL;
	if (strcmp(type, "LineRenderer") == 0 && nonempty_line &&
		strstr(json, "positionCount")) {
		if (e == ENTITY_LEADER) rec->leader_writes++;
		else if (e == ENTITY_RETURN_STROKE) rec->return_writes++;
		else if (e == ENTITY_CORE) rec->core_writes++;
		else if (e == ENTITY_HALO) rec->halo_writes++;
		else if (e == ENTITY_UPWARD_STREAMER)
			rec->upward_streamer_writes++;
		else if (e >= ENTITY_BRANCH_BASE && e < ENTITY_BRANCH_BASE + 8) {
			rec->branch_writes++;
			rec->branch_seen[e - ENTITY_BRANCH_BASE] = true;
		}
	} else if (strcmp(type, "Light") == 0) {
		if (e == ENTITY_IMPACT_LIGHT) rec->impact_light_writes++;
		if (e == ENTITY_CLOUD_LIGHT) rec->cloud_light_writes++;
		if (e == ENTITY_CLOUD_SCATTER_A || e == ENTITY_CLOUD_SCATTER_B)
			rec->cloud_scatter_writes++;
	} else if (e == ENTITY_IMPACT_GLOW && strcmp(type, "MeshRenderer") == 0) {
		rec->glow_writes++;
	}
	return true;
}

static bool mock_render_set(void *user, const char *json)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	if (strstr(json, "exposure")) rec->render_writes++;
	return true;
}

static void mock_shake(void *user, float amount)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	if (amount > 0.0f) rec->shake_calls++;
}

static void mock_play_sound(void *user, const char *path, const float pos[3],
							float volume)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	(void)volume;
	rec->sound_calls++;
	if (strstr(path, "thunder_crack")) rec->crack_sound_calls++;
	if (strstr(path, "thunder_roll")) rec->rumble_sound_calls++;
	strncpy(rec->sound_path, path, sizeof(rec->sound_path) - 1u);
	if (pos) memcpy(rec->sound_pos, pos, sizeof(rec->sound_pos));
}

static void mock_play_sound_spatial(void *user, const char *path,
									const float pos[3], float volume,
									float min_distance, float max_distance,
									float rolloff)
{
	LightningRecorder *rec = (LightningRecorder *)user;
	mock_play_sound(user, path, pos, volume);
	rec->spatial_sound_calls++;
	rec->sound_min_distance = min_distance;
	rec->sound_max_distance = max_distance;
	rec->sound_rolloff = rolloff;
}

static JceScript *create_vm(void)
{
	JceScriptHost host = {0};
	host.user = &g_rec;
	host.log = mock_log;
	host.read_file = mock_read_file;
	host.find_by_name = mock_find_by_name;
	host.get_position = mock_get_position;
	host.set_position = mock_set_position;
	host.comp_set_json = mock_comp_set;
	host.render_set_json = mock_render_set;
	host.shake_camera = mock_shake;
	host.play_sound = mock_play_sound;
	host.play_sound_spatial = mock_play_sound_spatial;
	return jce_script_create(&host);
}

static void step_vm(JceScript *vm, JceScriptInstance inst,
					float seconds, float dt)
{
	int steps = (int)(seconds / dt + 0.5f);
	for (int i = 0; i < steps; ++i) {
		jce_script_call_update(vm, inst, dt);
		jce_script_update_coroutines(vm, dt);
	}
}

void setUp(void)
{
	memset(&g_rec, 0, sizeof(g_rec));
}

void tearDown(void) {}

static void test_staged_discharge_and_distance_delayed_thunder(void)
{
	JceScript *vm = create_vm();
	JceScriptInstance inst;

	TEST_ASSERT_NOT_NULL(vm);
	inst = jce_script_instantiate(vm, "scripts/lightning_lab.lua", 1);
	TEST_ASSERT_NOT_EQUAL_MESSAGE(0, inst, g_rec.log);
	jce_script_call_start(vm, inst);
	TEST_ASSERT_EQUAL_INT(1, jce_script_call_named(vm,
												 "ck_lightning_trigger", 0));

	step_vm(vm, inst, 1.5f, 1.0f / 60.0f);
	TEST_ASSERT_GREATER_THAN(3, g_rec.leader_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.branch_writes);
	int visible_branches = 0;
	for (int i = 0; i < 8; ++i)
		if (g_rec.branch_seen[i]) visible_branches++;
	TEST_ASSERT_GREATER_OR_EQUAL_INT(3, visible_branches);
	TEST_ASSERT_GREATER_THAN(0, g_rec.return_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.core_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.halo_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.upward_streamer_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.impact_light_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.cloud_light_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.cloud_scatter_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.glow_writes);
	TEST_ASSERT_GREATER_THAN(0, g_rec.render_writes);
	TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, g_rec.shake_calls,
										  "negative CG flash should contain multiple strokes");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rec.sound_calls,
								  "thunder must not be simultaneous with a distant flash");

	step_vm(vm, inst, 3.0f, 1.0f / 60.0f);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(2, g_rec.sound_calls);
	TEST_ASSERT_EQUAL_INT(1, g_rec.crack_sound_calls);
	TEST_ASSERT_EQUAL_INT(1, g_rec.rumble_sound_calls);
	TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_rec.spatial_sound_calls,
								  "thunder must author long-range 3D attenuation");
	TEST_ASSERT_GREATER_THAN(1030.0f, g_rec.sound_max_distance);
	TEST_ASSERT_GREATER_THAN(1.0f, g_rec.sound_min_distance);
	TEST_ASSERT_LESS_THAN(1.0f, g_rec.sound_rolloff);
	TEST_ASSERT_FLOAT_WITHIN(20.0f, 0.0f, g_rec.sound_pos[1]);
	TEST_ASSERT_TRUE_MESSAGE(strstr(g_rec.log, "error") == NULL, g_rec.log);

	jce_script_destroy(vm);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_staged_discharge_and_distance_delayed_thunder);
	return UNITY_END();
}
