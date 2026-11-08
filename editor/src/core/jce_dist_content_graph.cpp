/* jce_dist_content_graph.cpp  Build one cooked graph, independent of container. */

#include "jce_dist_content_graph.h"
#include "jce_dist_name_policy.h"
#include "jce_project_prepared_assets.h"

extern "C" {
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_pack.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

struct GraphLogBridge {
    JceDistContentGraphLogFn fn = nullptr;
    void *user = nullptr;
    std::string first_error;
};

void pack_log_bridge(JceBundlePackLogLevel level, const char *message,
                     void *user)
{
    GraphLogBridge *bridge = (GraphLogBridge *)user;
    if (bridge && level == JCE_BUNDLE_PACK_LOG_ERROR &&
        bridge->first_error.empty() && message && message[0])
        bridge->first_error = message; // Producer callback is bounded to 2047 bytes.
    if (bridge && bridge->fn)
        bridge->fn((int)level, message, bridge->user);
}

std::string slash_norm(std::string path)
{
    for (char &c : path)
        if (c == '\\') c = '/';
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

std::string join_path(const std::string &base, const std::string &leaf)
{
    std::string result = slash_norm(base);
    std::string suffix = slash_norm(leaf);
    while (!suffix.empty() && suffix.front() == '/') suffix.erase(0, 1);
    if (!result.empty() && !suffix.empty()) result += '/';
    result += suffix;
    return result;
}

/* Same address/normalization policy as the SDK tool's --shader-root port.
 * The public packer calls this only after its existing VFS/project reads miss. */
bool resolve_sdk_stock_shader(const char *vpath, char *out, size_t size, void *user)
{
    const char *root = static_cast<const char *>(user);
    char normalized[1024];
    if (!root || !vpath || jce_path_is_absolute(vpath) ||
        !jce_path_normalize(normalized, sizeof(normalized), vpath))
        return false;
    const size_t length = std::strlen(normalized);
    if (std::strncmp(normalized, "shaders/", 8) != 0 || length <= 12 ||
        std::strcmp(normalized + length - 4, ".bin") != 0)
        return false;
    const int written = std::snprintf(out, size, "%s/%s", root, normalized + 8);
    return written >= 0 && (size_t)written < size && jce_fs_host_exists_file(out);
}

bool fail(std::string *out_error, const std::string &message)
{
    if (out_error) *out_error = message;
    return false;
}

} // namespace

int jce_dist_content_graph_host_platform(void)
{
#if JCE_PLATFORM_WINDOWS
    return 0;
#elif JCE_PLATFORM_LINUX
    return 1;
#elif JCE_PLATFORM_MACOS
    return 2;
#elif JCE_PLATFORM_ANDROID
    return 3;
#elif JCE_PLATFORM_IOS
    return 4;
#elif JCE_PLATFORM_WEB
    return 5;
#else
    return 6;
#endif
}

bool jce_dist_content_graph_build(const std::string &project_root,
                                  const std::string &generated_dir,
                                  const std::string &reports_dir,
                                  int target_platform,
                                  JceDistContentGraphLogFn log_fn,
                                  void *log_user,
                                  JceDistContentGraph *out_graph,
                                  std::string *out_error,
                                  const std::string &sdk_root,
                                  const std::string &prepared_root)
{
    if (!out_graph)
        return fail(out_error, "Dist content graph output is null");
    *out_graph = JceDistContentGraph{};
    if (out_error) out_error->clear();

    // Match the SDK tool's explicit root resolution through the public facade.
    // Relative UI paths use the actual launch CWD; the resolver always receives
    // an absolute root, independent of whether an active editor VFS exists.
    std::string stock_root;
    if (!sdk_root.empty()) {
        char absolute_sdk[1024], cwd[1024], joined[2048];
        bool normalized;
        if (jce_path_is_absolute(sdk_root.c_str())) {
            normalized = jce_path_normalize(absolute_sdk, sizeof(absolute_sdk),
                                             sdk_root.c_str());
        } else {
            normalized = jce_fs_host_get_current_dir(cwd, sizeof(cwd)) &&
                jce_path_join(joined, sizeof(joined), cwd, sdk_root.c_str()) &&
                jce_path_normalize(absolute_sdk, sizeof(absolute_sdk), joined);
        }
        if (!normalized || !jce_path_is_absolute(absolute_sdk))
            return fail(out_error, "cannot resolve selected SDK root to an absolute host path");
        stock_root = join_path(absolute_sdk, "share/jce/shaders");
    }

    // Bound the private declaration before the public project loader reads it.
    std::vector<std::string> confidential_paths;
    if (!jce_dist_name_policy_load(join_path(project_root, JCE_PROJECT_FILENAME),
                                   &confidential_paths, out_error))
        return false;
    JceProject *project = jce_project_load(project_root.c_str());
    if (!project)
        return fail(out_error, "cannot load project manifest for Dist content graph");
    if (!project->source_assets || !project->source_assets[0] ||
        !project->startup_scene || !project->startup_scene[0]) {
        jce_project_free(project);
        return fail(out_error, "Dist requires source_assets and startup_scene roots");
    }

    const std::string source_root = join_path(project_root,
                                              project->source_assets);
    const std::string startup_scene = join_path(source_root,
                                                project->startup_scene);
    jce_project_free(project);
    if (!jce_fs_host_exists_file(startup_scene.c_str()))
        return fail(out_error, "Dist startup scene is missing: " + startup_scene);

    const std::string graph_dir = join_path(generated_dir, "content_graph");
    if (!jce_fs_host_create_directory(graph_dir.c_str()))
        return fail(out_error, "cannot create Dist content graph dir: " + graph_dir);

    const std::string bundle_path = join_path(graph_dir, "dist_graph.jbundle");
    const std::string sidecar_path = bundle_path + ".json";
    const std::string generated_snapshot = join_path(
        graph_dir, JCE_BUNDLE_GRAPH_NAME);
    (void)jce_fs_host_remove_file(bundle_path.c_str());
    (void)jce_fs_host_remove_file(sidecar_path.c_str());
    (void)jce_fs_host_remove_file(generated_snapshot.c_str());

    const char *scenes[] = {startup_scene.c_str()};
    JceBundlePackOptions options{};
    options.scenes_dir = source_root.c_str();
    options.resource_root = source_root.c_str();
    options.out_dir = graph_dir.c_str();
    options.scene_files = scenes;
    options.scene_file_count = 1;
    options.single_file_mode = true;
    options.single_bundle_id = "dist_graph";
    options.zstd_level = 3;
    options.cook_assets = true;
    options.target_platform = target_platform;
    if (!stock_root.empty()) {
        options.resolve_fn = resolve_sdk_stock_shader;
        options.resolve_user = stock_root.data();
    }

    JceProjectPreparedAssets prepared;
    if (!jce_project_prepared_assets_bind(project_root, &options, &prepared,
                                          out_error, prepared_root))
        return false;

    GraphLogBridge bridge{log_fn, log_user, {}};
    if (jce_bundle_pack_run(&options, pack_log_bridge, &bridge) != 0)
        return fail(out_error, "Dist content graph build failed" +
            (bridge.first_error.empty() ? std::string() : ": " + bridge.first_error));
    if (!jce_fs_host_exists_file(bundle_path.c_str()) ||
        !jce_fs_host_exists_file(generated_snapshot.c_str())) {
        return fail(out_error,
                    "Dist graph producer omitted its bundle or graph snapshot");
    }

    if (!jce_fs_host_create_directory(reports_dir.c_str()))
        return fail(out_error, "cannot create Dist reports directory");
    out_graph->snapshot_path = join_path(
        reports_dir, "project_content_graph_dist.json");
    if (!jce_fs_host_copy_file(generated_snapshot.c_str(),
                               out_graph->snapshot_path.c_str())) {
        return fail(out_error, "cannot preserve Dist graph snapshot: " +
                    out_graph->snapshot_path);
    }

    JceArchive *archive = jce_archive_open_file(bundle_path.c_str());
    if (!archive || !jce_archive_verify_header(archive)) {
        if (archive) jce_archive_close(archive);
        return fail(out_error, "cannot verify generated Dist graph bundle");
    }

    const uint32_t count = jce_archive_count(archive);
    for (uint32_t i = 0; i < count; ++i) {
        const JceArchiveEntry *entry = jce_archive_get(archive, i);
        const char *debug_path = jce_archive_debug_path(archive, i);
        if (!entry || !debug_path) {
            jce_archive_close(archive);
            return fail(out_error,
                        "Dist graph bundle lacks auditable virtual addresses");
        }
        if (std::strcmp(debug_path, JCE_BUNDLE_MANIFEST_VPATH) == 0)
            continue;
        if (entry->original_size > (uint64_t)SIZE_MAX) {
            jce_archive_close(archive);
            return fail(out_error, "Dist graph asset exceeds addressable memory");
        }

        std::vector<char> canonical(std::strlen(debug_path) + 1u);
        size_t canonical_len = jce_archive_normalize_path(
            debug_path, canonical.data(), canonical.size());
        if (canonical_len == 0) {
            jce_archive_close(archive);
            return fail(out_error,
                        "Dist graph contains an invalid virtual address");
        }

        JceDistContentGraphAsset asset;
        asset.source_label = bundle_path + "#" + canonical.data();
        asset.address.assign(canonical.data(), canonical_len);
        asset.bytes.resize(entry->original_size ?
                           (size_t)entry->original_size : 1u);
        size_t got = jce_archive_read(archive, entry, asset.bytes.data(),
                                      asset.bytes.size());
        if (got != entry->original_size ||
            !jce_archive_verify_entry(entry, asset.bytes.data(), got)) {
            std::string address = asset.address;
            jce_archive_close(archive);
            return fail(out_error,
                        "cannot decode Dist graph asset: " + address);
        }
        asset.bytes.resize(got);
        asset.content_id = jce_archive_content_hash(asset.bytes.data(),
                                                    asset.bytes.size());
        out_graph->assets.push_back(std::move(asset));
    }
    jce_archive_close(archive);

    std::sort(out_graph->assets.begin(), out_graph->assets.end(),
              [](const JceDistContentGraphAsset &a,
                 const JceDistContentGraphAsset &b) {
                  return a.address < b.address;
              });
    for (size_t i = 1; i < out_graph->assets.size(); ++i) {
        if (out_graph->assets[i - 1].address ==
            out_graph->assets[i].address) {
            return fail(out_error, "Dist graph repeats address: " +
                        out_graph->assets[i].address);
        }
    }
    if (out_graph->assets.empty())
        return fail(out_error, "Dist content graph is empty");
    for (const std::string &path : confidential_paths) {
        const auto found = std::lower_bound(out_graph->assets.begin(), out_graph->assets.end(), path,
            [](const JceDistContentGraphAsset &asset, const std::string &address) {
                return asset.address < address;
            });
        if (found == out_graph->assets.end() || found->address != path) {
            *out_graph = JceDistContentGraph{};
            return fail(out_error, "confidential_asset_paths: address is not in selected graph: " + path);
        }
    }
    out_graph->confidential_paths = std::move(confidential_paths);
    return true;
}
