/* Project runtime configurations shared by Bundle and Dist graph producers. */
#ifndef JCE_PROJECT_PREPARED_ASSETS_H
#define JCE_PROJECT_PREPARED_ASSETS_H

#include <jce/resource/jce_bundle_pack.h>

#include <string>
#include <vector>

using JceProjectPreparedResolveFn = bool (*)(const char *, char *, size_t, void *);

struct JceProjectPreparedAssetSource {
    std::string address;
    std::string host_path;
};

struct JceProjectPreparedAssets {
    std::string source_root;
    std::vector<std::string> roots;
    std::vector<const char *> root_pointers;
    std::vector<JceProjectPreparedAssetSource> sources;
    JceProjectPreparedResolveFn fallback = nullptr;
    void *fallback_user = nullptr;
};

/* Borrowed for the subsequent public pack call; do not move or mutate inputs
 * until that call returns. Existing roots and resolver are composed, not replaced.
 * The finite canonical render, physics, project-settings and input-actions roots
 * are required only when present in VFS/source/prepared/project settings. The
 * public packer retains VFS -> source -> resolver precedence; prepared bytes
 * never override an existing canonical source. A selected fallback that later
 * disappears is still a required root and fails the public pack operation.
 * Manifest paths fill absent roots; an explicit prepared_root is a host path. */
bool jce_project_prepared_assets_bind(
    const std::string &project_root, JceBundlePackOptions *options,
    JceProjectPreparedAssets *inputs, std::string *error,
    const std::string &prepared_root = std::string());

#endif /* JCE_PROJECT_PREPARED_ASSETS_H */
