#include <jce/api.h>
#include <jce/os/core/jce_path.h>

#include "unity.h"

#include <string.h>

typedef struct {
	int count;
	JceEntity leader;
	JceEntity return_stroke;
	JceEntity core;
	JceEntity halo;
	JceEntity upward_streamer;
	JceEntity impact_light;
	JceEntity director;
} LightningEntities;

void setUp(void) {}
void tearDown(void) {}

static void collect_lightning_entities(JceScene *scene, JceEntity entity,
									   void *user_data)
{
	LightningEntities *found = (LightningEntities *)user_data;
	const char *name = jce_scene_entity_name(scene, entity);

	found->count++;
	if (!name) return;
	if (strcmp(name, "LightningLeader") == 0)
		found->leader = entity;
	else if (strcmp(name, "LightningReturnStroke") == 0)
		found->return_stroke = entity;
	else if (strcmp(name, "LightningCore") == 0)
		found->core = entity;
	else if (strcmp(name, "LightningHalo") == 0)
		found->halo = entity;
	else if (strcmp(name, "LightningUpwardStreamer") == 0)
		found->upward_streamer = entity;
	else if (strcmp(name, "LightningImpactLight") == 0)
		found->impact_light = entity;
	else if (strcmp(name, "LightningDirector") == 0)
		found->director = entity;
}

static bool resolve_lightning_asset(void *user_data, const char *in_path,
									char *out_path, int out_size)
{
	const char *root = (const char *)user_data;

	if (!root || !in_path || !out_path || out_size <= 0) return false;
	return jce_path_join(out_path, (size_t)out_size, root, in_path);
}

static float absf(float value)
{
	return value < 0.0f ? -value : value;
}

static void test_authored_scene_runs_through_real_runtime(void)
{
	JceScene *scene = jce_scene_create();
	LightningEntities found = {0};
	int max_leader_points = 0;
	int max_return_points = 0;
	int max_core_points = 0;
	int max_halo_points = 0;
	int max_upward_points = 0;
	float max_impact_intensity = 0.0f;
	float core_min_z = 1000000.0f;
	float core_max_z = -1000000.0f;
	bool core_hits_conductor = false;
	bool core_leaves_ground_upward = false;

	TEST_ASSERT_NOT_NULL(scene);
	TEST_ASSERT_TRUE(jce_scene_serial_load_file(scene,
											 CK_LIGHTNING_SCENE_PATH));
	jce_scene_each_entity(scene, collect_lightning_entities, &found);
	TEST_ASSERT_GREATER_OR_EQUAL_INT(20, found.count);
	TEST_ASSERT_NOT_EQUAL(0, found.leader);
	TEST_ASSERT_NOT_EQUAL(0, found.return_stroke);
	TEST_ASSERT_NOT_EQUAL(0, found.core);
	TEST_ASSERT_NOT_EQUAL(0, found.halo);
	TEST_ASSERT_NOT_EQUAL(0, found.upward_streamer);
	TEST_ASSERT_NOT_EQUAL(0, found.impact_light);
	TEST_ASSERT_NOT_EQUAL(0, found.director);

	JceRuntimeDesc desc = {0};
	desc.scene = scene;
	desc.enable_physics = false;
	desc.resolve_path_fn = resolve_lightning_asset;
	desc.user_data = (void *)CK_LIGHTNING_ASSET_ROOT;

	JceRuntime *runtime = jce_runtime_create(&desc);
	TEST_ASSERT_NOT_NULL(runtime);

	for (int frame = 0; frame < 300; ++frame) {
		JceLineRendererComponent *leader;
		JceLineRendererComponent *return_stroke;
		JceLineRendererComponent *core;
		JceLineRendererComponent *halo;
		JceLineRendererComponent *upward;
		JcePointLight *impact;

		jce_runtime_step(runtime, 1.0f / 60.0f);
		leader = jce_scene_get_line_renderer(scene, found.leader);
		return_stroke = jce_scene_get_line_renderer(scene,
											 found.return_stroke);
		core = jce_scene_get_line_renderer(scene, found.core);
		halo = jce_scene_get_line_renderer(scene, found.halo);
		upward = jce_scene_get_line_renderer(scene, found.upward_streamer);
		impact = jce_scene_get_point_light(scene, found.impact_light);
		if (leader && leader->position_count > max_leader_points)
			max_leader_points = leader->position_count;
		if (return_stroke && return_stroke->position_count > max_return_points)
			max_return_points = return_stroke->position_count;
		if (core && core->position_count > max_core_points) {
			max_core_points = core->position_count;
			core_min_z = 1000000.0f;
			core_max_z = -1000000.0f;
			for (int i = 0; i < core->position_count; ++i) {
				if (core->positions[i][2] < core_min_z)
					core_min_z = core->positions[i][2];
				if (core->positions[i][2] > core_max_z)
					core_max_z = core->positions[i][2];
			}
			const float *tip = core->positions[0];
			const int rise_index = core->position_count > 4 ? 4 :
				core->position_count - 1;
			core_hits_conductor =
				(absf(tip[0]) < 0.15f && absf(tip[1] - 8.4f) < 0.15f &&
				 absf(tip[2]) < 0.15f) ||
				(absf(tip[0] + 8.0f) < 0.15f &&
				 absf(tip[1] - 5.6f) < 0.15f && absf(tip[2] + 3.0f) < 0.15f) ||
				(absf(tip[0] - 8.0f) < 0.15f &&
				 absf(tip[1] - 6.6f) < 0.15f && absf(tip[2] - 2.0f) < 0.15f);
			core_leaves_ground_upward =
				core->positions[rise_index][1] > tip[1] + 0.2f;
		}
		if (halo && halo->position_count > max_halo_points)
			max_halo_points = halo->position_count;
		if (upward && upward->position_count > max_upward_points)
			max_upward_points = upward->position_count;
		if (impact && impact->intensity > max_impact_intensity)
			max_impact_intensity = impact->intensity;
	}

	TEST_ASSERT_GREATER_THAN(3, max_leader_points);
	TEST_ASSERT_GREATER_THAN(3, max_return_points);
	TEST_ASSERT_GREATER_THAN(3, max_core_points);
	TEST_ASSERT_GREATER_THAN(3, max_halo_points);
	TEST_ASSERT_GREATER_THAN(2, max_upward_points);
	TEST_ASSERT_GREATER_THAN_FLOAT(0.5f, core_max_z - core_min_z);
	TEST_ASSERT_TRUE_MESSAGE(core_hits_conductor,
							 "return stroke must originate at an authored conductor tip");
	TEST_ASSERT_TRUE_MESSAGE(core_leaves_ground_upward,
							 "channel must not crawl horizontally along the ground boundary");
	TEST_ASSERT_GREATER_THAN_FLOAT(1.0f, max_impact_intensity);

	jce_runtime_destroy(runtime);
	jce_scene_destroy(scene);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_authored_scene_runs_through_real_runtime);
	return UNITY_END();
}
