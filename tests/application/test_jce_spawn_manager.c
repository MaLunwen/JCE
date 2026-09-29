/* test_jce_spawn_manager.c
 *
 * SpawnManager P0: a scene SpawnManager with a ped_prefab_path actually
 * INSTANTIATES ped prefabs at runtime (previously inert — on_create just bumped
 * a counter).  Writes a minimal prefab fixture, runs a SpawnManager with that
 * prefab + a small radius/interval, steps the runtime, and asserts the scene
 * entity count grew (peds spawned around the default (0,0,0) viewer).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

#define PREFAB_PATH "rt_spawn_ped_test.prefab.json"
static const char *PREFAB_JSON =
    "{\"scene\":{\"entities\":[{\"components\":["
    "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
    "{\"type\":\"EditorMeta\",\"name\":\"Ped\"}"
    "]}]}}";

void setUp(void)    {}
void tearDown(void) { remove(PREFAB_PATH); }

static int g_count;
static void count_cb(JceScene *s, JceEntity e, void *ud) { (void)s; (void)e; (void)ud; g_count++; }
static int scene_entity_count(JceScene *s) { g_count = 0; jce_scene_each_entity(s, count_cb, NULL); return g_count; }

static void write_prefab(void)
{
    FILE *f = fopen(PREFAB_PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(PREFAB_JSON, 1, strlen(PREFAB_JSON), f);
    fclose(f);
}

static void test_spawnmanager_instantiates_peds(void)
{
    write_prefab();

    JceScene *sc = jce_scene_create();
    TEST_ASSERT_NOT_NULL(sc);
    JceEntity mgr = jce_scene_create_entity(sc, "Spawner");

    JceSpawnManagerComponent m;
    memset(&m, 0, sizeof m);
    m.enabled          = 1;
    m.max_peds         = 2;
    m.min_spawn_radius = 1.0f;
    m.max_spawn_radius = 5.0f;
    m.spawn_interval   = 0.01f;
    snprintf(m.ped_prefab_path, sizeof m.ped_prefab_path, "%s", PREFAB_PATH);
    jce_scene_set_spawn_manager(sc, mgr, &m);

    int before = scene_entity_count(sc);

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene          = sc;
    rd.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    for (int i = 0; i < 60; ++i) jce_runtime_step(rt, 0.1f);  /* ~6s: spawn the peds */

    int after = scene_entity_count(sc);
    printf("[spawnmgr] entities before=%d after=%d (max_peds=2)\n", before, after);

    jce_runtime_destroy(rt);
    jce_scene_destroy(sc);

    TEST_ASSERT_TRUE_MESSAGE(after > before,
        "SpawnManager must instantiate ped prefabs at runtime (was inert)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spawnmanager_instantiates_peds);
    return UNITY_END();
}
