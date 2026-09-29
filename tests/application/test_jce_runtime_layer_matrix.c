/* test_jce_runtime_layer_matrix.c
 *
 * Top 4 (physics layer collision matrix at ship): the matrix loaded from JSON
 * BEFORE jce_runtime_create — the exact path the shipped drop-in main app_init
 * and the build-export now use — actually FILTERS runtime collisions per body.
 *
 * (1) JSON round-trip: set a non-colliding pair, save, wipe, reload -> still
 *     non-colliding.  Proves jce_physics_layer_matrix_save/load_json, which the
 *     build-export (jce_build_manager) and app_init (jce_default_main) rely on.
 * (2) End-to-end: two overlapping dynamic boxes on layers 1 & 2.  Matrix loaded
 *     NON-colliding -> they stay overlapped; all-colliding -> pushed apart.
 *     Proves the runtime applies the loaded matrix per body (jce_runtime.c:1977
 *     jce_physics_body_set_layer using rb->physics_layer).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/resource/jce_scene_serial.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LAYER_JSON "test_layer_matrix.json"

/* Two unit boxes overlapping in X (0.0 and 0.3), floating (useGravity=false),
 * dynamic, on physics layers 1 and 2. */
static const char *SCENE_JSON =
"{\"scene\":{\"entities\":["
 "{\"components\":["
  "{\"type\":\"Transform\",\"posX\":0.0,\"posY\":5.0,\"posZ\":0.0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
  "{\"type\":\"BoxCollider\",\"sizeX\":1.0,\"sizeY\":1.0,\"sizeZ\":1.0},"
  "{\"type\":\"Rigidbody\",\"mass\":1.0,\"useGravity\":false,\"isKinematic\":false,\"physicsLayer\":1}"
 "]},"
 "{\"components\":["
  "{\"type\":\"Transform\",\"posX\":0.3,\"posY\":5.0,\"posZ\":0.0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
  "{\"type\":\"BoxCollider\",\"sizeX\":1.0,\"sizeY\":1.0,\"sizeZ\":1.0},"
  "{\"type\":\"Rigidbody\",\"mass\":1.0,\"useGravity\":false,\"isKinematic\":false,\"physicsLayer\":2}"
 "]}"
"]}}";

void setUp(void)    { jce_physics_layer_matrix_reset_default(); }
void tearDown(void) { remove(LAYER_JSON); jce_physics_layer_matrix_reset_default(); }

static void test_matrix_json_roundtrip(void)
{
    jce_physics_layer_matrix_reset_default();
    TEST_ASSERT_TRUE(jce_physics_get_layer_collides(1, 2));    /* default: all collide */

    jce_physics_set_layer_collides(1, 2, false);
    TEST_ASSERT_TRUE(jce_physics_layer_matrix_save_json(LAYER_JSON));

    jce_physics_layer_matrix_reset_default();                  /* wipe back to all-collide */
    TEST_ASSERT_TRUE(jce_physics_get_layer_collides(1, 2));

    TEST_ASSERT_TRUE(jce_physics_layer_matrix_load_json(LAYER_JSON));
    TEST_ASSERT_FALSE(jce_physics_get_layer_collides(1, 2));   /* restored from disk */
    TEST_ASSERT_TRUE(jce_physics_get_layer_collides(0, 1));    /* untouched pairs stay */
}

static float run_separation(bool layers_collide)
{
    jce_physics_layer_matrix_reset_default();
    jce_physics_set_layer_collides(1, 2, layers_collide);     /* set BEFORE runtime create */

    JceScene *sc = jce_scene_create();
    JceEntity *ents = NULL; uint32_t n = 0;
    bool ok = jce_scene_serial_load_additive(sc, SCENE_JSON, strlen(SCENE_JSON), &ents, &n);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(2u, n);
    JceEntity a = ents[0], b = ents[1];
    jce_scene_serial_free_entities(ents);

    JceRuntimeDesc rd; memset(&rd, 0, sizeof rd);
    rd.scene = sc; rd.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);
    for (int i = 0; i < 120; ++i) jce_runtime_step(rt, 1.0f / 60.0f);

    JceTransform *ta = jce_scene_get_transform(sc, a);
    JceTransform *tb = jce_scene_get_transform(sc, b);
    float sep = (ta && tb) ? fabsf(ta->position.x - tb->position.x) : -1.0f;

    jce_runtime_destroy(rt);
    jce_scene_destroy(sc);
    return sep;
}

static void test_runtime_layer_filtering(void)
{
    float sep_off = run_separation(false);   /* non-colliding -> remain overlapped */
    float sep_on  = run_separation(true);    /* colliding     -> pushed apart */
    printf("[layer] sep no-collide=%.3f  collide=%.3f\n", sep_off, sep_on);
    TEST_ASSERT_TRUE_MESSAGE(sep_off < 0.5f, "non-colliding layers must NOT separate (stay ~0.3 apart)");
    TEST_ASSERT_TRUE_MESSAGE(sep_on  > 0.7f, "colliding layers must push apart (> box size)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matrix_json_roundtrip);
    RUN_TEST(test_runtime_layer_filtering);
    return UNITY_END();
}
