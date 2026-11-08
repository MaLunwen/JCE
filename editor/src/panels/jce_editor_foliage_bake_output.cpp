#include "jce_editor_foliage_bake_output.h"
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_filesystem.h>
#include <cstdio>
#include <cstring>

bool jce_editor_foliage_bake_paths(const char *project_root,
    const char *source_assets, bool isolated, uint64_t entity, uint32_t seed,
    char *asset_key, size_t key_capacity, char *host_path, size_t host_capacity)
{
    if (asset_key && key_capacity) asset_key[0] = '\0';
    if (host_path && host_capacity) host_path[0] = '\0';
    if (!asset_key || !key_capacity || !host_path || !host_capacity ||
        isolated || !project_root || !jce_path_is_absolute(project_root))
        return false;

    const char *source = source_assets && source_assets[0] ? source_assets : "assets";
    /* Colon rejects drive-relative names such as C:assets on Windows. */
    if (jce_path_is_absolute(source) || std::strchr(source, ':')) return false;
    char root[1024], relative_source[1024], joined[1024], asset_root[1024];
    if (!jce_path_normalize(root, sizeof root, project_root) ||
        !jce_path_normalize(relative_source, sizeof relative_source, source) ||
        !relative_source[0] ||
        (relative_source[0] == '.' && relative_source[1] == '.' &&
         (relative_source[2] == '/' || relative_source[2] == '\0')) ||
        !jce_path_join(joined, sizeof joined, root, relative_source) ||
        !jce_path_normalize(asset_root, sizeof asset_root, joined))
        return false;
    char relative_check[1024];
    if (!jce_path_relative(relative_check, sizeof relative_check, asset_root, root) ||
        (relative_check[0] == '.' && relative_check[1] == '.' &&
         (relative_check[2] == '/' || relative_check[2] == '\0')))
        return false;

    char key[160], output[1024];
    const int length = std::snprintf(key, sizeof key,
        "generated/foliage/vegetation_%llu_%u.foliage.bin",
        static_cast<unsigned long long>(entity), seed);
    if (length < 0 || static_cast<size_t>(length) >= sizeof key ||
        static_cast<size_t>(length) >= key_capacity ||
        !jce_path_join(output, sizeof output, asset_root, key) ||
        std::strlen(output) >= host_capacity)
        return false;
    std::memcpy(asset_key, key, static_cast<size_t>(length) + 1u);
    std::memcpy(host_path, output, std::strlen(output) + 1u);
    return true;
}

bool jce_editor_foliage_bake_write(const char *project_root,
    const char *source_assets, bool isolated, uint64_t entity, uint32_t seed,
    const void *data, uint64_t size, char *asset_key, size_t key_capacity,
    const char **reason)
{
    if (asset_key && key_capacity) asset_key[0] = '\0';
    if (reason) *reason = "invalid_output_context";
    char key[160], output[1024], parent[1024];
    if (!asset_key || !key_capacity || !data || !size ||
        !jce_editor_foliage_bake_paths(project_root, source_assets, isolated,
            entity, seed, key, sizeof key, output, sizeof output) ||
        std::strlen(key) >= key_capacity ||
        !jce_path_parent(parent, sizeof parent, output))
        return false;
    if (reason) *reason = "create_parent_failed";
    if (!jce_fs_host_create_directory(parent)) return false;
    if (reason) *reason = "atomic_write_failed";
    if (!jce_fs_host_write_all_atomic(output, data, size)) return false;
    std::memcpy(asset_key, key, std::strlen(key) + 1u);
    if (reason) *reason = "written_pending_render_validation";
    return true;
}
