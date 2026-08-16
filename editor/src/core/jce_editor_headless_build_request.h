/*
 * jce_editor_headless_build_request.h
 *
 * UI-free parser for the editor's same-executable project-build automation
 * contract.  Parsing completes before project state or the build manager is
 * touched, so malformed automation requests fail closed.
 */

#ifndef JCE_EDITOR_HEADLESS_BUILD_REQUEST_H
#define JCE_EDITOR_HEADLESS_BUILD_REQUEST_H

#include <string>

enum class JceEditorHeadlessBuildRequestState {
    Inactive,
    Valid,
    Invalid,
};

struct JceEditorHeadlessBuildRequest {
    std::string project;
    std::string sdk;
    std::string target;
    std::string exe;
    std::string variant;
    std::string arch;
    std::string package_out;
    std::string result_path;
    bool clean = false;
    std::string error;
};

using JceEditorHeadlessEnvLookup =
    const char *(*)(void *user, const char *name);

JceEditorHeadlessBuildRequestState
jce_editor_headless_build_request_parse(
    JceEditorHeadlessBuildRequest *out,
    JceEditorHeadlessEnvLookup lookup,
    void *user);

JceEditorHeadlessBuildRequestState
jce_editor_headless_build_request_parse_environment(
    JceEditorHeadlessBuildRequest *out);

#endif /* JCE_EDITOR_HEADLESS_BUILD_REQUEST_H */
