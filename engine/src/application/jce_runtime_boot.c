/* jce_runtime_boot.c */

#include <jce/application/jce_runtime_boot.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_pak_loader.h>

#include <stdint.h>
#include <string.h>

bool JCE_CALL jce_runtime_boot_scene_path_is_valid(const char *path)
{
    size_t segment_start = 0;
    size_t len;

    if (!path || !*path || path[0] == '/')
        return false;

    len = strlen(path);
    if (len >= JCE_RUNTIME_BOOT_SCENE_PATH_MAX)
        return false;

    for (size_t i = 0; i <= len; ++i) {
        const unsigned char c = (unsigned char)path[i];
        if (c == '\0') {
            const size_t segment_len = i - segment_start;
            if (segment_len == 0 ||
                (segment_len == 1 && path[segment_start] == '.') ||
                (segment_len == 2 && path[segment_start] == '.' &&
                 path[segment_start + 1] == '.')) {
                return false;
            }
            break;
        }
        if (c < 0x20 || c == '\\' || c == ':')
            return false;
        if (c == '/') {
            const size_t segment_len = i - segment_start;
            if (segment_len == 0 ||
                (segment_len == 1 && path[segment_start] == '.') ||
                (segment_len == 2 && path[segment_start] == '.' &&
                 path[segment_start + 1] == '.')) {
                return false;
            }
            segment_start = i + 1;
        }
    }

    return true;
}

bool JCE_CALL jce_runtime_boot_manifest_parse(
    const char *json, size_t json_size, JceRuntimeBootManifest *out_manifest)
{
    JceJson *root;
    const char *contract;
    const char *startup_scene;
    bool ok = false;

    if (!out_manifest)
        return false;
    memset(out_manifest, 0, sizeof(*out_manifest));
    if (!json || json_size == 0)
        return false;

    root = jce_json_parse(json, json_size);
    if (!root || !jce_json_is_object(root))
        goto done;

    contract = jce_json_get_string(root, "contract", NULL);
    startup_scene = jce_json_get_string(root, "startup_scene", NULL);
    {
        /* Accept any schema in [1, current]: schema-1 manifests predate the
         * player window metadata and parse fine with those fields left zeroed. */
        const int schema = jce_json_get_int(root, "schema", 0);
        if (!contract || strcmp(contract, JCE_RUNTIME_BOOT_CONTRACT) != 0 ||
            schema < 1 || schema > JCE_RUNTIME_BOOT_SCHEMA_VERSION ||
            !startup_scene ||
            (startup_scene[0] &&
             !jce_runtime_boot_scene_path_is_valid(startup_scene))) {
            goto done;
        }
    }

    memcpy(out_manifest->startup_scene, startup_scene,
           strlen(startup_scene) + 1);

    /* Optional player window metadata (schema >= 2; absent keys stay zeroed,
     * which the shipped game reads as "keep the engine-config default"). */
    {
        const char *title = jce_json_get_string(root, "window_title", NULL);
        if (title && title[0]) {
            size_t n = strlen(title);
            if (n >= JCE_RUNTIME_BOOT_TITLE_MAX)
                n = JCE_RUNTIME_BOOT_TITLE_MAX - 1;
            memcpy(out_manifest->window_title, title, n);
            out_manifest->window_title[n] = '\0';
        }
        out_manifest->window_width      = jce_json_get_int(root, "window_width", 0);
        out_manifest->window_height     = jce_json_get_int(root, "window_height", 0);
        out_manifest->fullscreen        = jce_json_get_int(root, "fullscreen", 0) != 0;
        out_manifest->run_in_background =
            jce_json_get_int(root, "run_in_background", 0) != 0;
    }
    ok = true;

done:
    if (root)
        jce_json_free(root);
    return ok;
}

bool JCE_CALL jce_runtime_boot_manifest_load_pak(
    const JcePakArchive *pak, JceRuntimeBootManifest *out_manifest)
{
    const JcePakAsset *asset;
    char *json;
    size_t size;
    bool ok;

    if (!out_manifest)
        return false;
    memset(out_manifest, 0, sizeof(*out_manifest));
    if (!pak)
        return false;

    asset = jce_pak_find(pak, JCE_RUNTIME_BOOT_MANIFEST_PATH);
    if (!asset || asset->original_size == 0 ||
        asset->original_size >= (uint64_t)SIZE_MAX) {
        return false;
    }
    size = (size_t)asset->original_size;
    json = (char *)jce_malloc(size + 1);
    if (!json)
        return false;

    if (jce_pak_decompress_ex(pak, asset, json, size) != size) {
        jce_free(json);
        return false;
    }
    json[size] = '\0';
    ok = jce_runtime_boot_manifest_parse(json, size, out_manifest);
    jce_free(json);
    return ok;
}
