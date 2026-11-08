#include "unity.h"

#include <jce/middleware/scene/jce_scene_water_nearshore.h>
#include <jce/middleware/scene/jce_scene_water_spectrum.h>
#include <jce/middleware/scene/jce_terrain.h>
#include "jce_water_field_internal.h"

#include <math.h>
#include <string.h>

static const char terrain_path[] = "terrain/nearshore_test.terrain.json";
static JceScene *scene;
static JceEntity water, land;

void setUp(void)
{
    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    water = jce_scene_create_entity(scene, "Water");
    land = jce_scene_create_entity(scene, "Terrain");
    JceTransform transform = {0};
    transform.scale = jce_v3(1.0f, 1.0f, 1.0f);
    transform.rotation.w = 1.0f;
    jce_scene_set_transform(scene, water, &transform);
    transform.position = jce_v3(-8.0f, -2.0f, -8.0f);
    jce_scene_set_transform(scene, land, &transform);

    JceTerrainComponent terrain = {0};
    memcpy(terrain.terrain_path, terrain_path, sizeof terrain_path);
    terrain.visible = true;
    jce_scene_set_terrain(scene, land, &terrain);
    JceTerrain *heightfield = jce_terrain_create(17, 17, 16.0f, 16.0f, 4.0f, 8);
    TEST_ASSERT_NOT_NULL(heightfield);
    uint16_t heights[17 * 17];
    for (int z = 0; z < 17; ++z)
        for (int x = 0; x < 17; ++x)
            heights[z * 17 + x] = (uint16_t)((x * 65535) / 16);
    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_r16(heightfield, heights, 17, 17));
    TEST_ASSERT_TRUE(jce_scene_adopt_terrain(scene, terrain_path, heightfield));

    JceWaterComponent body = {0};
    body.visible = true;
    body.water_mode = JCE_WATER_MODE_FFT;
    body.size_x = body.size_z = 32.0f;
    body.fft_resolution = 32;
    body.fft_patch_size = 32.0f;
    body.fft_wind_speed = 8.0f;
    body.fft_wind_dir_x = 1.0f;
    body.fft_amplitude = 0.001f;
    jce_scene_set_water(scene, water, &body);
    JceWaterSpectrumComponent spectrum = jce_scene_water_spectrum_default();
    jce_scene_set_water_spectrum(scene, water, &spectrum);

    JceWaterNearshoreComponent config = jce_scene_water_nearshore_default();
    config.terrain = land;
    config.extent = 8.0f;
    jce_scene_set_water_nearshore(scene, water, &config);
}

void tearDown(void)
{
    jce_scene_destroy(scene);
}

static JceWaterShoreSnapshot snapshot(void)
{
    JceWaterShoreSnapshot result = {0};
    const JceWaterShoreState *state = jce_water_field_set_shore(jce_scene_water_fields(scene), water);
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_TRUE(jce_water_shore_state_snapshot(state, &result));
    return result;
}

static void test_translated_bed_and_dry_coverage(void)
{
    TEST_ASSERT_NOT_NULL(jce_scene_acquire_water_field(scene, water));
    const JceWaterShoreSnapshot data = snapshot();
    TEST_ASSERT_EQUAL_INT(32, data.side);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.96875f, data.bed[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.96875f, data.bed[31]);

    float y = -99.0f, coverage = -99.0f;
    TEST_ASSERT_TRUE(jce_scene_water_nearshore_surface(scene, water, -2.0f, 0.0f, 1.0f, &y, &coverage));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, y);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, coverage);
    TEST_ASSERT_TRUE(jce_scene_water_nearshore_surface(scene, water, 2.0f, 0.0f, 1.0f, &y, &coverage));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, coverage);
    TEST_ASSERT_FALSE(jce_scene_water_nearshore_surface(scene, water, 4.1f, 0.0f, 1.0f, &y, &coverage));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, coverage);
}

static void test_one_driver_excites_flow_and_foam(void)
{
    JceWaterField *field = jce_scene_acquire_water_field(scene, water);
    TEST_ASSERT_NOT_NULL(field);
    JceWaterFieldSet *set = jce_scene_water_fields(scene);
    const JceWaterShoreSnapshot initial = snapshot();
    const int owner = 0, competing = 1;
    float delta = 0.0f, foam_peak = 0.0f;
    for (int tick = 0; tick < 240; ++tick) {
        TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &owner, 1.0 / 60.0));
        const JceWaterShoreSnapshot data = snapshot();
        TEST_ASSERT_EQUAL_UINT64(initial.identity, data.identity);
        TEST_ASSERT_FALSE(jce_water_field_set_advance(set, &competing, 1.0));
        TEST_ASSERT_EQUAL_UINT64(data.revision, snapshot().revision);
        for (int z = 6; z < 26; ++z) {
            for (int x = 7; x < 16; ++x) {
                const int i = z * data.side + x;
                delta = fmaxf(delta, fabsf(data.cells[i].depth + data.bed[i]));
                foam_peak = fmaxf(foam_peak, data.foam[i]);
            }
        }
    }
    TEST_ASSERT_TRUE(delta > 0.01f);
    TEST_ASSERT_TRUE(foam_peak > 0.003f);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &owner, -4.0));
    const JceWaterShoreSnapshot reset = snapshot();
    TEST_ASSERT_EQUAL_UINT64(initial.identity, reset.identity);
    for (int i = 0; i < reset.side * reset.side; ++i)
        TEST_ASSERT_EQUAL_FLOAT(0.0f, reset.foam[i]);
}

static void test_asset_revision_and_missing_coverage(void)
{
    JceWaterField *ambient = jce_scene_acquire_water_field(scene, water);
    TEST_ASSERT_NOT_NULL(ambient);
    const JceWaterShoreSnapshot first = snapshot();
    TEST_ASSERT_TRUE(jce_scene_touch_terrain(scene, terrain_path));
    TEST_ASSERT_EQUAL_PTR(ambient, jce_scene_acquire_water_field(scene, water));
    TEST_ASSERT_TRUE(first.identity != snapshot().identity);

    JceWaterNearshoreComponent config = *jce_scene_get_water_nearshore(scene, water);
    config.offset_x = 7.0f;
    jce_scene_set_water_nearshore(scene, water, &config);
    TEST_ASSERT_EQUAL_PTR(ambient, jce_scene_acquire_water_field(scene, water));
    TEST_ASSERT_NULL(jce_water_field_set_shore(jce_scene_water_fields(scene), water));

    config.offset_x = 0.0f;
    jce_scene_set_water_nearshore(scene, water, &config);
    TEST_ASSERT_EQUAL_PTR(ambient, jce_scene_acquire_water_field(scene, water));
    TEST_ASSERT_NOT_NULL(jce_water_field_set_shore(jce_scene_water_fields(scene), water));
    jce_scene_invalidate_terrain(scene, terrain_path);
    TEST_ASSERT_EQUAL_PTR(ambient, jce_scene_acquire_water_field(scene, water));
    TEST_ASSERT_NULL(jce_water_field_set_shore(jce_scene_water_fields(scene), water));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_translated_bed_and_dry_coverage);
    RUN_TEST(test_one_driver_excites_flow_and_foam);
    RUN_TEST(test_asset_revision_and_missing_coverage);
    return UNITY_END();
}
