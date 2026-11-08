#include "stock_bundle.h"
#include <jce/api_app.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_json.h>
#include <string.h>
#define STOCK_MANIFEST_LIMIT (4u * 1024u * 1024u)
#define STOCK_ASSET_LIMIT 16384
struct JceStockBundle {
    JceBundleFile *file;
    JceJson *manifest;
};
static bool text_is(const char *left, const char *right)
{
    return left && strcmp(left, right) == 0;
}
static bool unique_tree(const JceJson *node, unsigned depth)
{
    JceJson *child;
    if (depth > 64) return false;
    for (child = jce_json_first_child(node); child;
         child = jce_json_next_sibling(child)) {
        if (jce_json_is_object(node)) {
            const char *key = jce_json_member_key(child);
            JceJson *prior;
            if (!key) return false;
            for (prior = jce_json_first_child(node); prior != child;
                 prior = jce_json_next_sibling(prior)) {
                if (text_is(jce_json_member_key(prior), key)) return false;
            }
        }
        if (!unique_tree(child, depth + 1)) return false;
    }
    return true;
}
static bool extension_is(const char *path, const char *wanted)
{
    const char *extension = path ? strrchr(path, '.') : NULL;
    if (!extension) return false;
    while (*extension && *wanted) {
        unsigned char c = (unsigned char)*extension++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 'a' - 'A');
        if (c != (unsigned char)*wanted++) return false;
    }
    return !*extension && !*wanted;
}
static bool stock_assets(const JceJson *assets)
{
    int count = jce_json_array_size(assets);
    int index;
    if (!jce_json_is_array(assets) || count < 1 || count > STOCK_ASSET_LIMIT)
        return false;
    for (index = 0; index < count; ++index) {
        const JceJson *asset = jce_json_array_at(assets, index);
        const char *path = jce_json_get_string(asset, "path", NULL);
        const char *type = jce_json_get_string(asset, "type", NULL);
        const char *representation = jce_json_get_string(asset, "representation", NULL);
        bool lua = text_is(representation, "lua.source");
        bool js = text_is(representation, "js.source");
        if (!path || !type || !representation) return false;
        /* Reject native/optional-VM aliases even when falsely typed as data. */
        if (extension_is(path, ".jcec") || extension_is(path, ".jcecpp") ||
            extension_is(path, ".py") || extension_is(path, ".java") ||
            extension_is(path, ".class") || extension_is(path, ".cs") ||
            extension_is(path, ".csharp") || extension_is(path, ".c") ||
            extension_is(path, ".cpp")) return false;
        if (text_is(type, "script")) {
            if (!lua && !js) return false;
            if ((extension_is(path, ".lua") && !lua) ||
                (extension_is(path, ".jcejs") && !js)) return false;
        } else if (lua || js || extension_is(path, ".lua") ||
                   extension_is(path, ".jcejs") ||
                   text_is(representation, "c.class-ref") ||
                   text_is(representation, "cpp.class-ref") ||
                   text_is(representation, "csharp.class-ref") ||
                   text_is(representation, "python.source") ||
                   text_is(representation, "java.source") ||
                   text_is(representation, "java.class")) return false;
    }
    return true;
}
bool jce_stock_bundle_scene_allowed(const JceStockBundle *bundle, const char *path)
{
    const JceJson *assets;
    int index, count;
    if (!bundle || !jce_runtime_boot_scene_path_is_valid(path)) return false;
    assets = jce_json_get(bundle->manifest, "assets");
    count = jce_json_array_size(assets);
    for (index = 0; index < count; ++index) {
        const JceJson *asset = jce_json_array_at(assets, index);
        if (text_is(jce_json_get_string(asset, "path", NULL), path))
            return text_is(jce_json_get_string(asset, "type", NULL), "scene");
    }
    return false;
}
void jce_stock_bundle_close(JceStockBundle *bundle)
{
    if (!bundle) return;
    jce_json_free(bundle->manifest);
    jce_bundle_file_close(bundle->file);
    jce_free(bundle);
}
/* Bound decompression before the shared Bundle reader verifies its graph. */
static bool stock_archive_budget(const void *bytes, size_t size)
{
    JcePakArchive *pak = jce_pak_open(bytes, size);
    uint64_t remaining = UINT64_C(512) * 1024 * 1024;
    uint32_t index, count;
    bool valid = false;
    if (!pak) return false;
    /* Inspecting a Bundle parses this entry before our typed-table checks. */
    {
        const JcePakAsset *manifest = jce_pak_find(pak, "__bundle__/manifest.json");
        if (!manifest || !manifest->original_size ||
            manifest->original_size > STOCK_MANIFEST_LIMIT) goto done;
    }
    count = jce_pak_count(pak);
    if (count < 2 || count > STOCK_ASSET_LIMIT + 1) goto done;
    for (index = 0; index < count; ++index) {
        const JcePakAsset *asset = jce_pak_get(pak, index);
        if (!asset || asset->original_size > remaining) goto done;
        remaining -= asset->original_size;
    }
    valid = true;
done:
    jce_pak_close(pak);
    return valid;
}
JceStockBundle *jce_stock_bundle_open(const void *bytes, size_t size)
{
    JceStockBundle *bundle;
    const JcePakAsset *asset;
    JcePakArchive *pak;
    char *text = NULL;
    const JceJson *contract;
    const JceJson *dependencies;
    if (!stock_archive_budget(bytes, size)) return NULL;
    bundle = (JceStockBundle *)jce_malloc(sizeof(*bundle));
    if (!bundle) return NULL;
    memset(bundle, 0, sizeof(*bundle));
    bundle->file = jce_bundle_file_inspect_memory(bytes, size, "windows");
    if (!bundle->file) goto fail;
    pak = jce_bundle_file_pak(bundle->file);
    if (!pak || jce_pak_verify_all(pak) != 0) goto fail;
    asset = jce_pak_find(pak, "__bundle__/manifest.json");
    if (!asset || !asset->original_size ||
        asset->original_size > STOCK_MANIFEST_LIMIT) goto fail;
    text = (char *)jce_malloc((size_t)asset->original_size + 1);
    if (!text || jce_pak_decompress_ex(pak, asset, text,
            (size_t)asset->original_size) != (size_t)asset->original_size ||
        !jce_pak_verify(asset, text, (size_t)asset->original_size)) goto fail;
    text[asset->original_size] = 0;
    bundle->manifest = jce_json_parse_strict(text, (size_t)asset->original_size);
    jce_free(text);
    text = NULL;
    if (!bundle->manifest || !unique_tree(bundle->manifest, 0)) goto fail;
    contract = jce_json_get(bundle->manifest, "contract");
    dependencies = jce_json_get(bundle->manifest, "depends_on");
    if (!text_is(jce_json_get_string(contract, "name", NULL), "jce.bundle") ||
        jce_json_get_number(contract, "major", -1) != 1 ||
        jce_json_get_number(contract, "minor", -1) != 1 ||
        !jce_json_is_array(dependencies) || jce_json_array_size(dependencies) != 0 ||
        !stock_assets(jce_json_get(bundle->manifest, "assets")) ||
        !jce_stock_bundle_scene_allowed(bundle,
            jce_bundle_file_scene_path(bundle->file))) goto fail;
    return bundle;
fail:
    jce_free(text);
    jce_stock_bundle_close(bundle);
    return NULL;
}
