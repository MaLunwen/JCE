/*
 * test_jce_comp_set_partial.c  comp_set is a PARTIAL update.
 *
 * jce_scene_component_apply_json is what the scripting API's comp_set() lands
 * on.  It used to hand the caller's JSON fragment straight to the component's
 * parse function, and every parse_* opens with `memset(&x, 0, sizeof x)` and
 * then fills each field from j_num/j_bool with a LITERAL default.  So a script
 * naming one key silently reverted every other field of that component to its
 * parse-time default:
 *
 *     comp_set(e, "CanvasGroup", '{"alpha": 0.5}')
 *
 * -- the obvious way to fade a panel -- also rewrote interactable,
 * blocks_raycasts and ignore_parent_groups, discarding what the scene authored.
 * Not a UI defect: EVERY component behaved this way.
 *
 * The sibling entry point jce_scene_rendering_apply_json had always merged, so
 * the two halves of the same scripting API disagreed about what "set" means.
 *
 * These pin the merge on three shapes: a bool that must survive, a float that
 * must survive, and a string that must survive -- strings are the case a
 * numeric-only spot check would miss.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_identity.h>
#include <jce/middleware/scene/jce_scene_sky_celestial.h>
#include <jce/middleware/scene/jce_scene_cloud_shape.h>
#include <jce/middleware/scene/jce_scene_water_motion.h>
#include <jce/middleware/scene/jce_scene_water_spectrum.h>
#include "middleware/scene/jce_component_registry_internal.h"
#ifndef JCE_LINE_COLORS_OWNER
#define JCE_LINE_COLORS_OWNER "middleware/scene/jce_scene_line_colors_internal.h"
#endif
#include JCE_LINE_COLORS_OWNER
#include <cjson/cJSON.h>
#include <flecs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Installed once in main, before the first JSON allocation. These ports keep
 * the CRT allocator for the binary's entire lifetime; only the bounded window
 * counters/failpoint are reset. Production never installs these hooks. */
typedef enum PatchFailure {
    PATCH_OK, PATCH_PARSE, PATCH_ARRAY, PATCH_FIRST,
    PATCH_LEAF, PATCH_REPLACE, PATCH_ADD, PATCH_NESTED_ARRAY
} PatchFailure;
static struct {
    bool active, armed, generic_line;
    size_t arrays, line_fast_paths;
    PatchFailure failure;
    size_t attempts, allocations, frees, fail_at, failures, whole_duplicates;
    size_t scalar_duplicates, array_duplicates;
} patch_probe;
static size_t patch_json_live;

static void *CJSON_CDECL patch_malloc(size_t bytes)
{
    if (patch_probe.active) {
        ++patch_probe.attempts;
        if (patch_probe.fail_at && patch_probe.attempts == patch_probe.fail_at) {
            ++patch_probe.failures;
            return NULL;
        }
    }
    void *value = malloc(bytes);
    if (value) {
        ++patch_json_live;
        if (patch_probe.active) ++patch_probe.allocations;
    }
    return value;
}

static void CJSON_CDECL patch_free(void *value)
{
    if (value) {
        --patch_json_live;
        if (patch_probe.active) ++patch_probe.frees;
    }
    free(value);
}

static void patch_arm(PatchFailure failure, size_t offset)
{
    if (patch_probe.active && !patch_probe.armed && patch_probe.failure == failure) {
        patch_probe.fail_at = patch_probe.attempts + offset;
        patch_probe.armed = true;
    }
}

static void patch_begin(PatchFailure failure)
{
    memset(&patch_probe, 0, sizeof patch_probe);
    patch_probe.active = true;
    patch_probe.failure = failure;
}

static cJSON *patch_parse(const char *json)
{
    patch_arm(PATCH_PARSE, 1);
    return cJSON_Parse(json);
}

static cJSON *patch_array(void)
{
    if (patch_probe.active) ++patch_probe.arrays;
    patch_arm(PATCH_ARRAY, 1);
    cJSON *value = cJSON_CreateArray();
    /* The next allocation belongs to the REAL registered serializer's first
     * object, not a fake serialize return. */
    patch_arm(PATCH_FIRST, 1);
    return value;
}

static cJSON *patch_duplicate(const cJSON *value, cJSON_bool recurse)
{
    if (patch_probe.active && cJSON_IsObject(value)) ++patch_probe.whole_duplicates;
    if (patch_probe.active && cJSON_IsArray(value)) ++patch_probe.array_duplicates;
    if (patch_probe.active && !cJSON_IsObject(value) && !cJSON_IsArray(value))
        ++patch_probe.scalar_duplicates;
    patch_arm(PATCH_LEAF, 2); /* real duplicate node succeeds; key copy fails */
    if (cJSON_IsArray(value)) patch_arm(PATCH_NESTED_ARRAY, 3);
    return cJSON_Duplicate(value, recurse);
}

static cJSON_bool patch_replace(cJSON *parent, const char *key, cJSON *value)
{
    patch_arm(PATCH_REPLACE, 1);
    return cJSON_ReplaceItemInObjectCaseSensitive(parent, key, value);
}

static cJSON_bool patch_add(cJSON *parent, const char *key, cJSON *value)
{
    patch_arm(PATCH_ADD, 1);
    return cJSON_AddItemToObject(parent, key, value);
}

/* A typed admission port selects the unchanged generic branch for a same-input
 * reference. It never substitutes a colour parser, serializer or scene setter. */
static bool patch_line_colors(JceScene *scene, JceEntity entity, const cJSON *patch)
{
    if (patch_probe.generic_line) return false;
    bool handled = scene_line_try_color_patch(scene, entity, patch);
    if (handled && patch_probe.active) ++patch_probe.line_fast_paths;
    return handled;
}

/* The complete private production component/merge owner, with only typed
 * allocation-operation observation ports. Registry, serializers, typed parse,
 * ECS data and cJSON remain real. Renaming avoids defining engine ABI twice. */
#ifndef JCE_COMPONENT_PATCH_OWNER
#define JCE_COMPONENT_PATCH_OWNER "middleware/scene/jce_scene_json_patch_internal.h"
#endif
#define jce_scene_component_apply_json patch_apply_component
#define scene_line_try_color_patch patch_line_colors
#define cJSON_Parse patch_parse
#define cJSON_CreateArray patch_array
#define cJSON_Duplicate patch_duplicate
#define cJSON_ReplaceItemInObjectCaseSensitive patch_replace
#define cJSON_AddItemToObject patch_add
#include JCE_COMPONENT_PATCH_OWNER
#undef cJSON_AddItemToObject
#undef cJSON_ReplaceItemInObjectCaseSensitive
#undef cJSON_Duplicate
#undef cJSON_CreateArray
#undef cJSON_Parse
#undef scene_line_try_color_patch
#undef jce_scene_component_apply_json

/* Engine-internal (jce_scene_internal.h); declared here so the test does not
 * need the private include path. */
bool  jce_scene_component_apply_json(JceScene *s, JceEntity e,
                                     const char *type, const char *json);
char *jce_scene_component_to_json(JceScene *s, JceEntity e, const char *type);
bool jce_scene_rendering_apply_json(JceScene *s, const char *json);

char *jce_scene_rendering_to_json(JceScene *s);
void jce_scene_json_free(char *s);

void setUp(void) { memset(&patch_probe, 0, sizeof patch_probe); }
void tearDown(void) { patch_probe.active = false; }

/* A bool and a bool: alpha is patched, the other three must not move. */
static void test_a_one_key_patch_leaves_the_rest_of_the_component_alone(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Panel");

    JceCanvasGroupComponent cg;
    memset(&cg, 0, sizeof cg);
    cg.alpha                = 1.0f;
    cg.interactable         = false;   /* NOT the parse default (true)  */
    cg.blocks_raycasts      = false;   /* NOT the parse default (true)  */
    cg.ignore_parent_groups = true;    /* NOT the parse default (false) */
    jce_scene_set_canvas_group(s, e, &cg);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "CanvasGroup", "{\"alpha\":0.5}"));

    JceCanvasGroupComponent *got = jce_scene_get_canvas_group(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, got->alpha);   /* patched      */
    TEST_ASSERT_FALSE(got->interactable);                  /* survived     */
    TEST_ASSERT_FALSE(got->blocks_raycasts);               /* survived     */
    TEST_ASSERT_TRUE(got->ignore_parent_groups);           /* survived     */

    jce_scene_destroy(s);
}

/* A string field: the shape a numeric spot check would miss. */
static void test_a_patch_does_not_blank_an_unmentioned_string(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Btn");

    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable  = true;
    bt.fade_duration = 0.25f;
    snprintf(bt.on_click_handler, sizeof bt.on_click_handler, "%s", "on_play");
    jce_scene_set_ui_button(s, e, &bt);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "UIButton", "{\"fadeDuration\":0.75}"));

    JceUIButtonComponent *got = jce_scene_get_ui_button(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.75f, got->fade_duration);
    TEST_ASSERT_EQUAL_STRING("on_play", got->on_click_handler);

    jce_scene_destroy(s);
}

/* An ABSENT component still takes the patch whole -- that is how a script adds
 * one, and it must not regress into "merge onto nothing produces nothing". */
static void test_comp_set_on_an_absent_component_still_adds_it(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "New");

    TEST_ASSERT_FALSE(jce_scene_has_canvas_group(s, e));
    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "CanvasGroup", "{\"alpha\":0.25}"));

    JceCanvasGroupComponent *got = jce_scene_get_canvas_group(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, got->alpha);
    /* and the unmentioned fields take their documented parse defaults */
    TEST_ASSERT_TRUE(got->interactable);
    TEST_ASSERT_TRUE(got->blocks_raycasts);

    jce_scene_destroy(s);
}

/* UIProgressBar has no dedicated script accessor -- the ui_get/ui_set family
 * covers slider, toggle and text only.  Its `value` is documented "read-only;
 * gameplay sets", so the question that decides whether that is a capability
 * gap or an ergonomics gap is whether the GENERIC comp_set can drive it.
 *
 * It can, but only now: before the merge landed, a script setting `value` also
 * reset min_value, max_value, direction, both colours and fill_sprite to their
 * parse defaults, which is not a usable way to drive a progress bar every
 * frame.  This pins the working path so the dedicated accessor is a
 * convenience rather than the only route. */
static void test_a_progress_bar_can_be_driven_through_the_generic_setter(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Bar");

    JceUIProgressBarComponent pb;
    memset(&pb, 0, sizeof pb);
    pb.value     = 0.0f;
    pb.min_value = 0.0f;
    pb.max_value = 200.0f;      /* NOT the parse default (1.0) */
    pb.direction = 3;           /* NOT the parse default (0)   */
    jce_scene_set_ui_progress_bar(s, e, &pb);

    TEST_ASSERT_TRUE(jce_scene_component_apply_json(
        s, e, "UIProgressBar", "{\"value\":120.0}"));

    JceUIProgressBarComponent *got = jce_scene_get_ui_progress_bar(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, got->value);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, got->max_value);  /* survived */
    TEST_ASSERT_EQUAL_INT(3, got->direction);                 /* survived */

    jce_scene_destroy(s);
}

static JceBillboardRendererComponent billboard_value(void)
{
    JceBillboardRendererComponent value;
    memset(&value, 0, sizeof value);
    snprintf(value.texture_path, sizeof value.texture_path, "%s", "keep/particle.png");
    value.mode = 2;
    value.size[0] = 3.0f;
    value.size[1] = 4.0f;
    value.color[0] = 0.2f;
    value.color[1] = 0.3f;
    value.color[2] = 0.4f;
    value.color[3] = 0.8f;
    value.visible = false;
    return value;
}

static void test_billboard_patch_moves_owned_tree_and_preserves_typed_fields(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "billboard-patch");
    JceBillboardRendererComponent before = billboard_value();
    jce_scene_set_billboard_renderer(scene, entity, &before);
    const size_t live = patch_json_live;
    patch_begin(PATCH_OK);
    bool first = patch_apply_component(scene, entity, "BillboardRenderer", "{\"colorA\":0.25}");
    patch_probe.active = false;
    const size_t first_allocations = patch_probe.allocations;
    const size_t first_frees = patch_probe.frees;
    const size_t first_duplicates = patch_probe.whole_duplicates;
    const size_t live_first = patch_json_live;
    patch_begin(PATCH_OK);
    bool second = patch_apply_component(scene, entity, "BillboardRenderer", "{\"colorA\":0.50}");
    patch_probe.active = false;
    JceBillboardRendererComponent after = *jce_scene_get_billboard_renderer(scene, entity);
    const size_t second_allocations = patch_probe.allocations;
    const size_t second_frees = patch_probe.frees;
    const size_t second_duplicates = patch_probe.whole_duplicates;
    const size_t live_second = patch_json_live;
    jce_scene_destroy(scene);
    before.color[3] = 0.5f;
    printf("COMP_PATCH_ALLOC first=%zu second=%zu whole_duplicates=%zu/%zu\n",
           first_allocations, second_allocations, first_duplicates, second_duplicates);
    TEST_ASSERT_TRUE(first);
    TEST_ASSERT_TRUE(second);
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof before);
    TEST_ASSERT_EQUAL_UINT64(first_allocations, first_frees);
    TEST_ASSERT_EQUAL_UINT64(second_allocations, second_frees);
    TEST_ASSERT_EQUAL_UINT64(live, live_first);
    TEST_ASSERT_EQUAL_UINT64(live, live_second);
    TEST_ASSERT_EQUAL_UINT64(first_allocations, second_allocations);
    /* Ownership oracle, not an unverified allocation quota. The alpha leaf is
     * duplicated for merge; a whole serialized component must never be cloned. */
    TEST_ASSERT_EQUAL_UINT64(0, first_duplicates);
    TEST_ASSERT_EQUAL_UINT64(0, second_duplicates);
}

static void test_explicit_component_construction_failures_preserve_ecs(void)
{
    const PatchFailure failures[] = { PATCH_PARSE, PATCH_ARRAY, PATCH_FIRST,
                                      PATCH_LEAF, PATCH_REPLACE, PATCH_ADD };
    for (size_t i = 0; i < sizeof failures / sizeof failures[0]; ++i) {
        JceScene *scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(scene);
        JceEntity entity = jce_scene_create_entity(scene, "failed-patch");
        JceBillboardRendererComponent before = billboard_value();
        jce_scene_set_billboard_renderer(scene, entity, &before);
        const size_t live = patch_json_live;
        patch_begin(failures[i]);
        bool accepted = patch_apply_component(scene, entity, "BillboardRenderer",
            failures[i] == PATCH_ADD ? "{\"unrecognized\":42}" : "{\"colorA\":0.25}");
        patch_probe.active = false;
        const size_t consumed = patch_probe.failures;
        const size_t live_after = patch_json_live;
        JceBillboardRendererComponent after = *jce_scene_get_billboard_renderer(scene, entity);
        jce_scene_destroy(scene);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(1, consumed, "real allocator failpoint was not consumed");
        TEST_ASSERT_FALSE_MESSAGE(accepted, "failed JSON construction was admitted");
        TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof before);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(live, live_after, "JSON ownership leaked on rejection");
    }
}

static void test_nested_merge_array_failure_is_propagated_and_owned(void)
{
    const size_t live = patch_json_live;
    cJSON *destination = cJSON_Parse("{\"branch\":{\"keep\":7,\"number\":1,\"items\":[1,2]}}");
    cJSON *patch = cJSON_Parse("{\"branch\":{\"number\":9,\"items\":[3,4]}}");
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(patch);
    patch_begin(PATCH_NESTED_ARRAY);
    bool accepted = rs_json_merge(destination, patch);
    patch_probe.active = false;
    const size_t consumed = patch_probe.failures;
    const cJSON *branch = cJSON_GetObjectItemCaseSensitive(destination, "branch");
    const double number = cJSON_GetObjectItemCaseSensitive(branch, "number")->valuedouble;
    const double retained = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(branch, "items"), 0)->valuedouble;
    const double requested = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(patch, "branch"), "items"), 0)->valuedouble;
    cJSON_Delete(destination);
    cJSON_Delete(patch);
    /* A preceding key can modify the disposable tree. Its owner must reject
     * the whole result before parsing it into ECS, never claim rollback here. */
    TEST_ASSERT_EQUAL_UINT64(1, consumed);
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, number);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, retained);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, requested);
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
}

static void test_nested_merge_preserves_objects_and_replaces_arrays(void)
{
    const size_t live = patch_json_live;
    cJSON *destination = cJSON_Parse("{\"branch\":{\"keep\":7,\"items\":[1,2,5]}}");
    cJSON *patch = cJSON_Parse("{\"branch\":{\"items\":[3,4],\"new\":{\"value\":9}}}");
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(patch);
    bool accepted = rs_json_merge(destination, patch);
    const cJSON *branch = cJSON_GetObjectItemCaseSensitive(destination, "branch");
    const double keep = cJSON_GetObjectItemCaseSensitive(branch, "keep")->valuedouble;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(branch, "items");
    const int length = cJSON_GetArraySize(items);
    const double first = cJSON_GetArrayItem(items, 0)->valuedouble;
    const double added = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(branch, "new"), "value")->valuedouble;
    const double source = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(patch, "branch"), "items"), 0)->valuedouble;
    cJSON_Delete(destination);
    cJSON_Delete(patch);
    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_EQUAL_DOUBLE(7.0, keep);
    TEST_ASSERT_EQUAL_INT(2, length);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, first);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, added);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, source);
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
}

static void test_actual_rendering_entry_rejects_failed_merge_before_set(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceSceneRenderingSettings before = jce_scene_rendering_settings_default();
    before.ambient_intensity = 0.125f;
    jce_scene_set_rendering_settings(scene, &before);
    before = *jce_scene_get_rendering_settings(scene);
    const size_t live = patch_json_live;
    patch_begin(PATCH_OK);
    bool healthy = jce_scene_rendering_apply_json(scene, "{\"ambient\":{\"intensity\":0.75}}");
    patch_probe.active = false;
    const size_t healthy_attempts = patch_probe.attempts;
    const float healthy_value = jce_scene_get_rendering_settings(scene)->ambient_intensity;
    jce_scene_set_rendering_settings(scene, &before);
    patch_begin(PATCH_OK);
    /* In the actual owner the final JSON allocation is the merge replacement
     * key. Derive its ordinal from this run, never a dependency-specific quota. */
    patch_probe.fail_at = healthy_attempts;
    bool accepted = jce_scene_rendering_apply_json(scene, "{\"ambient\":{\"intensity\":0.75}}");
    patch_probe.active = false;
    const size_t consumed = patch_probe.failures;
    const size_t live_after = patch_json_live;
    JceSceneRenderingSettings after = *jce_scene_get_rendering_settings(scene);
    jce_scene_destroy(scene);
    TEST_ASSERT_TRUE(healthy);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.75f, healthy_value);
    TEST_ASSERT_TRUE(healthy_attempts > 0);
    TEST_ASSERT_EQUAL_UINT64(1, consumed);
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof before);
    TEST_ASSERT_EQUAL_UINT64(live, live_after);
}

static void test_cloud_shape_patch_keeps_other_fields_and_round_trips(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *restored = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_NULL(jce_scene_get_cloud_shape_settings(scene));
    TEST_ASSERT_EQUAL_FLOAT(0.45f, jce_scene_cloud_shape_default().detail_erosion);
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"sky\":{\"cloudShape\":{\"detailErosion\":0.95,\"curlMetres\":96}}}}"));
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"sky\":{\"cloudCoverage\":0.51,\"cloudShape\":{\"curlMetres\":112}}}}"));
    TEST_ASSERT_FALSE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"sky\":{\"cloudCoverage\":0.8,\"cloudShape\":{\"curlMetres\":257}}}}"));
    const JceSceneCloudShapeSettings *value = jce_scene_get_cloud_shape_settings(scene);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(0.95f, value->detail_erosion);
    TEST_ASSERT_EQUAL_FLOAT(112.0f, value->curl_metres);
    TEST_ASSERT_EQUAL_FLOAT(0.51f, jce_scene_get_rendering_settings(scene)->cloud_coverage);

    char *json = jce_scene_rendering_to_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(restored, json));
    jce_scene_json_free(json);
    value = jce_scene_get_cloud_shape_settings(restored);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(112.0f, value->curl_metres);
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(restored,
        "{\"environment\":{\"sky\":{\"cloudShape\":null}}}"));
    TEST_ASSERT_NULL(jce_scene_get_cloud_shape_settings(restored));
    jce_scene_destroy(restored);
    jce_scene_destroy(scene);
}

static void test_terrain_snow_look_merge_reset_and_validation(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *restored = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(restored);
    char *json = jce_scene_rendering_to_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NULL(strstr(json, "\"snowLook\""));
    jce_scene_json_free(json);

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"type\":2,\"snowLook\":"
        "{\"brightness\":0.08,\"warmth\":-0.5,\"lowlandTint\":0.75}}}}"));
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"snowLook\":{\"microNormalStrength\":0.5}}}}"));
    TEST_ASSERT_FALSE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"type\":1,\"snowLook\":{\"lowlandTint\":1.01}}}}"));
    TEST_ASSERT_FALSE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"snowLook\":{\"warmth\":\"cool\"}}}}"));

    json = jce_scene_rendering_to_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(restored, json));
    cJSON *rendering = cJSON_Parse(json);
    jce_scene_json_free(json);
    TEST_ASSERT_NOT_NULL(rendering);
    TEST_ASSERT_EQUAL_INT(2, jce_scene_get_rendering_settings(restored)->weather_type);
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(rendering, "environment");
    const cJSON *weather = cJSON_GetObjectItemCaseSensitive(environment, "weather");
    const cJSON *look = cJSON_GetObjectItemCaseSensitive(weather, "snowLook");
    TEST_ASSERT_TRUE(cJSON_IsObject(look));
    TEST_ASSERT_EQUAL_FLOAT(0.08f, cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(look, "brightness")));
    TEST_ASSERT_EQUAL_FLOAT(-0.50f, cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(look, "warmth")));
    TEST_ASSERT_EQUAL_FLOAT(0.75f, cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(look, "lowlandTint")));
    TEST_ASSERT_EQUAL_FLOAT(0.50f, cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(look, "microNormalStrength")));
    cJSON_Delete(rendering);
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(restored,
        "{\"environment\":{\"weather\":{\"snowLook\":null}}}"));
    json = jce_scene_rendering_to_json(restored);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NULL(strstr(json, "\"snowLook\""));
    jce_scene_json_free(json);
    jce_scene_destroy(restored);
    jce_scene_destroy(scene);
}

static void test_rendering_patch_keeps_celestial_and_rejects_invalid_updates(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceSceneSkyCelestialSettings value = jce_scene_sky_celestial_default();
    value.star_intensity = 1.5f;
    value.moon_intensity = 1.25f;
    value.solar_flare_intensity = 0.75f;
    value.moon_phase = 0.5f;
    TEST_ASSERT_TRUE(jce_scene_set_sky_celestial_settings(scene, &value));

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"timeOfDay\":{\"hour\":23.5},\"sky\":"
        "{\"mode\":4,\"cloudCoverage\":0.25}}}"));
    const JceSceneRenderingSettings *rendering = jce_scene_get_rendering_settings(scene);
    TEST_ASSERT_EQUAL_FLOAT(23.5f, rendering->tod_hour);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_PHYSICAL, rendering->sky_mode);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, rendering->cloud_coverage);
    const JceSceneSkyCelestialSettings *celestial = jce_scene_get_sky_celestial_settings(scene);
    TEST_ASSERT_NOT_NULL(celestial);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, celestial->star_intensity);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, celestial->moon_intensity);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, celestial->solar_flare_intensity);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, celestial->moon_phase);

    char *current = jce_scene_rendering_to_json(scene);
    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_NOT_NULL(strstr(current, "\"celestial\""));
    TEST_ASSERT_NOT_NULL(strstr(current, "\"solarFlareIntensity\""));
    jce_scene_json_free(current);

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"sky\":{\"celestial\":{\"moonPhase\":0.25}}}}"));
    TEST_ASSERT_FALSE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"timeOfDay\":{\"hour\":2},\"sky\":"
        "{\"celestial\":{\"moonPhase\":2}}}}"));
    TEST_ASSERT_EQUAL_FLOAT(23.5f, jce_scene_get_rendering_settings(scene)->tod_hour);
    celestial = jce_scene_get_sky_celestial_settings(scene);
    TEST_ASSERT_NOT_NULL(celestial);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, celestial->star_intensity);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, celestial->moon_phase);

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"sky\":{\"celestial\":null}}}"));
    TEST_ASSERT_NULL(jce_scene_get_sky_celestial_settings(scene));
    jce_scene_destroy(scene);
}

typedef struct PatchPointSetObservation {
    JceEntity entity;
    unsigned sets;
    float last_intensity;
} PatchPointSetObservation;
static void patch_point_on_set(ecs_iter_t *it)
{
    PatchPointSetObservation *out = it->ctx;
    const JcePointLight *lights = ecs_field(it, JcePointLight, 0);
    for (int i = 0; i < it->count; ++i) if (it->entities[i] == out->entity) {
        ++out->sets; out->last_intensity = lights[i].intensity;
    }
}
static void patch_observe_point_set(JceScene *scene, PatchPointSetObservation *out)
{
    ecs_world_t *world = jce_scene_get_world(scene);
    ecs_entity_t component = ecs_lookup(world, "JcePointLight");
    TEST_ASSERT_NOT_EQUAL_UINT64(0, component);
    ecs_observer_desc_t descriptor = {0};
    descriptor.query.terms[0].id = component;
    descriptor.events[0] = EcsOnSet;
    descriptor.callback = patch_point_on_set; descriptor.ctx = out;
    TEST_ASSERT_NOT_EQUAL_UINT64(0, ecs_observer_init(world, &descriptor));
}

static void test_equal_scalar_patch_saves_allocations_without_skipping_typed_set(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "equal-light-patch");
    JcePointLight light;
    memset(&light, 0, sizeof light);
    light.color.x = 0.5f; light.color.y = 0.25f; light.color.z = 1.0f;
    light.intensity = 1.0f; light.radius = 3.0f; light.layer_mask = 7;
    jce_scene_set_point_light(scene, entity, &light);
    const size_t live = patch_json_live;
    PatchPointSetObservation observation = {entity, 0, 0};
    patch_observe_point_set(scene, &observation);
    patch_begin(PATCH_OK);
    bool changed = patch_apply_component(scene, entity, "Light", "{\"intensity\":0.25}");
    patch_probe.active = false;
    const size_t changed_allocations = patch_probe.allocations;
    const size_t changed_frees = patch_probe.frees;
    const size_t changed_copies = patch_probe.scalar_duplicates;
    const unsigned changed_sets = observation.sets;
    patch_begin(PATCH_OK);
    bool equal = patch_apply_component(scene, entity, "Light", "{\"intensity\":0.25}");
    patch_probe.active = false;
    const size_t equal_allocations = patch_probe.allocations;
    const size_t equal_frees = patch_probe.frees;
    const size_t equal_copies = patch_probe.scalar_duplicates;
    const unsigned equal_sets = observation.sets - changed_sets;
    light.intensity = 0.25f;
    const JcePointLight after = *jce_scene_get_point_light(scene, entity);
    const size_t live_after = patch_json_live;
    jce_scene_destroy(scene);
    printf("COMP_PATCH_SCALAR_ALLOC changed=%zu equal=%zu copies=%zu/%zu typed_sets=%llu/%llu\n",
           changed_allocations, equal_allocations, changed_copies, equal_copies,
           (unsigned long long)changed_sets, (unsigned long long)equal_sets);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_TRUE(equal);
    TEST_ASSERT_EQUAL_MEMORY(&light, &after, sizeof light);
    /* Actual ECS OnSet observes both parser/typed writes independently of cull
     * generation, including the equal primitive path. */
    TEST_ASSERT_EQUAL_UINT(1, changed_sets);
    TEST_ASSERT_EQUAL_UINT(1, equal_sets);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, observation.last_intensity);
    TEST_ASSERT_TRUE_MESSAGE(equal_allocations < changed_allocations,
        "equal scalar still allocated its merge copy");
    TEST_ASSERT_EQUAL_UINT64(1, changed_copies);
    TEST_ASSERT_EQUAL_UINT64(0, equal_copies);
    TEST_ASSERT_EQUAL_UINT64(changed_allocations, changed_frees);
    TEST_ASSERT_EQUAL_UINT64(equal_allocations, equal_frees);
    TEST_ASSERT_EQUAL_UINT64(live, live_after);
}

static void test_equal_prefix_then_failed_changed_leaf_cannot_reach_typed_set(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "mixed-failed-light-patch");
    JcePointLight before;
    memset(&before, 0, sizeof before);
    before.color.x = 1.0f; before.radius = 3.0f; before.intensity = 1.0f;
    jce_scene_set_point_light(scene, entity, &before);
    const uint64_t generation = jce_scene_get_cull_data_gen(scene);
    PatchPointSetObservation observation = {entity, 0, 0};
    patch_observe_point_set(scene, &observation);
    const size_t live = patch_json_live;
    patch_begin(PATCH_LEAF);
    bool accepted = patch_apply_component(scene, entity, "Light",
        "{\"radius\":3,\"intensity\":0.25}");
    patch_probe.active = false;
    const size_t failures = patch_probe.failures;
    const size_t live_after = patch_json_live;
    const uint64_t generation_after = jce_scene_get_cull_data_gen(scene);
    const JcePointLight after = *jce_scene_get_point_light(scene, entity);
    jce_scene_destroy(scene);
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL_UINT64(1, failures);
    TEST_ASSERT_EQUAL_UINT64(generation, generation_after);
    TEST_ASSERT_EQUAL_UINT(0, observation.sets);
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof before);
    TEST_ASSERT_EQUAL_UINT64(live, live_after);
}

static void test_all_equal_primitives_elide_but_nested_arrays_still_copy(void)
{
    const char *json = "{\"number\":1,\"text\":\"same bytes\",\"yes\":true,\"no\":false,"
        "\"nothing\":null,\"branch\":{\"n\":7,\"items\":[1,2]}}";
    const size_t live = patch_json_live;
    cJSON *destination = cJSON_Parse(json), *patch = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(patch);
    patch_begin(PATCH_OK);
    bool accepted = rs_json_merge(destination, patch);
    patch_probe.active = false;
    const size_t scalars = patch_probe.scalar_duplicates;
    const size_t arrays = patch_probe.array_duplicates;
    const cJSON *branch = cJSON_GetObjectItemCaseSensitive(destination, "branch");
    TEST_ASSERT_EQUAL_DOUBLE(7.0, cJSON_GetObjectItemCaseSensitive(branch, "n")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("same bytes", cJSON_GetObjectItemCaseSensitive(destination, "text")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(destination, "yes")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(destination, "no")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(destination, "nothing")));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(branch, "items")));
    cJSON_Delete(destination);
    cJSON_Delete(patch);
    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_EQUAL_UINT64(0, scalars);
    TEST_ASSERT_EQUAL_UINT64(1, arrays);
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
}

static void test_scalar_precision_sign_type_and_string_changes_are_not_elided(void)
{
    static const char *const before[] = {
        "{\"v\":1}", "{\"v\":0}", "{\"v\":-0}",
        "{\"v\":9007199254740991}", "{\"v\":true}",
        "{\"v\":null}", "{\"v\":1}", "{\"v\":\"same\"}", "{\"v\":\"Same\"}"
    };
    static const char *const after[] = {
        "{\"v\":1.0000000000000002}", "{\"v\":-0}", "{\"v\":0}",
        "{\"v\":9007199254740992}", "{\"v\":false}",
        "{\"v\":false}", "{\"v\":\"1\"}", "{\"v\":\"same prefix\"}", "{\"v\":\"same\"}"
    };
    for (size_t i = 0; i < sizeof before / sizeof before[0]; ++i) {
        const size_t live = patch_json_live;
        cJSON *destination = cJSON_Parse(before[i]), *patch = cJSON_Parse(after[i]);
        TEST_ASSERT_NOT_NULL(destination);
        TEST_ASSERT_NOT_NULL(patch);
        const cJSON *expected = cJSON_GetObjectItemCaseSensitive(patch, "v");
        patch_begin(PATCH_OK);
        bool accepted = rs_json_merge(destination, patch);
        patch_probe.active = false;
        const size_t copies = patch_probe.scalar_duplicates;
        const cJSON *got = cJSON_GetObjectItemCaseSensitive(destination, "v");
        TEST_ASSERT_EQUAL_INT(expected->type & 0xff, got->type & 0xff);
        if (cJSON_IsNumber(expected))
            TEST_ASSERT_EQUAL_MEMORY(&expected->valuedouble, &got->valuedouble, sizeof(double));
        if (cJSON_IsString(expected)) TEST_ASSERT_EQUAL_STRING(expected->valuestring, got->valuestring);
        cJSON_Delete(destination);
        cJSON_Delete(patch);
        TEST_ASSERT_TRUE(accepted);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(1, copies, after[i]);
        TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
    }
}


/* Line patch controls execute the complete private owner and compare it with
 * that same owner's unchanged generic branch using real serializers/parsers.
 * Compare named storage, not the C struct's unobservable trailing padding. */
static JceLineRendererComponent patch_line_value(int count)
{
    JceLineRendererComponent value;
    memset(&value, 0, sizeof value);
    const char *material = "keep/line.material";
    memcpy(value.material_path, material, strlen(material) + 1);
    memset(value.material_path + strlen(material) + 1, 0x5a,
           sizeof value.material_path - strlen(material) - 1);
    value.position_count = count;
    for (int i = 0; i < JCE_LINE_MAX_POINTS; ++i) {
        value.positions[i][0] = (float)i + 0.25f;
        value.positions[i][1] = -(float)i - 0.5f;
        value.positions[i][2] = i == 0 ? -0.0f : (float)i * 0.125f;
    }
    value.width_start = -0.25f;
    value.width_end = 0.625f;
    for (int i = 0; i < 4; ++i) {
        value.color_start[i] = (float)i * 0.25f;
        value.color_end[i] = 1.25f + (float)i * 0.125f;
    }
    value.use_world_space = false;
    value.loop = true;
    return value;
}

static void patch_assert_line_equal(const JceLineRendererComponent *expected,
                                    const JceLineRendererComponent *actual)
{
    TEST_ASSERT_EQUAL_MEMORY(expected->material_path, actual->material_path,
                             sizeof expected->material_path);
    TEST_ASSERT_EQUAL_INT(expected->position_count, actual->position_count);
    TEST_ASSERT_EQUAL_MEMORY(expected->positions, actual->positions, sizeof expected->positions);
    TEST_ASSERT_EQUAL_MEMORY(&expected->width_start, &actual->width_start, sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(&expected->width_end, &actual->width_end, sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(expected->color_start, actual->color_start, sizeof expected->color_start);
    TEST_ASSERT_EQUAL_MEMORY(expected->color_end, actual->color_end, sizeof expected->color_end);
    TEST_ASSERT_EQUAL_INT(expected->use_world_space, actual->use_world_space);
    TEST_ASSERT_EQUAL_INT(expected->loop, actual->loop);
}

static void patch_assert_line_discarded_storage(const JceLineRendererComponent *value)
{
    const size_t end = strlen(value->material_path) + 1;
    for (size_t i = end; i < sizeof value->material_path; ++i)
        TEST_ASSERT_EQUAL_INT(0, value->material_path[i]);
    const float zero = 0.0f;
    for (int i = value->position_count; i < JCE_LINE_MAX_POINTS; ++i)
        for (int axis = 0; axis < 3; ++axis)
            TEST_ASSERT_EQUAL_MEMORY(&zero, &value->positions[i][axis], sizeof zero);
}

typedef struct LinePatchObservation {
    JceEntity entity;
    unsigned sets;
} LinePatchObservation;

static void patch_line_on_set(ecs_iter_t *it)
{
    LinePatchObservation *out = it->ctx;
    for (int i = 0; i < it->count; ++i)
        if (it->entities[i] == out->entity) ++out->sets;
}

static void patch_observe_line_set(JceScene *scene, LinePatchObservation *out)
{
    ecs_world_t *world = jce_scene_get_world(scene);
    ecs_entity_t component = ecs_lookup(world, "JceLineRendererComponent");
    TEST_ASSERT_NOT_EQUAL_UINT64(0, component);
    ecs_observer_desc_t descriptor = {0};
    descriptor.query.terms[0].id = component;
    descriptor.events[0] = EcsOnSet;
    descriptor.callback = patch_line_on_set;
    descriptor.ctx = out;
    TEST_ASSERT_NOT_EQUAL_UINT64(0, ecs_observer_init(world, &descriptor));
}

typedef struct LinePatchResult {
    bool accepted;
    size_t allocations, frees, arrays, fast_paths;
    JceLineRendererComponent value;
} LinePatchResult;

static LinePatchResult patch_apply_line(JceScene *scene, JceEntity entity,
                                        const char *type, const char *json,
                                        bool generic)
{
    LinePatchResult result;
    memset(&result, 0, sizeof result);
    const size_t live = patch_json_live;
    patch_begin(PATCH_OK);
    patch_probe.generic_line = generic;
    result.accepted = patch_apply_component(scene, entity, type, json);
    patch_probe.active = false;
    result.allocations = patch_probe.allocations;
    result.frees = patch_probe.frees;
    result.arrays = patch_probe.arrays;
    result.fast_paths = patch_probe.line_fast_paths;
    const JceLineRendererComponent *value = jce_scene_get_line_renderer(scene, entity);
    if (value) result.value = *value;
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
    TEST_ASSERT_EQUAL_UINT64(result.allocations, result.frees);
    return result;
}

static void test_line_full_json_roundtrip_uses_shared_color_parser(void)
{
    const size_t live = patch_json_live;
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity source = jce_scene_create_entity(scene, "line-roundtrip-source");
    JceEntity restored = jce_scene_create_entity(scene, "line-roundtrip-restored");
    JceLineRendererComponent before = patch_line_value(14);
    /* Full text serialization has canonical inactive storage; signed-zero
     * transport is exercised separately by the existing partial-patch tests. */
    const size_t material_size = strlen(before.material_path) + 1;
    memset(before.material_path + material_size, 0, sizeof before.material_path - material_size);
    memset(before.positions + before.position_count, 0,
           (JCE_LINE_MAX_POINTS - (size_t)before.position_count) * sizeof before.positions[0]);
    before.positions[0][2] = 0.0f;
    jce_scene_set_line_renderer(scene, source, &before);
    char *serialized = jce_scene_component_to_json(scene, source, "LineRenderer");
    TEST_ASSERT_NOT_NULL(serialized);
    TEST_ASSERT_TRUE(jce_scene_component_apply_json(scene, restored, "LineRenderer", serialized));
    patch_assert_line_equal(&before, jce_scene_get_line_renderer(scene, restored));

    /* Real serializer bytes and the complete shared full-parser helper. The
     * ordinary missing-read owner must fail here, independently of fast-patch
     * allocation counts or the linked engine's separate helper instantiation. */
    JceJson *json = jce_json_parse(serialized, 0);
    TEST_ASSERT_NOT_NULL(json);
    JceLineRendererComponent colors;
    memset(&colors, 0, sizeof colors);
    scene_line_parse_colors(&colors, json, false);
    TEST_ASSERT_EQUAL_MEMORY(before.color_start, colors.color_start, sizeof before.color_start);
    TEST_ASSERT_EQUAL_MEMORY(before.color_end, colors.color_end, sizeof before.color_end);
    jce_json_free(json);
    cJSON_free(serialized);
    jce_scene_destroy(scene);
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
}

static void test_line_colors_avoid_point_json_and_keep_real_typed_writes(void)
{
    static const char *json = "{\"colorStartR\":-0,\"colorStartG\":0.125,"
        "\"colorStartB\":0.25,\"colorStartA\":0.375,\"colorEndR\":0.5,"
        "\"colorEndG\":0.625,\"colorEndB\":0.75,\"colorEndA\":1.5}";
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "line-color-owner");
    JceEntity reference = jce_scene_create_entity(scene, "line-color-generic");
    JceEntity linked = jce_scene_create_entity(scene, "line-color-linked");
    JceLineRendererComponent before = patch_line_value(14);
    jce_scene_set_line_renderer(scene, entity, &before);
    jce_scene_set_line_renderer(scene, reference, &before);
    jce_scene_set_line_renderer(scene, linked, &before);
    LinePatchObservation observation = {entity, 0};
    patch_observe_line_set(scene, &observation);
    LinePatchResult fast = patch_apply_line(scene, entity, "LineRenderer", json, false);
    LinePatchResult generic = patch_apply_line(scene, reference, "LineRenderer", json, true);
    TEST_ASSERT_TRUE(jce_scene_component_apply_json(scene, linked, "LineRenderer", json));
    patch_assert_line_equal(&generic.value, &fast.value);
    patch_assert_line_equal(&generic.value, jce_scene_get_line_renderer(scene, linked));
    TEST_ASSERT_EQUAL_UINT(1, observation.sets);
    LinePatchResult equal = patch_apply_line(scene, entity, "LineRenderer", json, false);
    TEST_ASSERT_EQUAL_UINT(2, observation.sets);
    patch_assert_line_equal(&fast.value, &equal.value);
    patch_assert_line_discarded_storage(&fast.value);
    TEST_ASSERT_EQUAL_INT(14, fast.value.position_count);
    TEST_ASSERT_EQUAL_MEMORY(before.positions, fast.value.positions, 14 * sizeof before.positions[0]);
    TEST_ASSERT_EQUAL_STRING("keep/line.material", fast.value.material_path);
    TEST_ASSERT_EQUAL_FLOAT(-0.25f, fast.value.width_start);
    TEST_ASSERT_EQUAL_FLOAT(0.625f, fast.value.width_end);
    TEST_ASSERT_FALSE(fast.value.use_world_space);
    TEST_ASSERT_TRUE(fast.value.loop);
    const float start[] = {-0.0f, 0.125f, 0.25f, 0.375f};
    const float end[] = {0.5f, 0.625f, 0.75f, 1.5f};
    TEST_ASSERT_EQUAL_MEMORY(start, fast.value.color_start, sizeof start);
    TEST_ASSERT_EQUAL_MEMORY(end, fast.value.color_end, sizeof end);
    patch_begin(PATCH_OK);
    cJSON *parsed = cJSON_Parse(json);
    cJSON_Delete(parsed);
    patch_probe.active = false;
    const size_t parse_allocations = patch_probe.allocations;
    TEST_ASSERT_TRUE(fast.accepted);
    TEST_ASSERT_TRUE(equal.accepted);
    TEST_ASSERT_TRUE(generic.accepted);
    TEST_ASSERT_EQUAL_UINT64(1, fast.fast_paths);
    TEST_ASSERT_EQUAL_UINT64(1, equal.fast_paths);
    TEST_ASSERT_EQUAL_UINT64(0, fast.arrays);
    TEST_ASSERT_EQUAL_UINT64(1, generic.arrays);
    TEST_ASSERT_EQUAL_UINT64(parse_allocations, fast.allocations);
    TEST_ASSERT_TRUE_MESSAGE(fast.allocations < generic.allocations,
                             "colour-only patch still constructed the point JSON");
    printf("COMP_PATCH_LINE_ALLOC points=14 fast=%zu generic=%zu parse=%zu typed_sets=%u\n",
           fast.allocations, generic.allocations, parse_allocations, observation.sets);
    jce_scene_destroy(scene);
}

static void test_each_partial_line_color_preserves_other_fields_and_alias(void)
{
    static const char *const keys[] = {"colorStartR", "colorStartG", "colorStartB", "colorStartA",
                                      "colorEndR", "colorEndG", "colorEndB", "colorEndA"};
    for (int i = 0; i < 8; ++i) {
        JceScene *scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(scene);
        JceEntity entity = jce_scene_create_entity(scene, "partial-line-color");
        JceEntity reference = jce_scene_create_entity(scene, "partial-line-reference");
        JceLineRendererComponent before = patch_line_value(i % 3 == 0 ? 0 :
                                                         (i % 3 == 1 ? 14 : JCE_LINE_MAX_POINTS));
        if (before.position_count < JCE_LINE_MAX_POINTS)
            before.positions[before.position_count][0] = NAN;
        jce_scene_set_line_renderer(scene, entity, &before);
        jce_scene_set_line_renderer(scene, reference, &before);
        char json[80];
        snprintf(json, sizeof json, "{\"%s\":-0.75}", keys[i]);
        const char *type = i % 2 ? "lineRenderer" : "LineRenderer";
        LinePatchResult fast = patch_apply_line(scene, entity, type, json, false);
        LinePatchResult generic = patch_apply_line(scene, reference, type, json, true);
        TEST_ASSERT_TRUE(fast.accepted);
        TEST_ASSERT_TRUE(generic.accepted);
        TEST_ASSERT_EQUAL_UINT64(1, fast.fast_paths);
        TEST_ASSERT_EQUAL_UINT64(0, fast.arrays);
        patch_assert_line_equal(&generic.value, &fast.value);
        patch_assert_line_discarded_storage(&fast.value);
        for (int channel = 0; channel < 8; ++channel) {
            const float expected = channel == i ? -0.75f :
                (channel < 4 ? before.color_start[channel] : before.color_end[channel - 4]);
            const float actual = channel < 4 ? fast.value.color_start[channel] :
                                                    fast.value.color_end[channel - 4];
            TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof expected);
        }
        jce_scene_destroy(scene);
    }
}

static void test_line_color_ambiguous_and_bad_patch_uses_generic_semantics(void)
{
    static const char *const patches[] = {
        "{}", "{\"unknown\":7}", "{\"colorStartR\":0.5,\"widthEnd\":0.875}",
        "{\"colorStartR\":0.5,\"colorStartR\":0.75}",
        "{\"colorStartR\":null}", "{\"colorStartR\":true}",
        "{\"colorStartR\":\"0.5\"}", "{\"colorStartR\":[]}",
        "{\"colorStartR\":{}}", "{\"colorStartR\":1e40}",
        "{\"colorStartR\":1e999}", "{\"colorStartR\":"
    };
    for (size_t i = 0; i < sizeof patches / sizeof patches[0]; ++i) {
        JceScene *scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(scene);
        JceEntity entity = jce_scene_create_entity(scene, "fallback-line-color");
        JceEntity reference = jce_scene_create_entity(scene, "fallback-line-reference");
        JceLineRendererComponent before = patch_line_value(14);
        jce_scene_set_line_renderer(scene, entity, &before);
        jce_scene_set_line_renderer(scene, reference, &before);
        LinePatchResult actual = patch_apply_line(scene, entity, "lineRenderer", patches[i], false);
        LinePatchResult generic = patch_apply_line(scene, reference, "lineRenderer", patches[i], true);
        TEST_ASSERT_EQUAL_INT(generic.accepted, actual.accepted);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, actual.fast_paths, patches[i]);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(generic.accepted ? 1 : 0, actual.arrays, patches[i]);
        patch_assert_line_equal(&generic.value, &actual.value);
        if (i == 3) TEST_ASSERT_EQUAL_FLOAT(0.75f, actual.value.color_start[0]);
        if (i >= 4 && i <= 8) TEST_ASSERT_EQUAL_FLOAT(1.0f, actual.value.color_start[0]);
        jce_scene_destroy(scene);
    }
}

static void test_line_noncanonical_live_state_keeps_generic_clamps_and_fallback(void)
{
    for (int i = 0; i < 6; ++i) {
        JceScene *scene = jce_scene_create();
        TEST_ASSERT_NOT_NULL(scene);
        JceEntity entity = jce_scene_create_entity(scene, "noncanonical-line");
        JceEntity reference = jce_scene_create_entity(scene, "noncanonical-reference");
        JceLineRendererComponent before = patch_line_value(14);
        if (i == 0) before.position_count = -3;
        if (i == 1) before.position_count = JCE_LINE_MAX_POINTS + 2;
        if (i == 2) before.width_start = NAN;
        if (i == 3) before.color_end[3] = INFINITY;
        if (i == 4) before.positions[4][1] = NAN;
        if (i == 5) before.positions[4][2] = -INFINITY;
        jce_scene_set_line_renderer(scene, entity, &before);
        jce_scene_set_line_renderer(scene, reference, &before);
        LinePatchResult actual = patch_apply_line(scene, entity, "LineRenderer", "{\"colorStartG\":0.5}", false);
        LinePatchResult generic = patch_apply_line(scene, reference, "LineRenderer", "{\"colorStartG\":0.5}", true);
        TEST_ASSERT_TRUE(actual.accepted);
        TEST_ASSERT_TRUE(generic.accepted);
        TEST_ASSERT_EQUAL_UINT64(0, actual.fast_paths);
        TEST_ASSERT_EQUAL_UINT64(1, actual.arrays);
        patch_assert_line_equal(&generic.value, &actual.value);
        if (i == 0) TEST_ASSERT_EQUAL_INT(0, actual.value.position_count);
        if (i == 1) TEST_ASSERT_EQUAL_INT(JCE_LINE_MAX_POINTS, actual.value.position_count);
        jce_scene_destroy(scene);
    }
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "unterminated-line");
    JceLineRendererComponent before = patch_line_value(14);
    memset(before.material_path, 'x', sizeof before.material_path);
    jce_scene_set_line_renderer(scene, entity, &before);
    cJSON *patch = cJSON_Parse("{\"colorStartG\":0.5}");
    TEST_ASSERT_NOT_NULL(patch);
    /* The legacy serializer requires a C string. Test only the safe admission
     * refusal for this corrupted storage, without invoking its unbounded read. */
    TEST_ASSERT_FALSE(scene_line_try_color_patch(scene, entity, patch));
    patch_assert_line_equal(&before, jce_scene_get_line_renderer(scene, entity));
    cJSON_Delete(patch);
    jce_scene_destroy(scene);
}

static void test_absent_line_patch_retains_registered_parser_defaults(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "absent-line");
    LinePatchResult result = patch_apply_line(scene, entity, "lineRenderer", "{\"colorEndA\":0.25}", false);
    TEST_ASSERT_TRUE(result.accepted);
    TEST_ASSERT_EQUAL_UINT64(0, result.fast_paths);
    TEST_ASSERT_EQUAL_UINT64(0, result.arrays);
    TEST_ASSERT_TRUE(jce_scene_has_line_renderer(scene, entity));
    TEST_ASSERT_EQUAL_INT(0, result.value.position_count);
    TEST_ASSERT_EQUAL_STRING("", result.value.material_path);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, result.value.width_start);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, result.value.width_end);
    TEST_ASSERT_TRUE(result.value.use_world_space);
    TEST_ASSERT_FALSE(result.value.loop);
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(1.0f, result.value.color_start[i]);
        TEST_ASSERT_EQUAL_FLOAT(i == 3 ? 0.25f : 1.0f, result.value.color_end[i]);
    }
    patch_assert_line_discarded_storage(&result.value);
    jce_scene_destroy(scene);
}

/* Both water serializers emit type/properties, while their registered
 * parsers take the property object. Use the real registry and ECS values. */
static void test_nested_water_motion_patch_preserves_properties(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    const JceEntity entity = jce_scene_create_entity(scene, "Nested motion");
    JceWaterMotionComponent before = jce_scene_water_motion_default();
    before.cascade_fraction = 0.41f;
    before.phase_rate = 1.5f;
    before.horizontal_scale = 1.25f;
    before.max_horizontal_displacement = 2.5f;
    jce_scene_set_water_motion(scene, entity, &before);

    const size_t live = patch_json_live;
    TEST_ASSERT_TRUE(patch_apply_component(scene, entity, "WaterMotion",
        "{\"properties\":{\"cascadeFraction\":0.25}}"));
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
    const JceWaterMotionComponent *value = jce_scene_get_water_motion(scene, entity);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, value->cascade_fraction);
    TEST_ASSERT_EQUAL_FLOAT(before.phase_rate, value->phase_rate);
    TEST_ASSERT_EQUAL_FLOAT(before.horizontal_scale, value->horizontal_scale);
    TEST_ASSERT_EQUAL_FLOAT(before.max_horizontal_displacement,
                            value->max_horizontal_displacement);
    jce_scene_destroy(scene);
}

static void test_flat_patch_of_nested_water_spectrum_preserves_properties(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    const JceEntity entity = jce_scene_create_entity(scene, "Nested spectrum");
    JceWaterSpectrumComponent before = jce_scene_water_spectrum_default();
    before.model = JCE_WATER_FFT_SPECTRUM_TMA;
    before.depth = 11.0f;
    before.directional_blend = 0.8f;
    jce_scene_set_water_spectrum(scene, entity, &before);

    const size_t live = patch_json_live;
    TEST_ASSERT_TRUE(patch_apply_component(scene, entity, "WaterSpectrum",
        "{\"shadingFlags\":0}"));
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
    const JceWaterSpectrumComponent *value = jce_scene_get_water_spectrum(scene, entity);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_UINT32(0, value->shading_flags);
    TEST_ASSERT_EQUAL_UINT32(before.model, value->model);
    TEST_ASSERT_EQUAL_FLOAT(before.depth, value->depth);
    TEST_ASSERT_EQUAL_FLOAT(before.directional_blend, value->directional_blend);
    jce_scene_destroy(scene);
}

static void test_nested_patch_adds_absent_water_motion_with_defaults(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    const JceEntity entity = jce_scene_create_entity(scene, "Absent motion");
    TEST_ASSERT_FALSE(jce_scene_has_water_motion(scene, entity));

    const size_t live = patch_json_live;
    TEST_ASSERT_TRUE(patch_apply_component(scene, entity, "WaterMotion",
        "{\"type\":\"WaterMotion\",\"properties\":{\"phaseRate\":2}}"));
    TEST_ASSERT_EQUAL_UINT64(live, patch_json_live);
    const JceWaterMotionComponent *value = jce_scene_get_water_motion(scene, entity);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, value->phase_rate);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, value->cascade_fraction);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, value->horizontal_scale);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, value->max_horizontal_displacement);
    jce_scene_destroy(scene);
}

int main(void)
{
    cJSON_Hooks hooks = { patch_malloc, patch_free };
    cJSON_InitHooks(&hooks);
    UNITY_BEGIN();
    RUN_TEST(test_a_one_key_patch_leaves_the_rest_of_the_component_alone);
    RUN_TEST(test_a_patch_does_not_blank_an_unmentioned_string);
    RUN_TEST(test_comp_set_on_an_absent_component_still_adds_it);
    RUN_TEST(test_a_progress_bar_can_be_driven_through_the_generic_setter);
    RUN_TEST(test_billboard_patch_moves_owned_tree_and_preserves_typed_fields);
    RUN_TEST(test_explicit_component_construction_failures_preserve_ecs);
    RUN_TEST(test_nested_merge_array_failure_is_propagated_and_owned);
    RUN_TEST(test_nested_merge_preserves_objects_and_replaces_arrays);
    RUN_TEST(test_actual_rendering_entry_rejects_failed_merge_before_set);
    RUN_TEST(test_rendering_patch_keeps_celestial_and_rejects_invalid_updates);
    RUN_TEST(test_cloud_shape_patch_keeps_other_fields_and_round_trips);
    RUN_TEST(test_terrain_snow_look_merge_reset_and_validation);
    RUN_TEST(test_equal_scalar_patch_saves_allocations_without_skipping_typed_set);
    RUN_TEST(test_equal_prefix_then_failed_changed_leaf_cannot_reach_typed_set);
    RUN_TEST(test_all_equal_primitives_elide_but_nested_arrays_still_copy);
    RUN_TEST(test_scalar_precision_sign_type_and_string_changes_are_not_elided);
    RUN_TEST(test_line_full_json_roundtrip_uses_shared_color_parser);
    RUN_TEST(test_line_colors_avoid_point_json_and_keep_real_typed_writes);
    RUN_TEST(test_each_partial_line_color_preserves_other_fields_and_alias);
    RUN_TEST(test_line_color_ambiguous_and_bad_patch_uses_generic_semantics);
    RUN_TEST(test_line_noncanonical_live_state_keeps_generic_clamps_and_fallback);
    RUN_TEST(test_absent_line_patch_retains_registered_parser_defaults);
    RUN_TEST(test_nested_water_motion_patch_preserves_properties);
    RUN_TEST(test_flat_patch_of_nested_water_spectrum_preserves_properties);
    RUN_TEST(test_nested_patch_adds_absent_water_motion_with_defaults);
    return UNITY_END();
}
