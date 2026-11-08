/* Package admission and single-file publication; no directory deletion. */
#ifndef JCE_BUILD_PACKAGE_H
#define JCE_BUILD_PACKAGE_H
#include <string>

/* Portable leaf names. UTF-8 names are allowed; path separators, control
 * characters, parent traversal and Windows filename aliases are rejected. */
bool jce_build_package_component_valid(const std::string &name,
                                       std::string *error = nullptr);

/* Shared by Build Profiles and the build API. Does not create directories. */
bool jce_build_package_default_path(const std::string &project,
    const std::string &app, const std::string &platform,
    const std::string &arch, const std::string &variant,
    std::string *out, std::string *error = nullptr);

/* Normalize an absolute output path and reject source/build/SDK overlap.
 * Existing contents are checked before the build and again before publishing.
 * `cooked` may be project-relative. Dist accepts only an empty directory or
 * the same-name old application; authoring requires an empty directory. */
bool jce_build_package_admit(const std::string &project,
    const std::string &sdk, const std::string &cooked,
    const std::string &requested, const std::string &exe_name, bool dist,
    std::string *out, std::string *error = nullptr);
bool jce_build_package_check_directory(const std::string &out,
    const std::string &exe_name, bool replace_same_file,
    std::string *error = nullptr);

using JceBuildPackageAuditFn = bool (*)(const std::string &exe,
    const char *package_dir, void *user);

/* Validate the built artifact, copy into an owned reports candidate,
 * validate that one-file candidate, then publish by public host rename. Never
 * remove the destination on rename failure. Cross-volume/non-replacing host
 * rename fails closed; callers retain the old complete application. */
bool jce_build_package_publish_dist(const std::string &built_exe,
    const std::string &reports_dir, const std::string &out,
    const std::string &exe_name, JceBuildPackageAuditFn audit, void *user,
    std::string *error = nullptr);
#endif
