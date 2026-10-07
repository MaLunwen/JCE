#include <jce/api_scene.h>
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define ELEMENTS 1000u
#define WIDTH 1280.0f
#define HEIGHT 720.0f

static JceEntity make_canvas(JceScene *scene)
{
    JceEntity entity = jce_scene_create_entity(scene, "CapacityCanvas");
    JceCanvasComponent canvas = {0};
    jce_scene_set_canvas(scene, entity, &canvas);
    return entity;
}

static JceEntity make_element(JceScene *scene, JceEntity root, unsigned index, bool button)
{
    JceEntity entity = jce_scene_create_entity(scene, "CapacityElement");
    JceUIImageComponent image = {0};
    image.color[0] = image.color[1] = image.color[2] = image.color[3] = 1;
    image.raycast_target = true;
    image.rect.anchor_min[1] = image.rect.anchor_max[1] = 1;
    image.rect.pivot[1] = 1;
    image.rect.anchored_position[0] = 10.0f + (index % 100u) * 12.0f;
    image.rect.anchored_position[1] = -(10.0f + (index / 100u) * 6.0f);
    image.rect.size_delta[0] = 8;
    image.rect.size_delta[1] = 4;
    jce_scene_set_parent(scene, entity, root);
    jce_scene_set_ui_image(scene, entity, &image);
    if (button) {
        JceUIButtonComponent component = {0};
        component.interactable = true;
        jce_scene_set_ui_button(scene, entity, &component);
    }
    return entity;
}

static void render(JceUICanvas *canvas, JceScene *scene, const JceUIPointer *pointer)
{
    jce_ui_canvas_render(canvas, scene, 0, UINT16_MAX, WIDTH, HEIGHT, pointer, 1.0f / 60.0f);
}

static void test_every_record_is_pickable_past_256(void)
{
    JceScene *scene = jce_scene_create();
    JceUICanvas *canvas = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(canvas);
    JceEntity root = make_canvas(scene);
    JceEntity entities[ELEMENTS];
    for (unsigned i = 0; i < ELEMENTS; ++i) entities[i] = make_element(scene, root, i, false);
    render(canvas, scene, NULL);
    for (unsigned i = 0; i < ELEMENTS; ++i) {
        float x, y, w, h;
        TEST_ASSERT_TRUE(jce_ui_canvas_entity_rect(canvas, entities[i], &x, &y, &w, &h));
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 8, w);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, 4, h);
        TEST_ASSERT_EQUAL_UINT64(entities[i], jce_ui_canvas_pick(canvas, x + 2, y + 2));
    }
    jce_scene_destroy_entity(scene, root);
    render(canvas, scene, NULL);
    TEST_ASSERT_EQUAL_UINT64(0, jce_ui_canvas_pick(canvas, 12, 12));
    jce_ui_canvas_destroy(canvas);
    jce_scene_destroy(scene);
}

static void test_last_button_state_survives_growth_and_deletion(void)
{
    JceScene *scene = jce_scene_create();
    JceUICanvas *canvas = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(canvas);
    JceEntity root = make_canvas(scene);
    JceEntity entities[ELEMENTS];
    for (unsigned i = 0; i < ELEMENTS; ++i) entities[i] = make_element(scene, root, i, true);
    render(canvas, scene, NULL);
    JceUIPointer pointer = {1200, 66, true, true}; /* Center of element 999. */
    render(canvas, scene, &pointer);
    /* Remove many hashed records while the final button keeps its press. */
    for (unsigned i = 0; i < ELEMENTS / 2u; ++i) jce_scene_destroy_entity(scene, entities[i]);
    pointer.down = false;
    render(canvas, scene, &pointer);
    TEST_ASSERT_EQUAL_UINT64(entities[ELEMENTS - 1u], jce_ui_canvas_last_clicked(canvas));
    pointer.down = true;
    render(canvas, scene, &pointer);
    jce_scene_destroy_entity(scene, entities[ELEMENTS - 1u]);
    JceEntity replacement = make_element(scene, root, ELEMENTS - 1u, true);
    pointer.down = false;
    render(canvas, scene, &pointer);
    TEST_ASSERT_EQUAL_UINT64(0, jce_ui_canvas_last_clicked(canvas));
    TEST_ASSERT_EQUAL_UINT64(replacement, jce_ui_canvas_pick(canvas, pointer.x, pointer.y));
    pointer.down = true;
    render(canvas, scene, &pointer);
    pointer.down = false;
    render(canvas, scene, &pointer);
    TEST_ASSERT_EQUAL_UINT64(replacement, jce_ui_canvas_last_clicked(canvas));
    jce_ui_canvas_destroy(canvas);
    jce_scene_destroy(scene);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_record_is_pickable_past_256);
    RUN_TEST(test_last_button_state_survives_growth_and_deletion);
    return UNITY_END();
}
