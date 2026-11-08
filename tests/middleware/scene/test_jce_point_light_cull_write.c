/* Complete Scene owner + real Flecs OnSet/membership and actual selection.
 * Typed point-uniform payload ports never initialize a window/backend/GPU. */
#include "unity.h"
#ifndef JCE_POINT_LIGHT_SCENE_OWNER
#define JCE_POINT_LIGHT_SCENE_OWNER "middleware/scene/jce_scene.c"
#endif
#include JCE_POINT_LIGHT_SCENE_OWNER
#undef LOG_TAG
#include "middleware/scene/jce_sr_internal.h"
#include <jce/renderer/jce_lighting_system.h>

typedef struct PointWriteObservation {
    unsigned sets;
    JcePointLight last;
} PointWriteObservation;
static JceScene *point_scene;
static PointWriteObservation point_observation;
static bool point_defer_owned;
static void point_on_set(ecs_iter_t *it)
{
    PointWriteObservation *out = it->ctx;
    const JcePointLight *lights = ecs_field(it, JcePointLight, 0);
    for (int i = 0; i < it->count; ++i) { ++out->sets; out->last = lights[i]; }
}
static JcePointLight point_value(unsigned char padding)
{
    JcePointLight light;
    memset(&light, padding, sizeof light);
    light.position = jce_v3(0, 0, 0); light.color = jce_v3(0.25f, 0.5f, 0.75f);
    light.intensity = 1; light.radius = 7; light.casts_shadow = false;
    light.shadow_bias = 0.125f; light.layer_mask = 9;
    return light;
}
void setUp(void)
{
    memset(&point_observation, 0, sizeof point_observation);
    point_defer_owned = false;
    point_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(point_scene);
    ecs_observer_desc_t desc = {0};
    desc.query.terms[0].id = ecs_id(JcePointLight);
    desc.events[0] = EcsOnSet; desc.callback = point_on_set; desc.ctx = &point_observation;
    TEST_ASSERT_NOT_EQUAL_UINT64(0, ecs_observer_init(point_scene->world, &desc));
    jce_lighting_set_pixel_light_count(0);
}
void tearDown(void)
{
    /* A Unity assertion may leave the fixture's one deferred scope open.
     * Retire that owned scope before ecs_fini, preserving the first failure. */
    if (point_defer_owned && point_scene && ecs_is_deferred(point_scene->world))
        ecs_defer_end(point_scene->world);
    point_defer_owned = false;
    jce_scene_destroy(point_scene); point_scene = NULL;
    jce_lighting_set_pixel_light_count(0);
}
static JceEntity point_add(const char *name, float intensity)
{
    JceEntity e = jce_scene_create_entity(point_scene, name);
    JcePointLight light = point_value(0); light.intensity = intensity;
    jce_scene_set_point_light(point_scene, e, &light);
    return e;
}
static void test_equal_and_intensity_writes_keep_real_set_without_cull_churn(void)
{
    JceEntity e = point_add("point", 1);
    const uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
    const uint64_t structural = jce_scene_get_structural_epoch(point_scene);
    const float values[] = {1, 2, 0, -1, NAN, INFINITY, -INFINITY, -0.0f, 1};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        JcePointLight light = point_value((unsigned char)(0x55 + i));
        light.intensity = values[i];
        unsigned sets = point_observation.sets;
        jce_scene_set_point_light(point_scene, e, &light);
        TEST_ASSERT_EQUAL_UINT(sets + 1, point_observation.sets);
        TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_cull_data_gen(point_scene));
        TEST_ASSERT_EQUAL_UINT64(structural, jce_scene_get_structural_epoch(point_scene));
        const JcePointLight *stored = jce_scene_get_point_light(point_scene, e);
        if (isnan(values[i])) TEST_ASSERT_TRUE(isnan(stored->intensity));
        else {
            TEST_ASSERT_EQUAL_FLOAT(values[i], stored->intensity);
            if (values[i] == 0) TEST_ASSERT_EQUAL(!!signbit(values[i]), !!signbit(stored->intensity));
        }
        TEST_ASSERT_EQUAL_FLOAT(0.25f, stored->color.x); TEST_ASSERT_EQUAL_FLOAT(0.5f, stored->color.y);
        TEST_ASSERT_EQUAL_FLOAT(0.75f, stored->color.z); TEST_ASSERT_EQUAL_FLOAT(7, stored->radius);
        TEST_ASSERT_EQUAL_FLOAT(0.125f, stored->shadow_bias); TEST_ASSERT_EQUAL_UINT(9, stored->layer_mask);
        TEST_ASSERT_FALSE(stored->casts_shadow);
    }
}
static void test_every_other_field_retains_historical_invalidation(void)
{
    JceEntity e = point_add("fields", 1);
    for (unsigned field = 0; field < 10; ++field) {
        JcePointLight light = *jce_scene_get_point_light(point_scene, e);
        switch (field) {
        case 0: light.position.x += 1; break; case 1: light.position.y += 1; break;
        case 2: light.position.z += 1; break; case 3: light.color.x += 0.25f; break;
        case 4: light.color.y += 0.25f; break; case 5: light.color.z += 0.25f; break;
        case 6: light.radius += 1; break; case 7: light.casts_shadow = !light.casts_shadow; break;
        case 8: light.shadow_bias += 0.25f; break; default: light.layer_mask ^= 2; break;
        }
        uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
        unsigned sets = point_observation.sets;
        jce_scene_set_point_light(point_scene, e, &light);
        TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
        TEST_ASSERT_EQUAL_UINT(sets + 1, point_observation.sets);
        TEST_ASSERT_EQUAL_MEMORY(&light, jce_scene_get_point_light(point_scene, e), sizeof light);
    }
    JcePointLight light = *jce_scene_get_point_light(point_scene, e);
    light.position.x = 0; jce_scene_set_point_light(point_scene, e, &light);
    uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
    light.position.x = -0.0f; jce_scene_set_point_light(point_scene, e, &light);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
    TEST_ASSERT_TRUE(signbit(jce_scene_get_point_light(point_scene, e)->position.x));
    light.color.y = NAN; jce_scene_set_point_light(point_scene, e, &light);
    generation = jce_scene_get_cull_data_gen(point_scene);
    jce_scene_set_point_light(point_scene, e, &light);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));

    /* PointLight's prepare/apply hooks must not leak to adjacent light kinds. */
    JceSpotLight spot = {0};
    jce_scene_set_spot_light(point_scene, e, &spot);
    generation = jce_scene_get_cull_data_gen(point_scene);
    jce_scene_set_spot_light(point_scene, e, &spot);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
    JceAreaLight area = {0};
    jce_scene_set_area_light(point_scene, e, &area);
    generation = jce_scene_get_cull_data_gen(point_scene);
    jce_scene_set_area_light(point_scene, e, &area);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
}
static void test_borrowed_mutable_alias_cannot_claim_unchanged_fields(void)
{
    JceEntity e = point_add("borrowed", 1);
    JcePointLight *borrowed = jce_scene_get_point_light(point_scene, e);
    borrowed->radius = 13;
    uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
    unsigned sets = point_observation.sets;
    jce_scene_set_point_light(point_scene, e, borrowed);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
    TEST_ASSERT_EQUAL_UINT(sets + 1, point_observation.sets);
    TEST_ASSERT_EQUAL_FLOAT(13, point_observation.last.radius);
    borrowed = jce_scene_get_point_light(point_scene, e);
    generation = jce_scene_get_cull_data_gen(point_scene);
    jce_scene_set_point_light(point_scene, e, borrowed);
    TEST_ASSERT_EQUAL_UINT64(generation + 1, jce_scene_get_cull_data_gen(point_scene));
}
static void test_membership_guards_and_deferred_onset_remain_real(void)
{
    JceEntity e = jce_scene_create_entity(point_scene, "membership");
    JcePointLight light = point_value(0);
    uint64_t structural = jce_scene_get_structural_epoch(point_scene);
    jce_scene_set_point_light(NULL, e, &light);
    jce_scene_set_point_light(point_scene, 0, &light);
    jce_scene_set_point_light(point_scene, e, NULL);
    TEST_ASSERT_EQUAL_UINT(0, point_observation.sets);
    jce_scene_set_point_light(point_scene, e, &light);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(point_scene) > structural);
    TEST_ASSERT_TRUE(jce_scene_get_component_flags(point_scene, e) & JCE_COMP_FLAG_POINT_LIGHT);
    structural = jce_scene_get_structural_epoch(point_scene);
    uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
    unsigned sets = point_observation.sets;
    ecs_defer_begin(point_scene->world);
    point_defer_owned = true;
    light.intensity = 3; jce_scene_set_point_light(point_scene, e, &light);
    TEST_ASSERT_EQUAL_UINT(sets, point_observation.sets);
    /* Flecs copies an existing component immediately in this single-stage
     * world; its AddModified command defers OnSet until the scope ends. */
    TEST_ASSERT_EQUAL_FLOAT(3, jce_scene_get_point_light(point_scene, e)->intensity);
    ecs_defer_end(point_scene->world);
    point_defer_owned = false;
    TEST_ASSERT_EQUAL_UINT(sets + 1, point_observation.sets);
    TEST_ASSERT_EQUAL_FLOAT(3, jce_scene_get_point_light(point_scene, e)->intensity);
    TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_cull_data_gen(point_scene));
    TEST_ASSERT_EQUAL_UINT64(structural, jce_scene_get_structural_epoch(point_scene));
    jce_scene_remove_point_light(point_scene, e);
    TEST_ASSERT_FALSE(jce_scene_get_component_flags(point_scene, e) & JCE_COMP_FLAG_POINT_LIGHT);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(point_scene) > structural);
    structural = jce_scene_get_structural_epoch(point_scene);
    jce_scene_remove_point_light(point_scene, e);
    TEST_ASSERT_EQUAL_UINT64(structural, jce_scene_get_structural_epoch(point_scene));
    jce_scene_set_point_light(point_scene, e, &light);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(point_scene) > structural);
    TEST_ASSERT_TRUE(jce_scene_has_point_light(point_scene, e));
}

/* Actual gather and eight-lane packing blocks, with only their external
 * add-point destination replaced by a counted typed CPU payload owner. */
typedef struct PointUniformPayload {
    JcePointLightDesc point_lights[JCE_MAX_POINT_LIGHTS];
    uint32_t num_point;
    float packed_point[JCE_MAX_POINT_LIGHTS * 8];
} PointUniformPayload;
static PointUniformPayload point_payload;
static int point_add_payload(JceLightEnv *env, const JcePointLightDesc *light)
{
    TEST_ASSERT_EQUAL_PTR(&point_payload, env);
    TEST_ASSERT_TRUE(point_payload.num_point < JCE_MAX_POINT_LIGHTS);
    point_payload.point_lights[point_payload.num_point] = *light;
    return (int)point_payload.num_point++;
}
#define jce_light_env_add_point_light point_add_payload
static void point_gather(JceSceneRenderer *sr, JceScene *scene, JceEntity e)
{
    const JceTransform *xf = jce_scene_get_transform(scene, e);
#include JCE_POINT_LIGHT_GATHER_OWNER
}
#undef jce_light_env_add_point_light
static void point_pack(PointUniformPayload *env)
{
#include JCE_POINT_LIGHT_PACK_OWNER
}
static void test_reselection_and_uniform_payload_use_latest_intensity(void)
{
    JceEntity a = point_add("a", 2), b = point_add("b", 1);
    JceSceneRenderer *sr = calloc(1, sizeof *sr);
    TEST_ASSERT_NOT_NULL(sr);
    EntityList list = {0};
    sr->light_env = (JceLightEnv *)&point_payload;
    jce_lighting_set_pixel_light_count(1);
    const float values[] = {2, 0, -1, NAN, INFINITY, 4};
    const uint64_t generation = jce_scene_get_cull_data_gen(point_scene);
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        JcePointLight light = *jce_scene_get_point_light(point_scene, a);
        light.intensity = values[i];
        jce_scene_set_point_light(point_scene, a, &light);
        sr_select_lights(sr, point_scene, &list, NULL);
        TEST_ASSERT_EQUAL_UINT(1, sr->frame_sel_point_n);
        TEST_ASSERT_EQUAL_UINT64(i == 0 || i == 5 ? a : b, sr->frame_sel_point[0]);
        point_payload.num_point = 0;
        point_gather(sr, point_scene, a); point_gather(sr, point_scene, b);
        point_pack(&point_payload);
        TEST_ASSERT_EQUAL_UINT(1, point_payload.num_point);
        TEST_ASSERT_EQUAL_FLOAT(i == 0 ? 2 : i == 5 ? 4 : 1, point_payload.packed_point[7]);
        TEST_ASSERT_EQUAL_FLOAT(0.25f, point_payload.packed_point[4]);
        TEST_ASSERT_EQUAL_FLOAT(7, point_payload.packed_point[3]);
        TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_cull_data_gen(point_scene));
    }
    JcePointLight light = *jce_scene_get_point_light(point_scene, a);
    light.intensity = 0; jce_scene_set_point_light(point_scene, a, &light);
    jce_scene_set_point_light(point_scene, b, &light);
    sr_select_lights(sr, point_scene, &list, NULL);
    point_payload.num_point = 0; point_gather(sr, point_scene, a); point_gather(sr, point_scene, b);
    point_pack(&point_payload);
    TEST_ASSERT_EQUAL_UINT(0, sr->frame_sel_point_n);
    TEST_ASSERT_EQUAL_UINT(0, point_payload.num_point);
    TEST_ASSERT_EQUAL_FLOAT(0, point_payload.packed_point[7]);
    free(sr);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    UNITY_BEGIN();
    RUN_TEST(test_equal_and_intensity_writes_keep_real_set_without_cull_churn);
    RUN_TEST(test_every_other_field_retains_historical_invalidation);
    RUN_TEST(test_borrowed_mutable_alias_cannot_claim_unchanged_fields);
    RUN_TEST(test_membership_guards_and_deferred_onset_remain_real);
    RUN_TEST(test_reselection_and_uniform_payload_use_latest_intensity);
    return UNITY_END();
}
