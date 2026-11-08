/* Exact public default-main owner, CPU-only selected entry bodies.
 * Archive production/inspection and typed-scene admission are real public APIs;
 * getenv is the single dependency port for deterministic QA scene selection. */
#include "../stock_bundle.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_bundle_pack.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static JceStockBundle *policy_bundle;
static char *qa_scene;
static char *policy_getenv(const char *name)
{
    return strcmp(name, "JCE_STARTUP_SCENE") == 0 ? qa_scene : NULL;
}
static bool policy_scene_allowed(const char *path)
{
    return jce_stock_bundle_scene_allowed(policy_bundle, path);
}
#ifndef JCE_STOCK_POLICY_OWNER
#define JCE_STOCK_POLICY_OWNER <jce/application/jce_default_main.inc.h>
#endif
#define JCE_DEFAULT_MAIN_PACKAGED_ONLY 1
#define JCE_DEFAULT_MAIN_PACKAGED_SCENE_ALLOWED(path) policy_scene_allowed(path)
#define getenv policy_getenv
#include JCE_STOCK_POLICY_OWNER
#undef getenv
static int checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    fprintf(stderr, "stock policy line %d: %s\n", __LINE__, #value); } } while (0)
static bool write_text(const char *path, const char *text)
{
    return jce_fs_host_write_all(path, text, (uint64_t)strlen(text));
}
int main(void)
{
    char root[1200], relative_root[1024], base[1024];
    char scene_path[1400], script_path[1400], output_dir[1400];
    char bundle_path[1400], sentinel[1200], relative_sentinel[1200], resolved[1200];
    const char *files[1];
    const char *included[2] = {"payload.txt", "script.lua"};
    JceBundlePackOptions options;
    JceBundleFile *mounted[1] = {NULL};
    JceFileSystem *mount_fs = NULL;
    void *bytes = NULL, *read = NULL;
    uint64_t size = 0, read_size = 0;
    if (!jce_fs_host_get_current_dir(base, sizeof(base)) ||
        !jce_fs_host_make_unique_path("stock_policy_fixture", relative_root, sizeof(relative_root)))
        return 1;
    if (snprintf(root, sizeof(root), "%s/%s", base, relative_root) >= (int)sizeof(root) ||
        !jce_fs_host_create_directory(root)) return 1;
    snprintf(scene_path, sizeof(scene_path), "%s/main.scene.json", root);
    snprintf(script_path, sizeof(script_path), "%s/script.lua", root);
    snprintf(output_dir, sizeof(output_dir), "%s/out", root);
    snprintf(bundle_path, sizeof(bundle_path), "%s/stock.jbundle", output_dir);
    snprintf(sentinel, sizeof(sentinel), "%s/payload.txt", root);
    CHECK(write_text(scene_path, "{\"entities\":[]}"));
    /* A ScriptAsset whose bytes also parse as a scene catches a missing typed
     * admission guard: an extension or parse failure cannot pass this control. */
    CHECK(write_text(script_path, "{\"entities\":[]}"));
    CHECK(write_text(sentinel, "inside"));
    memset(&options, 0, sizeof(options));
    files[0] = scene_path;
    options.resource_root = root;
    options.scenes_dir = root;
    options.out_dir = output_dir;
    options.scene_files = files;
    options.scene_file_count = 1;
    options.single_file_mode = true;
    options.single_bundle_id = "stock";
    options.always_included = included;
    options.always_included_count = 2;
    CHECK(jce_bundle_pack_run(&options, NULL, NULL) == 0);
    bytes = jce_fs_host_read_capped(bundle_path, 8u * 1024u * 1024u, &size, NULL);
    CHECK(bytes != NULL && size > 0);
    if (!bytes) goto done;
    policy_bundle = jce_stock_bundle_open(bytes, (size_t)size);
    CHECK(policy_bundle != NULL);
    mount_fs = jce_fs_create();
    CHECK(mount_fs != NULL);
    if (!mount_fs) goto done;
    mounted[0] = jce_bundle_file_open_memory(mount_fs, bytes, (size_t)size, "stock_policy");
    CHECK(mounted[0] != NULL);
    if (!policy_bundle || !mounted[0]) goto done;
    s_bundle_files = mounted;
    s_bundle_count = 1;
    s_engine_pak = jce_bundle_file_pak(mounted[0]);
    CHECK(jce_default_mount_runtime_assets(NULL));
    CHECK(jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED);
    read = jce_fs_host_read_all("payload.txt", &read_size);
    CHECK(read != NULL && read_size == 6 && memcmp(read, "inside", 6) == 0);
    jce_fs_buffer_free(read);
    read = NULL;
    snprintf(sentinel, sizeof(sentinel), "%s/outside.txt", root);
    snprintf(relative_sentinel, sizeof(relative_sentinel), "%s/outside.txt", relative_root);
    CHECK(write_text(sentinel, "outside"));
    /* Exact relative name exists on host but not in the active archive. */
    read = jce_fs_host_read_all(relative_sentinel, &read_size);
    CHECK(read == NULL);
    jce_fs_buffer_free(read);
    read = NULL;
    CHECK(!s_default_resolve_path("payload.txt", resolved, sizeof(resolved), NULL));
    qa_scene = "script.lua";
    CHECK(!jce_default_load_startup_scene(NULL, NULL));
    if (s_scene) { jce_scene_destroy(s_scene); s_scene = NULL; }
    qa_scene = "../main.scene.json";
    CHECK(!jce_default_load_startup_scene(NULL, NULL));
    qa_scene = NULL;
    CHECK(jce_default_load_startup_scene(NULL, NULL));
    CHECK(s_scene != NULL);
done:
    if (s_scene) { jce_scene_destroy(s_scene); s_scene = NULL; }
    if (s_runtime_fs) {
        if (jce_fs_get_active() == s_runtime_fs) jce_fs_set_active(NULL);
        jce_fs_destroy(s_runtime_fs);
        s_runtime_fs = NULL;
    }
    s_engine_pak = NULL;
    s_bundle_count = 0;
    s_bundle_files = NULL;
    jce_bundle_file_close(mounted[0]);
    if (mount_fs) jce_fs_destroy(mount_fs);
    jce_stock_bundle_close(policy_bundle);
    jce_fs_buffer_free(bytes);
    printf("stock policy: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
