/*
 * jce_editor_headless_build_request.cpp
 *
 * Validates the complete JCE_HEADLESS_BUILD_* contract without mutating
 * editor state.  The lifecycle driver consumes only a Valid request.
 */

#include "jce_editor_headless_build_request.h"

extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

struct Field {
    const char *name;
    std::string JceEditorHeadlessBuildRequest::*value;
};

constexpr Field kStringFields[] = {
    {"JCE_HEADLESS_BUILD_PROJECT",
     &JceEditorHeadlessBuildRequest::project},
    {"JCE_HEADLESS_BUILD_SDK",
     &JceEditorHeadlessBuildRequest::sdk},
    {"JCE_HEADLESS_BUILD_TARGET",
     &JceEditorHeadlessBuildRequest::target},
    {"JCE_HEADLESS_BUILD_EXE",
     &JceEditorHeadlessBuildRequest::exe},
    {"JCE_HEADLESS_BUILD_VARIANT",
     &JceEditorHeadlessBuildRequest::variant},
    {"JCE_HEADLESS_BUILD_ARCH",
     &JceEditorHeadlessBuildRequest::arch},
    {"JCE_HEADLESS_BUILD_OUT",
     &JceEditorHeadlessBuildRequest::package_out},
    {"JCE_HEADLESS_BUILD_RESULT",
     &JceEditorHeadlessBuildRequest::result_path},
};

void fail(JceEditorHeadlessBuildRequest &request, std::string message)
{
    request.error = std::move(message);
}

bool one_of(const std::string &value,
            const char *a, const char *b, const char *c = nullptr)
{
    return value == a || value == b || (c && value == c);
}

bool safe_leaf(const std::string &value)
{
    if (value.empty() || value == "." || value == ".." ||
        value.find("..") != std::string::npos) {
        return false;
    }
    return value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos &&
           value.find(':') == std::string::npos;
}

bool canonical_normalized(const std::string &path, std::string &out)
{
    char normalized[2048] = {};
    if (!jce_path_normalize(normalized, sizeof(normalized), path.c_str()))
        return false;
    out = normalized;
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
#if JCE_PLATFORM_WINDOWS
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
#endif
    return true;
}

bool path_is_within(const std::string &path, const std::string &directory)
{
    std::string canonical_path;
    std::string canonical_dir;
    if (!canonical_normalized(path, canonical_path) ||
        !canonical_normalized(directory, canonical_dir)) {
        return false;
    }
    if (canonical_path == canonical_dir)
        return true;
    return canonical_path.size() > canonical_dir.size() &&
           canonical_path.compare(0, canonical_dir.size(), canonical_dir) == 0 &&
           canonical_path[canonical_dir.size()] == '/';
}

const char *process_env(void *, const char *name)
{
    return std::getenv(name);
}

} // namespace

JceEditorHeadlessBuildRequestState
jce_editor_headless_build_request_parse(
    JceEditorHeadlessBuildRequest *out,
    JceEditorHeadlessEnvLookup lookup,
    void *user)
{
    if (!out)
        return JceEditorHeadlessBuildRequestState::Invalid;

    *out = JceEditorHeadlessBuildRequest{};
    if (!lookup) {
        fail(*out, "headless build environment lookup is missing");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }

    bool any = false;
    for (const Field &field : kStringFields) {
        const char *value = lookup(user, field.name);
        if (value && value[0]) {
            out->*(field.value) = value;
            any = true;
        }
    }
    const char *clean = lookup(user, "JCE_HEADLESS_BUILD_CLEAN");
    if (clean && clean[0])
        any = true;

    if (!any)
        return JceEditorHeadlessBuildRequestState::Inactive;

    for (const Field &field : kStringFields) {
        if ((out->*(field.value)).empty()) {
            fail(*out, std::string("missing ") + field.name);
            return JceEditorHeadlessBuildRequestState::Invalid;
        }
    }
    if (!clean || !clean[0]) {
        fail(*out, "missing JCE_HEADLESS_BUILD_CLEAN");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    if (std::strcmp(clean, "0") != 0 && std::strcmp(clean, "1") != 0) {
        fail(*out, "JCE_HEADLESS_BUILD_CLEAN must be 0 or 1");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    out->clean = clean[0] == '1';

    const std::string *absolute_paths[] = {
        &out->project, &out->sdk, &out->package_out, &out->result_path,
    };
    for (const std::string *path : absolute_paths) {
        if (!jce_path_is_absolute(path->c_str())) {
            fail(*out, "headless build paths must be absolute");
            return JceEditorHeadlessBuildRequestState::Invalid;
        }
        std::string normalized;
        if (!canonical_normalized(*path, normalized)) {
            fail(*out, "headless build path cannot be normalized");
            return JceEditorHeadlessBuildRequestState::Invalid;
        }
    }

    if (!safe_leaf(out->target)) {
        fail(*out, "headless build target must be a leaf name");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    if (!safe_leaf(out->exe)) {
        fail(*out, "headless build executable must be a leaf name");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    if (!one_of(out->variant, "release", "debug", "dist")) {
        fail(*out, "unsupported headless build variant");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    if (!one_of(out->arch, "x86_64", "i686", "aarch64")) {
        fail(*out, "unsupported headless build architecture");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }

    const std::string manifest = out->project + "/jce_project.json";
    if (!jce_fs_host_exists_file(manifest.c_str())) {
        fail(*out, "project does not contain jce_project.json");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    const std::string sdk_config_a =
        out->sdk + "/lib/cmake/JCE/JCEConfig.cmake";
    const std::string sdk_config_b = out->sdk + "/cmake/JCEConfig.cmake";
    if (!jce_fs_host_exists_file(sdk_config_a.c_str()) &&
        !jce_fs_host_exists_file(sdk_config_b.c_str())) {
        fail(*out, "SDK does not contain JCEConfig.cmake");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }
    if (path_is_within(out->result_path, out->package_out)) {
        fail(*out, "private result path must be outside the public package");
        return JceEditorHeadlessBuildRequestState::Invalid;
    }

    return JceEditorHeadlessBuildRequestState::Valid;
}

JceEditorHeadlessBuildRequestState
jce_editor_headless_build_request_parse_environment(
    JceEditorHeadlessBuildRequest *out)
{
    return jce_editor_headless_build_request_parse(
        out, process_env, nullptr);
}
