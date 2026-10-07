/* Real runtime/Lua services, borrowed lifetime and persisted backend restart. */
#include <jce/application/jce_runtime_stats_host.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_platform_services.h>
#include <jce/os/core/jce_filesystem.h>
#include "jce_test_file_util.h"
#include "unity.h"
#include <string.h>
#include <limits.h>
#include <stdio.h>
#define LUA_FILE "test_runtime_services.lua"
#define STORE_DIR "_ut_runtime_services"
void setUp(void) { jce_fs_host_remove_recursive(STORE_DIR); }
void tearDown(void) { remove(LUA_FILE); jce_fs_host_remove_recursive(STORE_DIR); }
static JceRuntime *create_probe(JceScene **scene, JceEntity *entity)
{
    jce_test_write_file(LUA_FILE,
        "local M={}\nfunction M:on_update(dt)\n"
        "local available=jce.stat_available()\n"
        "local old=jce.stat_get('score',-123)\n"
        "assert(not jce.stat_set('bad',2147483648))\n"
        "assert(not jce.stat_set('bad',-2147483649))\n"
        "assert(not jce.stat_set('bad',0/0))\n"
        "assert(not jce.stat_set('bad',1/0))\n"
        "assert(not jce.stat_set('bad',1.25))\n"
        "assert(jce.stat_get('bad',12)==12)\n"
        "assert(jce.stat_get('bad',1/0)==0)\n"
        "assert(jce.stat_get('bad',0/0)==0)\n"
        "assert(jce.stat_get('bad',2147483648)==0)\n"
        "assert(jce.stat_get('bad',1.25)==0)\n"
        "assert(not jce.stat_set('',1))\n"
        "assert(not jce.stat_set(string.rep('x',96),1))\n"
        "assert(jce.stat_get('',7)==7)\n"
        "local saved=jce.stat_set('score',-2147483648)\n"
        "local flushed=jce.stat_flush()\n"
        "local requested=jce.request_quit()\n"
        "jce.set_position(self.entity,available and 1 or 0,saved and 1 or 0,old)\n"
        "jce.set_scale(self.entity,flushed and 1 or 0,requested and 1 or 0,9)\n"
        "end\nreturn M\n");
    *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(*scene);
    *entity = jce_scene_create_entity(*scene, "probe");
    JceScriptComponent script = {0};
    snprintf(script.script_path, sizeof(script.script_path), "%s", LUA_FILE);
    jce_scene_set_script(*scene, *entity, &script);
    jce_scene_set_component_enabled(*scene, *entity, JCE_COMP_FLAG_SCRIPT, true);
    JceRuntimeDesc desc = {0};
    desc.scene = *scene;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_FALSE(jce_runtime_quit_requested(rt));
    return rt;
}
static void test_no_services_and_quit_is_deferred_request(void)
{
    JceScene *scene;
    JceEntity entity;
    JceRuntime *rt = create_probe(&scene, &entity);
    TEST_ASSERT_FALSE(jce_runtime_quit_requested(NULL));
    jce_runtime_set_platform_services(NULL, NULL);
    jce_runtime_step(rt, 0.016f);
    const JceTransform *t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(0, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(0, t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(-123, t->position.z);
    TEST_ASSERT_EQUAL_FLOAT(0, t->scale.x);
    TEST_ASSERT_EQUAL_FLOAT(1, t->scale.y);
    /* The statement AFTER request_quit ran; the VM was not destroyed inside it. */
    TEST_ASSERT_EQUAL_FLOAT(9, t->scale.z);
    TEST_ASSERT_TRUE(jce_runtime_quit_requested(rt));
    jce_runtime_destroy(rt);
    jce_scene_destroy(scene);
    rt = create_probe(&scene, &entity);
    TEST_ASSERT_FALSE(jce_runtime_quit_requested(rt));
    jce_runtime_destroy(rt);
    jce_scene_destroy(scene);
}
static void test_borrowed_stats_persist_and_detach(void)
{
    JcePlatformServices *svc = jce_platform_init_local(STORE_DIR);
    TEST_ASSERT_NOT_NULL(svc);
    TEST_ASSERT_TRUE(jce_platform_stats_available(svc));
    TEST_ASSERT_TRUE(jce_platform_stat_set(svc, "score", 37));
    JceScene *scene;
    JceEntity entity;
    JceRuntime *rt = create_probe(&scene, &entity);
    jce_runtime_set_platform_services(rt, svc);
    jce_runtime_step(rt, 0.016f);
    const JceTransform *t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(1, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(1, t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(37, t->position.z);
    TEST_ASSERT_EQUAL_FLOAT(1, t->scale.x);
    TEST_ASSERT_EQUAL_INT64(INT_MIN, jce_platform_stat_get(svc, "score", 0));
    jce_runtime_set_platform_services(rt, NULL);
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT(-123, t->position.z);
    jce_runtime_destroy(rt);
    jce_scene_destroy(scene);
    /* Runtime does not flush/destroy borrowed service; it remains usable. */
    TEST_ASSERT_TRUE(jce_platform_stat_set(svc, "after_destroy", 42));
    TEST_ASSERT_TRUE(jce_platform_flush(svc));
    jce_platform_shutdown(svc);
    svc = jce_platform_init_local(STORE_DIR);
    TEST_ASSERT_NOT_NULL(svc);
    TEST_ASSERT_EQUAL_INT64(INT_MIN, jce_platform_stat_get(svc, "score", 0));
    TEST_ASSERT_EQUAL_INT64(42, jce_platform_stat_get(svc, "after_destroy", 0));
    TEST_ASSERT_TRUE(jce_platform_stat_set(svc, "score", INT64_MAX));
    rt = create_probe(&scene, &entity);
    jce_runtime_set_platform_services(rt, svc);
    jce_runtime_step(rt, 0.016f);
    t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(-123, t->position.z);
    jce_runtime_destroy(rt);
    jce_scene_destroy(scene);
    jce_platform_shutdown(svc);
}
static void test_trusted_host_store_controls_and_restart(void)
{
    char base[1024], directory[1024], other[1024], too_long[1024];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(base, sizeof(base)));
    TEST_ASSERT_TRUE(jce_path_join(directory, sizeof(directory), base, STORE_DIR));
    TEST_ASSERT_TRUE(jce_path_join(other, sizeof(other), directory, "other"));
    JceScene *scene;
    JceEntity entity;
    JceRuntimeStatsHost host = {0};
    host.runtime = create_probe(&scene, &entity);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, NULL));
    TEST_ASSERT_NULL(host.stats);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, ""));
    TEST_ASSERT_NULL(host.stats);
    TEST_ASSERT_FALSE(jce_runtime_stats_host_attach(&host, "relative_stats"));
    memset(too_long, 'x', sizeof(too_long));
    memcpy(too_long, base, strlen(base));
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_FALSE(jce_runtime_stats_host_attach(&host, too_long));
    TEST_ASSERT_NULL(host.stats);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    jce_runtime_set_platform_services(host.runtime, host.stats);
    JcePlatformServices *same = host.stats;
    TEST_ASSERT_NOT_NULL(same);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    TEST_ASSERT_EQUAL_PTR(same, host.stats);
    TEST_ASSERT_FALSE(jce_runtime_stats_host_attach(&host, other));
    TEST_ASSERT_EQUAL_PTR(same, host.stats);
    TEST_ASSERT_FALSE(jce_runtime_stats_host_close(&host));
    TEST_ASSERT_EQUAL_PTR(same, host.stats);
    TEST_ASSERT_TRUE(jce_platform_stat_set(same, "score", 51));
    jce_runtime_step(host.runtime, 0.016f);
    const JceTransform *t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(1, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(51, t->position.z);
    jce_runtime_destroy(host.runtime);
    host.runtime = NULL;
    jce_scene_destroy(scene);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
    TEST_ASSERT_NULL(host.stats);
    TEST_ASSERT_EQUAL_STRING("", host.configured_dir);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
    host.runtime = create_probe(&scene, &entity);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    jce_runtime_set_platform_services(host.runtime, host.stats);
    jce_runtime_step(host.runtime, 0.016f);
    t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT((float)INT32_MIN, t->position.z);
    jce_runtime_destroy(host.runtime);
    host.runtime = NULL;
    jce_scene_destroy(scene);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
}

static JceRuntime *create_start_probe(JceScene **scene, JceEntity *entity,
                                     JcePlatformServices *svc, bool legacy)
{
    jce_test_write_file(LUA_FILE,
        "local M={}\nfunction M:on_start()\n"
        "local available=jce.stat_available()\n"
        "local old=jce.stat_get('start_score',-7)\n"
        "local saved=jce.stat_set('start_score',81)\n"
        "local flushed=jce.stat_flush()\n"
        "jce.set_position(self.entity,available and 1 or 0,old,saved and 1 or 0)\n"
        "jce.set_scale(self.entity,flushed and 1 or 0,2,9)\n"
        "end\nreturn M\n");
    *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(*scene);
    *entity = jce_scene_create_entity(*scene, "start_probe");
    JceScriptComponent script = {0};
    snprintf(script.script_path, sizeof(script.script_path), "%s", LUA_FILE);
    jce_scene_set_script(*scene, *entity, &script);
    jce_scene_set_component_enabled(*scene, *entity, JCE_COMP_FLAG_SCRIPT, true);
    JceRuntimeDesc desc = {0};
    desc.scene = *scene;
    JceRuntime *rt = legacy ? jce_runtime_create(&desc)
        : jce_runtime_create_with_platform_services(&desc, svc);
    TEST_ASSERT_NOT_NULL(rt);
    return rt;
}
static void test_service_is_visible_during_on_start_and_legacy_is_unavailable(void)
{
    char base[1024], directory[1024];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(base, sizeof(base)));
    TEST_ASSERT_TRUE(jce_path_join(directory, sizeof(directory), base, STORE_DIR));
    JceRuntimeStatsHost host = {0};
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    TEST_ASSERT_NOT_NULL(host.stats);
    TEST_ASSERT_TRUE(jce_platform_stat_set(host.stats, "start_score", 19));
    JceScene *scene;
    JceEntity entity;
    host.runtime = create_start_probe(&scene, &entity, host.stats, false);
    /* NO runtime_step or post-create setter: constructor on_start is authority. */
    const JceTransform *t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(1, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(19, t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(1, t->position.z);
    TEST_ASSERT_EQUAL_FLOAT(1, t->scale.x);
    TEST_ASSERT_EQUAL_FLOAT(9, t->scale.z);
    jce_runtime_destroy(host.runtime);
    host.runtime = NULL;
    jce_scene_destroy(scene);
    TEST_ASSERT_EQUAL_INT64(81, jce_platform_stat_get(host.stats, "start_score", 0));
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    TEST_ASSERT_EQUAL_INT64(81, jce_platform_stat_get(host.stats, "start_score", 0));
    host.runtime = create_start_probe(&scene, &entity, host.stats, true);
    t = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(0, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(-7, t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(0, t->position.z);
    TEST_ASSERT_EQUAL_FLOAT(0, t->scale.x);
    jce_runtime_destroy(host.runtime);
    host.runtime = NULL;
    jce_scene_destroy(scene);
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
    TEST_ASSERT_TRUE(jce_runtime_stats_host_attach(&host, directory));
    TEST_ASSERT_NULL(jce_runtime_create_with_platform_services(NULL, host.stats));
    JceRuntimeDesc invalid = {0};
    TEST_ASSERT_NULL(jce_runtime_create_with_platform_services(&invalid, host.stats));
    TEST_ASSERT_EQUAL_INT64(81, jce_platform_stat_get(host.stats, "start_score", 0));
    TEST_ASSERT_TRUE(jce_runtime_stats_host_close(&host));
    TEST_ASSERT_NULL(host.stats);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_services_and_quit_is_deferred_request);
    RUN_TEST(test_borrowed_stats_persist_and_detach);
    RUN_TEST(test_trusted_host_store_controls_and_restart);
    RUN_TEST(test_service_is_visible_during_on_start_and_legacy_is_unavailable);
    return UNITY_END();
}
