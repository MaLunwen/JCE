#include <jce/api_scene.h>
#include <jce/os/core/jce_json.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void find_water(JceScene *s, JceEntity e, void *user)
{
    if (jce_scene_has_water(s, e)) *(JceEntity *)user = e;
}

static void test_spectrum_scene_round_trip_and_disable(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    JceEntity e = jce_scene_create_entity(scene, "Ocean");
    JceWaterComponent water = {0};
    water.water_mode = JCE_WATER_MODE_FFT;
    water.fft_resolution = 32;
    water.fft_patch_size = 64.0f;
    water.fft_wind_speed = 1.0f;
    water.fft_wind_dir_x = 1.0f;
    water.fft_amplitude = 0.0008f;
    water.fft_fetch = 100000.0f;
    water.fft_swell = 0.2f;
    jce_scene_set_water(scene, e, &water);
    JceWaterSpectrumComponent config = jce_scene_water_spectrum_default();
    config.model = JCE_WATER_FFT_SPECTRUM_TMA;
    config.depth = 8.0f;
    config.directional_blend = 0.3f;
    jce_scene_set_water_spectrum(scene, e, &config);
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    JceWaterSpectrumComponent *restored = jce_scene_get_water_spectrum(loaded, copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_UINT32(config.model, restored->model);
    TEST_ASSERT_EQUAL_FLOAT(config.depth, restored->depth);
    TEST_ASSERT_EQUAL_FLOAT(config.directional_blend, restored->directional_blend);
    TEST_ASSERT_EQUAL_UINT32(config.shading_flags, restored->shading_flags);
    JceWaterField *field = jce_scene_acquire_water_field(loaded, copy);
    TEST_ASSERT_NOT_NULL(field);
    const uint64_t revision = jce_water_field_revision(field);
    TEST_ASSERT_EQUAL_PTR(field, jce_scene_acquire_water_field(loaded, copy));
    TEST_ASSERT_EQUAL_UINT64(revision, jce_water_field_revision(field));
    const int id = jce_component_find("WaterSpectrum");
    TEST_ASSERT_TRUE(id != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(loaded, copy, id, false);
    TEST_ASSERT_EQUAL_PTR(field, jce_scene_acquire_water_field(loaded, copy));
    TEST_ASSERT_TRUE(jce_water_field_revision(field) > revision);
    JceWaterFieldDesc desc;
    TEST_ASSERT_TRUE(jce_scene_water_field_desc(loaded, copy, &desc));
    JceWaterField *legacy = jce_water_field_create(&desc);
    TEST_ASSERT_NOT_NULL(legacy);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(jce_water_field_fft(legacy)),
        jce_water_fft_height_data(jce_water_field_fft(field)), 32 * 32 * sizeof(float));
    jce_water_field_destroy(legacy);
    /* A dormant FFT extension must not reject a Gerstner body with no fetch. */
    jce_scene_set_comp_enabled(loaded, copy, id, true);
    water.water_mode = JCE_WATER_MODE_GERSTNER;
    water.fft_fetch = 0.0f;
    jce_scene_set_water(loaded, copy, &water);
    TEST_ASSERT_EQUAL_PTR(field, jce_scene_acquire_water_field(loaded, copy));
    TEST_ASSERT_EQUAL_INT(JCE_WATER_FIELD_GERSTNER, jce_water_field_model(field));
    jce_json_free(json);
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_invalid_version_keeps_authored_component(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity e = jce_scene_create_entity(scene, "Ocean");
    JceWaterSpectrumComponent config = jce_scene_water_spectrum_default();
    jce_scene_set_water_spectrum(scene, e, &config);
    config.struct_size += 16u;
    jce_scene_set_water_spectrum(scene, e, &config);
    TEST_ASSERT_EQUAL_UINT32(sizeof(config),
        jce_scene_get_water_spectrum(scene, e)->struct_size);
    config.version++;
    config.depth = 99.0f;
    jce_scene_set_water_spectrum(scene, e, &config);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, jce_scene_get_water_spectrum(scene, e)->depth);
    config.version = JCE_WATER_SPECTRUM_COMPONENT_VERSION;
    config.struct_size = sizeof(uint32_t);
    jce_scene_set_water_spectrum(scene, e, &config);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, jce_scene_get_water_spectrum(scene, e)->depth);
    jce_scene_destroy(scene);
}

static void test_crest_round_trip_rejects_invalid_values(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    JceEntity e = jce_scene_create_entity(scene, "Crest authoring");
    JceWaterComponent water = {0};
    jce_scene_set_water(scene, e, &water);
    JceWaterCrestComponent crest = jce_scene_water_crest_default();
    crest.start_height = 0.4f;
    crest.height_range = 0.8f;
    crest.fade_distance = 74.0f;
    crest.strength = 1.2f;
    crest.tint[2] = 0.3f;
    jce_scene_set_water_crest(scene, e, &crest);
    JceWaterCausticsComponent caustic = jce_scene_water_caustics_default();
    caustic.offset_x = -0.375f;
    caustic.offset_z = 0.125f;
    caustic.fade_depth = 3.25f;
    jce_scene_set_water_caustics(scene, e, &caustic);
    TEST_ASSERT_TRUE(jce_scene_has_water_crest(scene, e));
    JceWaterCrestComponent rejected = crest;
    rejected.height_range = 0.0f;
    jce_scene_set_water_crest(scene, e, &rejected);
    TEST_ASSERT_EQUAL_FLOAT(crest.height_range, jce_scene_get_water_crest(scene, e)->height_range);

    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(jce_scene_load_json(loaded, json));
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    const JceWaterCrestComponent *restored = jce_scene_get_water_crest(loaded, copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_MEMORY(&crest, restored, sizeof(crest));
    const JceWaterCausticsComponent *caustic_copy = jce_scene_get_water_caustics(loaded, copy);
    TEST_ASSERT_NOT_NULL(caustic_copy);
    TEST_ASSERT_EQUAL_MEMORY(&caustic, caustic_copy, sizeof(caustic));
    JceWaterCausticsComponent invalid_caustic = caustic;
    invalid_caustic.fade_depth = 0.0f;
    jce_scene_set_water_caustics(loaded, copy, &invalid_caustic);
    TEST_ASSERT_EQUAL_MEMORY(&caustic, jce_scene_get_water_caustics(loaded, copy), sizeof(caustic));
    jce_scene_remove_water_caustics(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_caustics(loaded, copy));
    jce_scene_remove_water_crest(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_crest(loaded, copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, copy));
    jce_json_free(json);
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_nearshore_terrain_round_trip_and_subtree(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    JceScene *prefab = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_NOT_NULL(prefab);
    const JceEntity water = jce_scene_create_entity(scene, "Shore water");
    const JceEntity terrain = jce_scene_create_entity(scene, "Shore terrain");
    const JceWaterComponent water_component = {0};
    const JceTerrainComponent terrain_component = {0};
    jce_scene_set_water(scene, water, &water_component);
    jce_scene_set_terrain(scene, terrain, &terrain_component);
    TEST_ASSERT_FALSE(jce_scene_has_water_nearshore(scene, water));

    JceWaterNearshoreComponent nearshore = jce_scene_water_nearshore_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(nearshore), nearshore.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_WATER_NEARSHORE_COMPONENT_VERSION, nearshore.version);
    TEST_ASSERT_EQUAL_UINT64(0, nearshore.terrain);
    jce_scene_set_water_nearshore(scene, water, &nearshore);
    TEST_ASSERT_TRUE(jce_scene_has_water_nearshore(scene, water));
    nearshore.terrain = terrain;
    nearshore.offset_x = -2.0f;
    nearshore.offset_z = 3.0f;
    nearshore.extent = 24.0f;
    nearshore.drag = 1.5f;
    nearshore.boundary_absorption = 10.0f;
    nearshore.foam_opacity = 0.5f;
    jce_scene_set_water_nearshore(scene, water, &nearshore);
    JceWaterNearshoreComponent invalid = nearshore;
    invalid.extent = 0.0f;
    jce_scene_set_water_nearshore(scene, water, &invalid);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.extent, jce_scene_get_water_nearshore(scene, water)->extent);
    const int component = jce_component_find("WaterNearshore");
    TEST_ASSERT_TRUE(component != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(scene, water, component, false);

    /* Occupy the source IDs in the destination so a verbatim reference fails. */
    TEST_ASSERT_TRUE(jce_scene_create_entity(loaded, "Existing water") != 0);
    TEST_ASSERT_TRUE(jce_scene_create_entity(loaded, "Existing terrain") != 0);
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(2, jce_scene_load_json(loaded, json));
    jce_json_free(json);
    JceEntity water_copy = 0, terrain_copy = 0;
    jce_scene_each_entity(loaded, find_water, &water_copy);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_query_by_name(loaded, "Shore terrain", &terrain_copy, 1));
    TEST_ASSERT_TRUE(terrain_copy != terrain);
    TEST_ASSERT_TRUE(jce_scene_has_terrain(loaded, terrain_copy));
    const JceWaterNearshoreComponent *restored = jce_scene_get_water_nearshore(loaded, water_copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_UINT64(terrain_copy, restored->terrain);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.offset_x, restored->offset_x);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.offset_z, restored->offset_z);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.extent, restored->extent);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.drag, restored->drag);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.boundary_absorption, restored->boundary_absorption);
    TEST_ASSERT_EQUAL_FLOAT(nearshore.foam_opacity, restored->foam_opacity);
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, water_copy, component));
    jce_scene_remove_water_nearshore(loaded, water_copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_nearshore(loaded, water_copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, water_copy));

    /* An external terrain reference clears when only the water subtree is copied. */
    json = jce_scene_save_subtree_json(scene, water);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(prefab, json));
    jce_json_free(json);
    water_copy = 0;
    jce_scene_each_entity(prefab, find_water, &water_copy);
    restored = jce_scene_get_water_nearshore(prefab, water_copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_UINT64(0, restored->terrain);

    jce_scene_destroy(prefab);
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_water_motion_authoring_round_trip(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    const JceEntity water = jce_scene_create_entity(scene, "Motion water");
    const JceWaterComponent base = {0};
    jce_scene_set_water(scene, water, &base);
    TEST_ASSERT_FALSE(jce_scene_has_water_motion(scene, water));

    JceWaterMotionComponent motion = jce_scene_water_motion_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(motion), motion.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_WATER_MOTION_COMPONENT_VERSION, motion.version);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, motion.cascade_fraction);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, motion.phase_rate);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, motion.horizontal_scale);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, motion.max_horizontal_displacement);
    motion.cascade_fraction = 0.35f;
    motion.phase_rate = 1.3f;
    motion.horizontal_scale = 1.5f;
    motion.max_horizontal_displacement = 4.0f;
    jce_scene_set_water_motion(scene, water, &motion);
    TEST_ASSERT_TRUE(jce_scene_has_water_motion(scene, water));

    JceWaterMotionComponent bad = motion;
    bad.phase_rate = NAN;
    jce_scene_set_water_motion(scene, water, &bad);
    bad = motion;
    bad.max_horizontal_displacement = 65.0f;
    jce_scene_set_water_motion(scene, water, &bad);
    bad = motion;
    ++bad.version;
    jce_scene_set_water_motion(scene, water, &bad);
    TEST_ASSERT_EQUAL_MEMORY(&motion, jce_scene_get_water_motion(scene, water), sizeof(motion));

    const int id = jce_component_find("WaterMotion");
    TEST_ASSERT_TRUE(id != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(scene, water, id, false);
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    jce_json_free(json);
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    const JceWaterMotionComponent *restored = jce_scene_get_water_motion(loaded, copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_MEMORY(&motion, restored, sizeof(motion));
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, copy, id));
    jce_scene_remove_water_motion(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_motion(loaded, copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, copy));
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_water_optics_defaults_round_trip_and_disable(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    const JceEntity water = jce_scene_create_entity(scene, "Optical water");
    const JceWaterComponent base = {0};
    jce_scene_set_water(scene, water, &base);
    TEST_ASSERT_FALSE(jce_scene_has_water_optics(scene, water));

    JceWaterOpticsComponent optics = jce_scene_water_optics_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(optics), optics.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_WATER_OPTICS_COMPONENT_VERSION, optics.version);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, optics.ssr_strength);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, optics.refraction_strength);
    optics.ssr_strength = 0.8f;
    optics.refraction_strength = 0.45f;
    jce_scene_set_water_optics(scene, water, &optics);
    TEST_ASSERT_TRUE(jce_scene_has_water_optics(scene, water));

    JceWaterOpticsComponent bad = optics;
    bad.ssr_strength = NAN;
    jce_scene_set_water_optics(scene, water, &bad);
    bad = optics;
    bad.refraction_strength = 1.01f;
    jce_scene_set_water_optics(scene, water, &bad);
    bad = optics;
    ++bad.version;
    jce_scene_set_water_optics(scene, water, &bad);
    TEST_ASSERT_EQUAL_MEMORY(&optics, jce_scene_get_water_optics(scene, water), sizeof(optics));

    const int component = jce_component_find("WaterOptics");
    TEST_ASSERT_TRUE(component != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(scene, water, component, false);
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    jce_json_free(json);
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    const JceWaterOpticsComponent *restored = jce_scene_get_water_optics(loaded, copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_MEMORY(&optics, restored, sizeof(optics));
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, copy, component));
    jce_scene_remove_water_optics(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_optics(loaded, copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, copy));
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_water_caustic_detail_defaults_round_trip_and_disable(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    const JceEntity water = jce_scene_create_entity(scene, "Detailed caustics");
    const JceWaterComponent base = {0};
    const JceWaterCausticsComponent projection = jce_scene_water_caustics_default();
    jce_scene_set_water(scene, water, &base);
    jce_scene_set_water_caustics(scene, water, &projection);
    TEST_ASSERT_FALSE(jce_scene_has_water_caustic_detail(scene, water));

    JceWaterCausticDetailComponent detail = jce_scene_water_caustic_detail_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(detail), detail.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_WATER_CAUSTIC_DETAIL_COMPONENT_VERSION, detail.version);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail.secondary_weight);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, detail.transition_start_m);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, detail.transition_end_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail.tile_size_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail.fine_period_ratio);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail.fine_weight);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, detail.fine_fade_depth_m);
    detail.secondary_weight = 0.75f;
    detail.transition_start_m = 1.0f;
    detail.transition_end_m = 8.0f;
    detail.tile_size_m = 3.5f;
    detail.fine_period_ratio = 0.125f;
    detail.fine_weight = 0.625f;
    detail.fine_fade_depth_m = 2.25f;
    jce_scene_set_water_caustic_detail(scene, water, &detail);
    TEST_ASSERT_TRUE(jce_scene_has_water_caustic_detail(scene, water));

    JceWaterCausticDetailComponent bad = detail;
    bad.transition_end_m = bad.transition_start_m;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    bad.secondary_weight = NAN;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    bad.tile_size_m = 129.0f;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    bad.fine_period_ratio = 0.51f;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    bad.fine_weight = NAN;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    bad.fine_fade_depth_m = 0.049f;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    bad = detail;
    ++bad.version;
    jce_scene_set_water_caustic_detail(scene, water, &bad);
    TEST_ASSERT_EQUAL_MEMORY(&detail, jce_scene_get_water_caustic_detail(scene, water), sizeof(detail));

    const int component = jce_component_find("WaterCausticDetail");
    TEST_ASSERT_TRUE(component != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(scene, water, component, false);
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    jce_json_free(json);
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    const JceWaterCausticDetailComponent *restored = jce_scene_get_water_caustic_detail(loaded, copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_MEMORY(&detail, restored, sizeof(detail));
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, copy, component));
    jce_scene_remove_water_caustic_detail(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_caustic_detail(loaded, copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, copy));
    TEST_ASSERT_EQUAL_MEMORY(&projection, jce_scene_get_water_caustics(loaded, copy), sizeof(projection));
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_double_caustic_json_defaults_and_preserves_shape(void)
{
    static const char source[] =
        "{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
        "\"scene\":{\"version\":1,\"entities\":[{\"id\":1,\"name\":\"Double caustics\","
        "\"components\":[{\"type\":\"Water\"},{\"type\":\"WaterCausticDetail\","
        "\"properties\":{\"version\":1,\"secondaryWeight\":0.75,"
        "\"transitionStartM\":0.5,\"transitionEndM\":4,\"tileSizeM\":3.5}}]}]}}";
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceJson *json = jce_json_parse(source, sizeof(source) - 1);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(scene, json));
    jce_json_free(json);
    JceEntity water = 0;
    jce_scene_each_entity(scene, find_water, &water);
    TEST_ASSERT_TRUE(water != 0);
    const JceWaterCausticDetailComponent *detail = jce_scene_get_water_caustic_detail(scene, water);
    TEST_ASSERT_NOT_NULL(detail);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, detail->secondary_weight);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, detail->transition_start_m);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, detail->transition_end_m);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, detail->tile_size_m);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail->fine_period_ratio);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, detail->fine_weight);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, detail->fine_fade_depth_m);

    json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    char *saved = jce_json_print(json, false);
    TEST_ASSERT_NOT_NULL(saved);
    TEST_ASSERT_NOT_NULL(strstr(saved, "\"secondaryWeight\""));
    TEST_ASSERT_NULL(strstr(saved, "\"finePeriodRatio\""));
    TEST_ASSERT_NULL(strstr(saved, "\"fineWeight\""));
    TEST_ASSERT_NULL(strstr(saved, "\"fineFadeDepthM\""));
    jce_json_free_string(saved);
    jce_json_free(json);
    jce_scene_destroy(scene);
}


static void test_water_normal_detail_round_trip_and_disable(void)
{
    JceScene *scene = jce_scene_create(), *loaded = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(loaded);
    const JceEntity entity = jce_scene_create_entity(scene, "Normal detail");
    const JceWaterComponent base = {0};
    jce_scene_set_water(scene, entity, &base);
    TEST_ASSERT_FALSE(jce_scene_has_water_normal_detail(scene, entity));
    TEST_ASSERT_NULL(jce_scene_get_water_normal_detail(scene, entity));
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    char *saved = jce_json_print(json, false);
    TEST_ASSERT_NOT_NULL(saved);
    TEST_ASSERT_NULL(strstr(saved, "WaterNormalDetail"));
    jce_json_free_string(saved);
    jce_json_free(json);

    JceWaterNormalDetailComponent value = jce_scene_water_normal_detail_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(value), value.struct_size);
    TEST_ASSERT_EQUAL_UINT32(1u, value.version);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, value.frequency_low);
    TEST_ASSERT_EQUAL_FLOAT(16.0f, value.frequency_high);
    TEST_ASSERT_EQUAL_FLOAT(0.14f, value.strength);
    /* Independent, reversed endpoint values must survive without sorting. */
    value.frequency_low = 64.0f;
    value.frequency_high = 0.1f;
    value.strength = 0.0f;
    value.struct_size += 16u;
    jce_scene_set_water_normal_detail(scene, entity, &value);
    value.struct_size = sizeof(value);
    TEST_ASSERT_TRUE(jce_scene_has_water_normal_detail(scene, entity));
    TEST_ASSERT_EQUAL_MEMORY(&value, jce_scene_get_water_normal_detail(scene, entity), sizeof(value));
    const int id = jce_component_find("WaterNormalDetail");
    TEST_ASSERT_TRUE(id != JCE_COMP_ID_INVALID);
    TEST_ASSERT_EQUAL_INT(id, jce_component_find("waterNormalDetail"));
    TEST_ASSERT_EQUAL_UINT64(0, jce_component_legacy_flag(id));
    uint32_t generic_size = 0;
    TEST_ASSERT_EQUAL_PTR(jce_scene_get_water_normal_detail(scene, entity),
        jce_scene_get_comp(scene, entity, id, &generic_size));
    TEST_ASSERT_EQUAL_UINT32(sizeof(value), generic_size);
    TEST_ASSERT_TRUE(jce_scene_set_comp(scene, entity, id, &value));
    TEST_ASSERT_EQUAL_MEMORY(&value, jce_scene_get_water_normal_detail(scene, entity), sizeof(value));
    TEST_ASSERT_TRUE(jce_scene_comp_enabled(scene, entity, id));
    jce_scene_set_comp_enabled(scene, entity, id, false);
    json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    jce_json_free(json);
    JceEntity copy = 0;
    jce_scene_each_entity(loaded, find_water, &copy);
    TEST_ASSERT_TRUE(copy != 0);
    TEST_ASSERT_EQUAL_MEMORY(&value, jce_scene_get_water_normal_detail(loaded, copy), sizeof(value));
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, copy, id));
    jce_scene_set_comp_enabled(loaded, copy, id, true);
    TEST_ASSERT_TRUE(jce_scene_comp_enabled(loaded, copy, id));
    const JceWaterComponent restored_base = *jce_scene_get_water(loaded, copy);
    jce_scene_remove_water_normal_detail(loaded, copy);
    jce_scene_remove_water_normal_detail(loaded, copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_normal_detail(loaded, copy));
    TEST_ASSERT_TRUE(jce_scene_has_water(loaded, copy));
    TEST_ASSERT_EQUAL_MEMORY(&restored_base, jce_scene_get_water(loaded, copy), sizeof(restored_base));
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_water_normal_detail_rejects_invalid_values(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    const JceEntity entity = jce_scene_create_entity(scene, "Normal validation");
    const JceWaterNormalDetailComponent valid = jce_scene_water_normal_detail_default();
    jce_scene_set_water_normal_detail(scene, entity, &valid);
    for (unsigned test = 0; test < 12; ++test) {
        JceWaterNormalDetailComponent bad = valid;
        switch (test) {
        case 0: bad.struct_size = sizeof(bad) - 1u; break;
        case 1: ++bad.version; break;
        case 2: bad.frequency_low = 0.099f; break;
        case 3: bad.frequency_low = 64.01f; break;
        case 4: bad.frequency_high = 0.099f; break;
        case 5: bad.frequency_high = 64.01f; break;
        case 6: bad.strength = -0.001f; break;
        case 7: bad.strength = 1.001f; break;
        case 8: bad.frequency_low = NAN; break;
        case 9: bad.frequency_high = NAN; break;
        case 10: bad.strength = NAN; break;
        default: bad.frequency_high = INFINITY; break;
        }
        jce_scene_set_water_normal_detail(scene, entity, &bad);
        TEST_ASSERT_EQUAL_MEMORY(&valid, jce_scene_get_water_normal_detail(scene, entity), sizeof(valid));
        const JceEntity absent = jce_scene_create_entity(scene, "Invalid normal detail");
        jce_scene_set_water_normal_detail(scene, absent, &bad);
        TEST_ASSERT_FALSE(jce_scene_has_water_normal_detail(scene, absent));
    }
    jce_scene_set_water_normal_detail(scene, entity, NULL);
    jce_scene_set_water_normal_detail(NULL, entity, &valid);
    jce_scene_set_water_normal_detail(scene, 0, &valid);
    jce_scene_remove_water_normal_detail(NULL, entity);
    TEST_ASSERT_NULL(jce_scene_get_water_normal_detail(NULL, entity));
    TEST_ASSERT_NULL(jce_scene_get_water_normal_detail(scene, 0));
    TEST_ASSERT_FALSE(jce_scene_has_water_normal_detail(NULL, entity));
    TEST_ASSERT_EQUAL_MEMORY(&valid, jce_scene_get_water_normal_detail(scene, entity), sizeof(valid));
    JceWaterNormalDetailComponent upper = valid;
    upper.strength = 1.0f;
    jce_scene_set_water_normal_detail(scene, entity, &upper);
    TEST_ASSERT_EQUAL_MEMORY(&upper, jce_scene_get_water_normal_detail(scene, entity), sizeof(upper));
    jce_scene_destroy(scene);
}

static void test_water_normal_detail_json_defaults_and_invalid(void)
{
    static const char *const properties[] = {
        "{}", "{\"strength\":1}",
        "{\"version\":2}", "{\"version\":true}",
        "{\"frequencyLow\":0}", "{\"frequencyHigh\":65}",
        "{\"frequencyLow\":\"6\"}", "{\"frequencyHigh\":false}",
        "{\"strength\":-1}", "{\"strength\":1.1}", "{\"strength\":null}"
    };
    for (unsigned test = 0; test < sizeof(properties) / sizeof(properties[0]); ++test) {
        char source[1024];
        const int length = snprintf(source, sizeof(source),
            "{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
            "\"scene\":{\"version\":1,\"entities\":[{\"id\":1,\"name\":\"Detail\","
            "\"components\":[{\"type\":\"Water\"},{\"type\":\"WaterNormalDetail\","
            "\"properties\":%s}]}]}}", properties[test]);
        TEST_ASSERT_TRUE(length > 0 && length < (int)sizeof(source));
        JceJson *json = jce_json_parse(source, (size_t)length);
        TEST_ASSERT_NOT_NULL(json);
        JceScene *scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(scene);
        TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(scene, json));
        jce_json_free(json);
        JceEntity entity = 0;
        jce_scene_each_entity(scene, find_water, &entity);
        TEST_ASSERT_TRUE(entity != 0);
        const JceWaterNormalDetailComponent *value = jce_scene_get_water_normal_detail(scene, entity);
        if (test < 2) {
            JceWaterNormalDetailComponent expected = jce_scene_water_normal_detail_default();
            if (test == 1) expected.strength = 1.0f;
            TEST_ASSERT_NOT_NULL(value);
            TEST_ASSERT_EQUAL_MEMORY(&expected, value, sizeof(expected));
        } else {
            TEST_ASSERT_NULL(value);
            TEST_ASSERT_FALSE(jce_scene_has_water_normal_detail(scene, entity));
        }
        jce_scene_destroy(scene);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spectrum_scene_round_trip_and_disable);
    RUN_TEST(test_invalid_version_keeps_authored_component);
    RUN_TEST(test_crest_round_trip_rejects_invalid_values);
    RUN_TEST(test_nearshore_terrain_round_trip_and_subtree);
    RUN_TEST(test_water_motion_authoring_round_trip);
    RUN_TEST(test_water_optics_defaults_round_trip_and_disable);
    RUN_TEST(test_water_caustic_detail_defaults_round_trip_and_disable);
    RUN_TEST(test_double_caustic_json_defaults_and_preserves_shape);
    RUN_TEST(test_water_normal_detail_round_trip_and_disable);
    RUN_TEST(test_water_normal_detail_rejects_invalid_values);
    RUN_TEST(test_water_normal_detail_json_defaults_and_invalid);
    return UNITY_END();
}
