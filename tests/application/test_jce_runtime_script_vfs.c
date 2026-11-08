/* Real Lua/runtime reads against mounted archives; host fallback must respect isolation. */
#include "unity.h"
#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_path.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <stdio.h>
#include <string.h>
static JceScene *scene;
static JceRuntime *runtime;
static JceFileSystem *fs;
static JcePakArchive *pak;
static void *blob;
static const JceFsPakProvider *previous;
static unsigned large_decompresses;
static char root[1024], script[1200], host_only[1200];
static const void *JCE_CALL find_asset(const JcePakArchive *archive, const char *path)
{ return jce_pak_find(archive, path); }
static uint64_t JCE_CALL asset_size(const void *asset)
{ return ((const JcePakAsset *)asset)->original_size; }
static size_t JCE_CALL decompress_asset(const void *asset, void *buffer, size_t capacity)
{
    const JcePakAsset *entry = asset;
    if (entry->original_size > JCE_SCRIPT_TEXT_ASSET_MAX_BYTES) ++large_decompresses;
    return jce_pak_decompress(entry, buffer, capacity);
}
static const JceFsPakProvider provider = {find_asset, asset_size, decompress_asset};
void setUp(void)
{
    char base[900];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(base, sizeof(base)));
    snprintf(root, sizeof(root), "%s/_ut_runtime_vfs", base);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(root));
    snprintf(script, sizeof(script), "%s/probe.lua", root);
    snprintf(host_only, sizeof(host_only), "%s/host-only.txt", root);
    previous = jce_fs_get_pak_provider();
    jce_fs_set_pak_provider(&provider);
    large_decompresses = 0;
}
void tearDown(void)
{
    if (runtime) jce_runtime_destroy(runtime);
    if (scene) jce_scene_destroy(scene);
    runtime = NULL; scene = NULL;
    jce_fs_set_active(NULL);
    if (fs) jce_fs_destroy(fs);
    if (pak) jce_pak_close(pak);
    jce_free(blob); fs = NULL; pak = NULL; blob = NULL;
    jce_fs_set_pak_provider(previous);
    jce_fs_host_remove_recursive(root);
}
static bool resolve(void *user, const char *path, char *out, int capacity)
{
    (void)user;
    int size = jce_path_is_absolute(path)
        ? snprintf(out, (size_t)capacity, "%s", path)
        : snprintf(out, (size_t)capacity, "%s/%s", root, path);
    return size >= 0 && size < capacity;
}
static JceEntity create_runtime(JceFsActivePolicy policy, bool archive_mount, bool relative_script)
{
    static const char source[] =
        "local M={}\nfunction M:on_start()\n"
        "local text=jce.asset_read_text('payload.txt')\n"
        "local missing=jce.asset_read_text('host-only.txt')\n"
        "local json,err=jce.asset_read_json('too-large.json')\n"
        "jce.log('READ_CONTROL text='..tostring(text)..' missing='..tostring(missing)..' error='..tostring(err))\n"
        "jce.set_position(self.entity,text=='VFS' and 1 or 0,missing==nil and 1 or 0,err=='too_large' and 1 or 0)\n"
        "jce.set_scale(self.entity,9,9,9)\n"
        "end\nreturn M\n";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(script, source, sizeof(source)-1));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(host_only, "HOST", 4));
    fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(fs);
    if (archive_mount) {
        JceArchiveWriter *writer = jce_archive_writer_create(NULL);
        TEST_ASSERT_NOT_NULL(writer);
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "payload.txt", "VFS", 3));
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "probe.lua", source, sizeof(source)-1));
        size_t bytes = JCE_SCRIPT_TEXT_ASSET_MAX_BYTES + 1u;
        char *large = jce_malloc(bytes); TEST_ASSERT_NOT_NULL(large);
        memset(large, ' ', bytes);
        TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "too-large.json", large, bytes));
        jce_free(large);
        size_t size = 0;
        TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, &blob, &size));
        jce_archive_writer_destroy(writer);
        pak = jce_pak_open(blob, size); TEST_ASSERT_NOT_NULL(pak);
        TEST_ASSERT_TRUE(jce_fs_mount_pak(fs, pak));
    }
    jce_fs_set_active_policy(fs, policy);
    scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "runtime-vfs-probe");
    JceScriptComponent component = {0};
    snprintf(component.script_path, sizeof(component.script_path), "%s", relative_script ? "probe.lua" : script);
    jce_scene_set_script(scene, entity, &component);
    jce_scene_set_component_enabled(scene, entity, JCE_COMP_FLAG_SCRIPT, true);
    JceRuntimeDesc desc = {0};
    desc.scene = scene; desc.resolve_path_fn = resolve;
    runtime = jce_runtime_create(&desc); TEST_ASSERT_NOT_NULL(runtime);
    return entity;
}
static void test_bounded_archive_read_and_isolated_miss_use_actual_runtime(void)
{
    JceEntity entity = create_runtime(JCE_FS_ACTIVE_ISOLATED, true, false);
    const JceTransform *position = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_NOT_NULL(position);
    TEST_ASSERT_EQUAL_FLOAT(1, position->position.x);
    TEST_ASSERT_EQUAL_FLOAT(1, position->position.y);
    TEST_ASSERT_EQUAL_FLOAT(1, position->position.z);
    TEST_ASSERT_EQUAL_UINT32(0, large_decompresses);
}
static void test_overlay_missing_path_retains_explicit_live_host_fallback(void)
{
    JceEntity entity = create_runtime(JCE_FS_ACTIVE_OVERLAY, false, false);
    const JceTransform *position = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_NOT_NULL(position);
    TEST_ASSERT_EQUAL_FLOAT(0, position->position.y);
}
static void test_uncapped_script_source_uses_the_same_mounted_archive(void)
{
    JceEntity entity = create_runtime(JCE_FS_ACTIVE_ISOLATED, true, true);
    const JceTransform *value = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(1, value->position.x);
    TEST_ASSERT_EQUAL_FLOAT(1, value->position.y);
    TEST_ASSERT_EQUAL_FLOAT(1, value->position.z);
    TEST_ASSERT_EQUAL_FLOAT(9, value->scale.x);
    TEST_ASSERT_EQUAL_UINT32(0, large_decompresses);
}
static void test_uncapped_isolated_script_miss_cannot_escape_to_live_source(void)
{
    JceEntity entity = create_runtime(JCE_FS_ACTIVE_ISOLATED, false, true);
    const JceTransform *value = jce_scene_get_transform(scene, entity);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_FLOAT(1, value->scale.x); /* Source did not execute. */
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bounded_archive_read_and_isolated_miss_use_actual_runtime);
    RUN_TEST(test_overlay_missing_path_retains_explicit_live_host_fallback);
    RUN_TEST(test_uncapped_script_source_uses_the_same_mounted_archive);
    RUN_TEST(test_uncapped_isolated_script_miss_cannot_escape_to_live_source);
    return UNITY_END();
}
