#include "jce_project_prepared_assets.h"

#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

#include <algorithm>
#include <cstdio>

namespace {

bool absolute_path(const std::string &path, std::string *out)
{
    char normalized[2048], joined[2048], cwd[2048];
    if (path.empty()) {
        out->clear();
        return true;
    }
    const char *candidate = path.c_str();
    if (!jce_path_is_absolute(candidate)) {
        if (!jce_fs_host_get_current_dir(cwd, sizeof(cwd)) ||
            !jce_path_join(joined, sizeof(joined), cwd, candidate))
            return false;
        candidate = joined;
    }
    if (!jce_path_normalize(normalized, sizeof(normalized), candidate) ||
        !jce_path_is_absolute(normalized))
        return false;
    *out = normalized;
    return true;
}

bool joined_path(const std::string &root, const char *leaf, std::string *out)
{
    if (root.empty() || !leaf || !leaf[0]) {
        out->clear();
        return true;
    }
    char joined[2048];
    return jce_path_join(joined, sizeof(joined), root.c_str(), leaf) &&
        absolute_path(joined, out);
}

bool resolve_prepared(const char *address, char *out, size_t capacity, void *user)
{
    const auto *inputs = static_cast<const JceProjectPreparedAssets *>(user);
    if (!inputs || !address || !out || !capacity)
        return false;
    for (const auto &source : inputs->sources) {
        if (source.address == address) {
            const int written = std::snprintf(out, capacity, "%s",
                                               source.host_path.c_str());
            return written >= 0 && static_cast<size_t>(written) < capacity;
        }
    }
    return inputs->fallback && inputs->fallback(address, out, capacity,
                                                  inputs->fallback_user);
}

void append_root(JceProjectPreparedAssets *inputs, const char *address)
{
    if (address && address[0] &&
        std::find(inputs->roots.begin(), inputs->roots.end(), address) ==
            inputs->roots.end())
        inputs->roots.emplace_back(address);
}

bool fail(std::string *error, const char *message)
{
    if (error) *error = message;
    return false;
}

} // namespace

bool jce_project_prepared_assets_bind(
    const std::string &project_root, JceBundlePackOptions *options,
    JceProjectPreparedAssets *inputs, std::string *error,
    const std::string &prepared_root)
{
    if (!options || !inputs)
        return fail(error, "prepared runtime configuration inputs are null");
    *inputs = JceProjectPreparedAssets{};
    inputs->fallback = options->resolve_fn;
    inputs->fallback_user = options->resolve_user;
    if (error) error->clear();

    std::string project_path, cooked_path, manifest_path, absolute_source;
    inputs->source_root = options->resource_root ? options->resource_root : "";
    if (!absolute_path(project_root, &project_path) ||
        !absolute_path(prepared_root, &cooked_path) ||
        !absolute_path(inputs->source_root, &absolute_source) ||
        !joined_path(project_path, JCE_PROJECT_FILENAME, &manifest_path))
        return fail(error, "cannot resolve prepared runtime configuration roots");

    if (!manifest_path.empty() && jce_fs_host_exists_file(manifest_path.c_str())) {
        JceProject *project = jce_project_load(project_path.c_str());
        if (!project)
            return fail(error, "cannot load project for prepared runtime configurations");
        // Preserve the caller's relative/absolute spelling for scene-vpath
        // derivation. Only callback fallback paths need absolute host names.
        char source[2048];
        bool source_ok = true;
        if (inputs->source_root.empty() && project->source_assets &&
            project->source_assets[0]) {
            source_ok = jce_path_join(source, sizeof(source), project_root.c_str(),
                                      project->source_assets);
            if (source_ok) inputs->source_root = source;
        }
        const bool paths_ok = source_ok &&
            (!cooked_path.empty() || joined_path(project_path,
                project->cooked_assets, &cooked_path));
        jce_project_free(project);
        if (!paths_ok)
            return fail(error, "prepared runtime configuration path exceeds capacity");
    }

    for (int i = 0; i < options->always_included_count; ++i)
        append_root(inputs, options->always_included ? options->always_included[i] : nullptr);

    struct CanonicalConfig {
        const char *address;
        const char *project_fallback;
    };
    static const CanonicalConfig configs[] = {
        {"render_settings.json", nullptr},
        {"physics_layers.json", nullptr},
        {"settings/project-settings.json", ".jce/project-settings.json"},
        {"settings/render_pipeline.rp.json", "Settings/RenderPipeline.rp.json"},
        {"settings/input_actions.json", ".jce/input_actions.json"}
    };
    JceFileSystem *fs = jce_fs_get_active();
    for (const auto &config : configs) {
        std::string source, prepared, authored;
        if (!joined_path(inputs->source_root, config.address, &source) ||
            !joined_path(cooked_path, config.address, &prepared) ||
            !joined_path(project_path, config.project_fallback, &authored))
            return fail(error, "prepared runtime configuration path exceeds capacity");

        // These are fallbacks after the public owner's existing VFS/source reads.
        const std::string *selected = nullptr;
        if (!prepared.empty() && jce_fs_host_exists_file(prepared.c_str()))
            selected = &prepared;
        else if (!authored.empty() && jce_fs_host_exists_file(authored.c_str()))
            selected = &authored;
        const bool present = selected ||
            (!source.empty() && jce_fs_host_exists_file(source.c_str())) ||
            (fs && jce_fs_exists(fs, config.address));
        if (!present)
            continue;
        append_root(inputs, config.address);
        if (selected)
            inputs->sources.push_back({config.address, *selected});
    }

    inputs->root_pointers.reserve(inputs->roots.size());
    for (const auto &root : inputs->roots)
        inputs->root_pointers.push_back(root.c_str());
    if ((!options->resource_root || !options->resource_root[0]) &&
        !inputs->source_root.empty())
        options->resource_root = inputs->source_root.c_str();
    options->always_included = inputs->root_pointers.data();
    options->always_included_count = static_cast<int>(inputs->root_pointers.size());
    options->resolve_fn = resolve_prepared;
    options->resolve_user = inputs;
    return true;
}
