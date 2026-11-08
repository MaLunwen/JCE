/* Actual public packer -> actual Bundle reader -> stock capability owner.
 * CPU archive/resource test only; no runtime scene execution or GPU. */
#include "../stock_bundle.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_bundle_pack.h>
#include <stdio.h>
#include <string.h>
static int checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    fprintf(stderr, "stock Bundle line %d: %s\n", __LINE__, #value); } } while (0)
static bool text_file(const char *path, const char *text)
{
    return jce_fs_host_write_all(path, text, (uint64_t)strlen(text));
}
static void run_case(const char *extension, const char *script, bool accepted)
{
    char root[1200], relative_root[1024], base[1024];
    char scene_path[1400], script_dir[1400], script_path[1600];
    char output_dir[1200], bundle_path[1400], scene[1024];
    const char *files[1];
    uint64_t size = 0;
    void *bytes;
    JceBundlePackOptions options;
    JceStockBundle *bundle;
    if (!jce_fs_host_get_current_dir(base, sizeof(base)) ||
        !jce_fs_host_make_unique_path("stock_bundle_fixture", relative_root, sizeof(relative_root)) ||
        snprintf(root, sizeof(root), "%s/%s", base, relative_root) >= (int)sizeof(root) ||
        !jce_fs_host_create_directory(root)) {
        CHECK(false);
        return;
    }
    snprintf(scene_path, sizeof(scene_path), "%s/main.scene.json", root);
    snprintf(script_dir, sizeof(script_dir), "%s/scripts", root);
    snprintf(script_path, sizeof(script_path), "%s/main.%s", script_dir, extension);
    snprintf(output_dir, sizeof(output_dir), "%s/out", root);
    snprintf(bundle_path, sizeof(bundle_path), "%s/stock.jbundle", output_dir);
    CHECK(jce_fs_host_create_directory(script_dir));
    CHECK(jce_fs_host_create_directory(output_dir));
    snprintf(scene, sizeof(scene),
        "{\"entities\":[{\"scriptPath\":\"scripts/main.%s\"}]}", extension);
    CHECK(text_file(scene_path, scene));
    CHECK(text_file(script_path, script));
    memset(&options, 0, sizeof(options));
    options.resource_root = root;
    options.scenes_dir = root;
    options.out_dir = output_dir;
    files[0] = scene_path;
    options.scene_files = files;
    options.scene_file_count = 1;
    options.single_file_mode = true;
    options.single_bundle_id = "stock";
    options.zstd_level = 1;
    CHECK(jce_bundle_pack_run(&options, NULL, NULL) == 0);
    bytes = jce_fs_host_read_capped(bundle_path, 8u * 1024u * 1024u, &size, NULL);
    CHECK(bytes != NULL && size > 0);
    if (!bytes) return;
    bundle = jce_stock_bundle_open(bytes, (size_t)size);
    CHECK((bundle != NULL) == accepted);
    if (bundle) {
        CHECK(jce_stock_bundle_scene_allowed(bundle, "main.scene.json"));
        CHECK(!jce_stock_bundle_scene_allowed(bundle, "scripts/main.lua"));
        CHECK(!jce_stock_bundle_scene_allowed(bundle, "missing.scene.json"));
        CHECK(!jce_stock_bundle_scene_allowed(bundle, "../main.scene.json"));
        jce_stock_bundle_close(bundle);
    }
    /* Keep the actual producer bytes/evidence in this test's ignored CWD. */
    ((unsigned char *)bytes)[0] ^= 1;
    bundle = jce_stock_bundle_open(bytes, (size_t)size);
    CHECK(bundle == NULL);
    jce_stock_bundle_close(bundle);
    jce_fs_buffer_free(bytes);
}
int main(void)
{
    run_case("lua", "return {}\n", true);
    run_case("jcejs", "function on_start() {}\n", true);
    run_case("py", "def on_start(): pass\n", false);
    run_case("jcec", "Controller\n", false);
    run_case("jcecpp", "Controller\n", false);
    printf("stock Bundle: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
