/* Real scene save/clear/load, real ECS and complete headless Canvas walk.
 * Layout and topmost click consume the same child order as painter submission.
 * No renderer, mocked ECS, texture content or pixel guarantee is involved. */
#include "unity.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/os/core/jce_json.h>
#include <string.h>
#include <stdio.h>

static JceScene *scene;
static JceUICanvas *canvas;
static JceJson *snapshot;
static JceSceneLoadStream *stream;
static JceEntity root;

static JceRectTransform rect(float width, float height)
{
    JceRectTransform r = {0};
    r.anchor_min[1] = r.anchor_max[1] = r.pivot[1] = 1;
    r.size_delta[0] = width; r.size_delta[1] = height;
    return r;
}
static JceEntity add_image(const char *name)
{
    JceEntity e = jce_scene_create_entity(scene, name);
    TEST_ASSERT_NOT_EQUAL(0, e); jce_scene_set_parent(scene, e, root);
    JceUIImageComponent image = {0}; image.rect = rect(40, 30);
    image.color[0] = image.color[1] = image.color[2] = image.color[3] = 1;
    image.raycast_target = true; image.fill_amount = 1;
    jce_scene_set_ui_image(scene, e, &image);
    return e;
}
static void add_button(JceEntity e)
{
    JceUIButtonComponent b = {0}; b.rect = rect(40, 30); b.interactable = true;
    b.normal_color[3] = b.highlighted_color[3] = b.pressed_color[3] = b.disabled_color[3] = 1;
    jce_scene_set_ui_button(scene, e, &b);
}
struct Find { const char *name; JceEntity found; };
static void find_each(JceScene *s, JceEntity e, void *user)
{
    struct Find *find = user;
    const char *name = jce_scene_entity_name(s, e);
    if (name && strcmp(name, find->name) == 0) find->found = e;
}
static JceEntity find(const char *name)
{
    struct Find query = {name, 0}; jce_scene_each_entity(scene, find_each, &query);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, query.found, "Restored named widget missing");
    TEST_ASSERT_TRUE(jce_scene_entity_alive(scene, query.found));
    return query.found;
}
static void render(const JceUIPointer *pointer)
{
    jce_ui_canvas_render(canvas, scene, 0, 0, 320, 200, pointer, .016f);
}
static float x_of(JceEntity e)
{
    float x = 0, y = 0, w = 0, h = 0;
    TEST_ASSERT_TRUE(jce_ui_canvas_entity_rect(canvas, e, &x, &y, &w, &h));
    return x;
}
static void save(void)
{
    if (snapshot) jce_json_free(snapshot);
    snapshot = jce_scene_save_json(scene); TEST_ASSERT_NOT_NULL(snapshot);
}
static void restore(void)
{
    TEST_ASSERT_GREATER_THAN_INT(0, jce_scene_clear(scene));
    TEST_ASSERT_GREATER_THAN_INT(0, jce_scene_load_json(scene, snapshot));
    root = find("Canvas");
}
void setUp(void)
{
    stream = NULL; snapshot = NULL; scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    root = jce_scene_create_entity(scene, "Canvas"); TEST_ASSERT_NOT_EQUAL(0, root);
    JceCanvasComponent c = {0}; c.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(scene, root, &c);
    canvas = jce_ui_canvas_create(NULL, NULL); TEST_ASSERT_NOT_NULL(canvas);
}
void tearDown(void)
{
    if (stream) jce_scene_load_stream_finalize(stream);
    stream = NULL;
    if (snapshot) jce_json_free(snapshot);
    if (canvas) jce_ui_canvas_destroy(canvas);
    if (scene) jce_scene_destroy(scene);
    snapshot = NULL; canvas = NULL; scene = NULL; root = 0;
}
static void test_restored_layout_keeps_named_order_after_runtime_table_changes(void)
{
    JceLayoutGroupComponent group = {0}; group.layout_kind = JCE_LAYOUT_HORIZONTAL;
    jce_scene_set_layout_group(scene, root, &group);
    JceEntity a = add_image("A"), b = add_image("B"), c = add_image("C");
    render(NULL);
    TEST_ASSERT_FLOAT_WITHIN(.01f, 0, x_of(a));
    TEST_ASSERT_FLOAT_WITHIN(.01f, 40, x_of(b));
    TEST_ASSERT_FLOAT_WITHIN(.01f, 80, x_of(c));
    save(); /* The pre-Play scene, before runtime component/table mutation. */
    for (int cycle = 0; cycle < 6; ++cycle) {
        const JceEntity prior = find("B");
        add_button(prior); /* Runtime migration must not become authored order. */
        JceEntity spawned = jce_scene_create_entity(scene, "RuntimeActor");
        TEST_ASSERT_NOT_EQUAL(0, spawned); jce_scene_destroy_entity(scene, spawned);
        restore();
        TEST_ASSERT_FALSE(jce_scene_entity_alive(scene, prior));
        render(NULL);
        printf("restore-layout cycle=%d A=%llu/x=%.2f B=%llu/x=%.2f C=%llu/x=%.2f\n",
            cycle, (unsigned long long)find("A"), x_of(find("A")),
            (unsigned long long)find("B"), x_of(find("B")),
            (unsigned long long)find("C"), x_of(find("C")));
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(.01f, 0, x_of(find("A")), "A moved after scene restoration");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(.01f, 40, x_of(find("B")), "B moved after scene restoration");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(.01f, 80, x_of(find("C")), "C moved after scene restoration");
    }
}
static void test_restored_overlapping_buttons_keep_topmost_click_owner(void)
{
    JceEntity bottom = add_image("Bottom"), top = add_image("Top");
    add_button(bottom); add_button(top); save();
    for (int cycle = 0; cycle < 6; ++cycle) {
        JceUIPointer pointer = {10, 10, false, true};
        render(&pointer); pointer.down = true; render(&pointer);
        pointer.down = false; render(&pointer);
        printf("restore-hit cycle=%d Bottom=%llu Top=%llu clicked=%llu\n", cycle,
            (unsigned long long)find("Bottom"), (unsigned long long)find("Top"),
            (unsigned long long)jce_ui_canvas_last_clicked(canvas));
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(find("Top"), jce_ui_canvas_last_clicked(canvas),
            "Opaque lower widget displaced the authored top widget after restore");
        restore();
    }
}
static void expect_saved_names(const JceJson *document, const char *const *names, int count)
{
    const JceJson *entities = jce_json_get(jce_json_get(document, "scene"), "entities");
    TEST_ASSERT_EQUAL_INT(count, jce_json_array_size(entities));
    for (int i = 0; i < count; ++i)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(names[i],
            jce_json_get_string(jce_json_array_at(entities, i), "name", ""),
            "Canonical scene array changed authored order");
}
static void test_canonical_save_and_streamed_restore_keep_authored_array_order(void)
{
    static const char *const names[] = {"Canvas", "A", "B", "C"};
    JceLayoutGroupComponent group = {0}; group.layout_kind = JCE_LAYOUT_HORIZONTAL;
    jce_scene_set_layout_group(scene, root, &group);
    add_image("A"); JceEntity b = add_image("B"); add_image("C");
    add_button(b); /* Flecs table movement precedes canonical save. */
    save(); expect_saved_names(snapshot, names, 4);
    for (int cycle = 0; cycle < 6; ++cycle) {
        TEST_ASSERT_GREATER_THAN_INT(0, jce_scene_clear(scene));
        int total = 0;
        stream = jce_scene_load_stream_begin(scene, snapshot, &total);
        TEST_ASSERT_NOT_NULL(stream); TEST_ASSERT_EQUAL_INT(4, total);
        for (int step = 1; step <= total; ++step)
            TEST_ASSERT_EQUAL_INT(step, jce_scene_load_stream_step(stream, 1));
        TEST_ASSERT_TRUE(jce_scene_load_stream_done(stream));
        const int loaded = jce_scene_load_stream_finalize(stream); stream = NULL;
        TEST_ASSERT_EQUAL_INT(total, loaded); root = find("Canvas");
        render(NULL);
        TEST_ASSERT_FLOAT_WITHIN(.01f, 0, x_of(find("A")));
        TEST_ASSERT_FLOAT_WITHIN(.01f, 40, x_of(find("B")));
        TEST_ASSERT_FLOAT_WITHIN(.01f, 80, x_of(find("C")));
        save(); expect_saved_names(snapshot, names, 4);
    }
}
static void test_subtree_save_preserves_siblings_after_reparent_and_table_migration(void)
{
    static const char *const names[] = {"Canvas", "A", "B", "C"};
    add_image("A"); JceEntity b = add_image("B"); add_image("C");
    for (int cycle = 0; cycle < 6; ++cycle) {
        add_button(b);
        TEST_ASSERT_TRUE(jce_scene_reparent(scene, b, 0, false));
        TEST_ASSERT_TRUE(jce_scene_reparent(scene, b, root, false));
        snapshot = jce_scene_save_subtree_json(scene, root);
        TEST_ASSERT_NOT_NULL(snapshot); expect_saved_names(snapshot, names, 4);
        restore(); b = find("B");
        jce_json_free(snapshot); snapshot = NULL;
    }
}
static void test_equal_canvas_priorities_keep_authored_topmost_owner_after_restore(void)
{
    JceEntity bottom = add_image("Bottom"); add_button(bottom);
    root = jce_scene_create_entity(scene, "CanvasTop"); TEST_ASSERT_NOT_EQUAL(0, root);
    JceCanvasComponent c = {0}; c.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(scene, root, &c);
    JceEntity top = add_image("Top"); add_button(top); save();
    for (int cycle = 0; cycle < 6; ++cycle) {
        JceUIPointer pointer = {10, 10, false, true};
        render(&pointer); pointer.down = true; render(&pointer);
        pointer.down = false; render(&pointer);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(find("Top"), jce_ui_canvas_last_clicked(canvas),
            "Equal-priority canvases lost authored order after restore");
        restore();
    }
}
static void test_legacy_array_order_is_independent_of_authored_ids_and_recycled_slots(void)
{
    static const char *const names[] = {"Canvas", "A", "B", "C"};
    snapshot = jce_json_parse("{\"entities\":[{\"id\":30,\"name\":\"Canvas\"},"
        "{\"id\":900,\"name\":\"A\",\"parentId\":30},"
        "{\"id\":2,\"name\":\"B\",\"parentId\":30},"
        "{\"id\":40,\"name\":\"C\",\"parentId\":30}]}", 0);
    TEST_ASSERT_NOT_NULL(snapshot); restore();
    for (int cycle = 0; cycle < 6; ++cycle) {
        JceEntity old_b = find("B");
        add_button(old_b); save(); expect_saved_names(snapshot, names, 4);
        restore(); TEST_ASSERT_FALSE(jce_scene_entity_alive(scene, old_b));
        TEST_ASSERT_TRUE(old_b != find("B")); /* Preserve full 64-bit generations. */
    }
    const JceEntity stale_root = root;
    jce_scene_destroy_entity(scene, root);
    jce_json_free(snapshot); snapshot = jce_scene_save_subtree_json(scene, stale_root);
    TEST_ASSERT_NULL_MESSAGE(snapshot, "Nonzero stale subtree root must be rejected");
    snapshot = jce_scene_save_subtree_json(scene, 0);
    TEST_ASSERT_NOT_NULL(snapshot); /* Historical root-zero empty envelope. */
    const JceJson *empty = jce_json_get(jce_json_get(snapshot, "scene"), "entities");
    TEST_ASSERT_EQUAL_INT(0, jce_json_array_size(empty));
}
static void test_wide_canonical_and_subtree_snapshot_preserve_all_authored_rows_after_migration(void)
{
    enum { WIDGETS = 130 }; /* Exercises the ordered snapshot's heap branch. */
    char name[32];
    for (int i = 0; i < WIDGETS; ++i) {
        snprintf(name, sizeof(name), "Widget%03d", i);
        JceEntity e = add_image(name);
        if (i % 2 == 0) add_button(e); /* Distinct component tables interleave. */
    }
    for (int cycle = 0; cycle < 6; ++cycle) {
        if (cycle % 2 == 0) save();
        else {
            jce_json_free(snapshot);
            snapshot = jce_scene_save_subtree_json(scene, root);
            TEST_ASSERT_NOT_NULL(snapshot);
        }
        const JceJson *entities = jce_json_get(jce_json_get(snapshot, "scene"), "entities");
        TEST_ASSERT_EQUAL_INT(WIDGETS + 1, jce_json_array_size(entities));
        TEST_ASSERT_EQUAL_STRING("Canvas", jce_json_get_string(jce_json_array_at(entities, 0), "name", ""));
        for (int i = 0; i < WIDGETS; ++i) {
            snprintf(name, sizeof(name), "Widget%03d", i);
            TEST_ASSERT_EQUAL_STRING(name,
                jce_json_get_string(jce_json_array_at(entities, i + 1), "name", ""));
        }
        restore();
        TEST_ASSERT_NOT_EQUAL(0, find("Widget129"));
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_restored_layout_keeps_named_order_after_runtime_table_changes);
    RUN_TEST(test_restored_overlapping_buttons_keep_topmost_click_owner);
    RUN_TEST(test_canonical_save_and_streamed_restore_keep_authored_array_order);
    RUN_TEST(test_subtree_save_preserves_siblings_after_reparent_and_table_migration);
    RUN_TEST(test_equal_canvas_priorities_keep_authored_topmost_owner_after_restore);
    RUN_TEST(test_legacy_array_order_is_independent_of_authored_ids_and_recycled_slots);
    RUN_TEST(test_wide_canonical_and_subtree_snapshot_preserve_all_authored_rows_after_migration);
    return UNITY_END();
}
