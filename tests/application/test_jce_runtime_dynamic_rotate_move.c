/* test_jce_runtime_dynamic_rotate_move.c
 *
 * Regression for the enemy-AI "locks on but won't chase" bug: a DYNAMIC body
 * driven by jce.set_velocity while a gameplay script also calls jce.set_rotation
 * EVERY frame (to face a target) must still MOVE.
 *
 * Before the fix, rt_push_external_transforms teleported the body on every
 * rotation change, yanking it back to its lagging interpolated Transform
 * position and discarding the velocity-driven motion — so the body barely moved.
 * The fix makes a rotation-only edit on a dynamic body update rotation in place
 * (keeping the live physics position).  This drives the real script path
 * (Script component -> Lua on_update -> set_velocity + set_rotation).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_serial.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LUA_PATH "rt_rotmove_test.lua"

/* Mirrors enemy_ai.lua's chase motion: face a changing yaw every frame AND
 * drive horizontal velocity.  Moves toward +X. */
static const char *LUA_SRC =
    "local M = {}\n"
    "function M:on_start() self.t = 0.0 end\n"
    "function M:on_update(dt)\n"
    "  self.t = self.t + dt\n"
    "  jce.set_rotation(self.entity, 0.0, self.t * 60.0, 0.0)\n"  /* face changing yaw every frame */
    "  local _, vy, _ = jce.get_velocity(self.entity)\n"
    "  jce.set_velocity(self.entity, 2.0, vy or 0.0, 0.0)\n"      /* chase toward +X */
    "end\n"
    "return M\n";

/* A ground plane + a dynamic box resting on it (gravity ON, freeze_rotation),
 * matching the demo enemies (on the ground, kept active by contacts — NOT a
 * contactless floating body that Bullet would sleep).  The mover carries the
 * Script above (per-frame set_velocity + set_rotation). */
static const char *SCENE_JSON =
    "{\"scene\":{\"entities\":["
    "{\"components\":["
    "{\"type\":\"Transform\",\"posX\":0,\"posY\":-0.5,\"posZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
    "{\"type\":\"BoxCollider\",\"sizeX\":60,\"sizeY\":1,\"sizeZ\":60},"
    "{\"type\":\"Rigidbody\",\"mass\":0.0,\"useGravity\":false,\"isKinematic\":false},"
    "{\"type\":\"EditorMeta\",\"name\":\"Ground\"}"
    "]},"
    "{\"components\":["
    "{\"type\":\"Transform\",\"posX\":0,\"posY\":2,\"posZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
    "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1},"
    "{\"type\":\"Rigidbody\",\"mass\":1.0,\"useGravity\":true,\"isKinematic\":false,\"freezeRotation\":true},"
    "{\"type\":\"Script\",\"scriptPath\":\"" LUA_PATH "\"},"
    "{\"type\":\"EditorMeta\",\"name\":\"Mover\"}"
    "]}"
    "]}}";

void setUp(void)    {}
void tearDown(void) { remove(LUA_PATH); }

static void test_dynamic_body_chases_while_facing(void)
{
    FILE *f = fopen(LUA_PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(LUA_SRC, 1, strlen(LUA_SRC), f);
    fclose(f);

    JceScene *sc = jce_scene_create();
    JceEntity *ents = NULL; uint32_t n = 0;
    TEST_ASSERT_TRUE(jce_scene_serial_load_additive(sc, SCENE_JSON, strlen(SCENE_JSON), &ents, &n));
    TEST_ASSERT_EQUAL_UINT32(2u, n);
    JceEntity e = ents[1];   /* [0] = Ground, [1] = Mover */
    jce_scene_serial_free_entities(ents);

    JceRuntimeDesc rd; memset(&rd, 0, sizeof rd);
    rd.scene = sc; rd.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    JceTransform *t0 = jce_scene_get_transform(sc, e);
    float x_start = t0 ? t0->position.x : 0.0f;

    for (int i = 0; i < 120; ++i) jce_runtime_step(rt, 1.0f / 60.0f);  /* 2s */

    JceTransform *t1 = jce_scene_get_transform(sc, e);
    float x_end = t1 ? t1->position.x : 0.0f;
    float moved = x_end - x_start;
    printf("[rotmove] x_start=%.3f x_end=%.3f moved=%.3f\n", x_start, x_end, moved);

    jce_runtime_destroy(rt);
    jce_scene_destroy(sc);

    /* +X velocity 2 u/s for 2s ~= 4 units; pre-fix the rotation-teleport
     * discarded it (moved ~0).  Require clear forward progress. */
    TEST_ASSERT_TRUE_MESSAGE(moved > 1.5f,
        "dynamic body driven by set_velocity must MOVE even while set_rotation "
        "faces a target every frame (chase regression)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dynamic_body_chases_while_facing);
    return UNITY_END();
}
