#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_headless_build_request.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

#include <map>
#include <string>

namespace {

struct TestEnv {
    std::map<std::string, std::string> values;
};

const char *lookup_env(void *user, const char *name)
{
    auto *env = static_cast<TestEnv *>(user);
    const auto it = env->values.find(name);
    return it == env->values.end() ? nullptr : it->second.c_str();
}

struct Fixture {
    std::string root;
    std::string project;
    std::string sdk;
    std::string package;
    std::string result;

    Fixture()
    {
        char cwd[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        root = std::string(cwd) + "/_ut_editor_headless_build_request";
        project = root + "/project";
        sdk = root + "/sdk";
        package = root + "/package";
        result = root + "/result.json";

        (void)jce_fs_host_remove_recursive(root.c_str());
        REQUIRE(jce_fs_host_create_directory(project.c_str()));
        REQUIRE(jce_fs_host_create_directory(
            (sdk + "/lib/cmake/JCE").c_str()));
        static constexpr char kProject[] = "{}\n";
        static constexpr char kConfig[] = "# test fixture\n";
        REQUIRE(jce_fs_host_write_all(
            (project + "/jce_project.json").c_str(),
            kProject, sizeof(kProject) - 1));
        REQUIRE(jce_fs_host_write_all(
            (sdk + "/lib/cmake/JCE/JCEConfig.cmake").c_str(),
            kConfig, sizeof(kConfig) - 1));
    }

    ~Fixture()
    {
        (void)jce_fs_host_remove_recursive(root.c_str());
    }

    TestEnv valid_env() const
    {
        TestEnv env;
        env.values = {
            {"JCE_HEADLESS_BUILD_PROJECT", project},
            {"JCE_HEADLESS_BUILD_SDK", sdk},
            {"JCE_HEADLESS_BUILD_TARGET", "SpaceCodexDemo"},
            {"JCE_HEADLESS_BUILD_EXE", "space_codex_demo.exe"},
            {"JCE_HEADLESS_BUILD_VARIANT", "dist"},
            {"JCE_HEADLESS_BUILD_ARCH", "x86_64"},
            {"JCE_HEADLESS_BUILD_OUT", package},
            {"JCE_HEADLESS_BUILD_RESULT", result},
            {"JCE_HEADLESS_BUILD_CLEAN", "1"},
        };
        return env;
    }
};

} // namespace

TEST_CASE("headless build request is inactive when no contract variables exist")
{
    TestEnv env;
    JceEditorHeadlessBuildRequest request;
    CHECK(jce_editor_headless_build_request_parse(
              &request, lookup_env, &env) ==
          JceEditorHeadlessBuildRequestState::Inactive);
    CHECK(request.error.empty());
}

TEST_CASE("headless build request accepts a complete absolute project contract")
{
    Fixture fixture;
    TestEnv env = fixture.valid_env();
    JceEditorHeadlessBuildRequest request;

    CHECK(jce_editor_headless_build_request_parse(
              &request, lookup_env, &env) ==
          JceEditorHeadlessBuildRequestState::Valid);
    CHECK(request.project == fixture.project);
    CHECK(request.sdk == fixture.sdk);
    CHECK(request.target == "SpaceCodexDemo");
    CHECK(request.exe == "space_codex_demo.exe");
    CHECK(request.variant == "dist");
    CHECK(request.arch == "x86_64");
    CHECK(request.package_out == fixture.package);
    CHECK(request.result_path == fixture.result);
    CHECK(request.clean);
    CHECK(request.error.empty());
}

TEST_CASE("headless build request rejects partial and unsafe contracts")
{
    Fixture fixture;

    SUBCASE("partial request")
    {
        TestEnv env = fixture.valid_env();
        env.values.erase("JCE_HEADLESS_BUILD_RESULT");
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("JCE_HEADLESS_BUILD_RESULT") !=
              std::string::npos);
    }

    SUBCASE("relative project")
    {
        TestEnv env = fixture.valid_env();
        env.values["JCE_HEADLESS_BUILD_PROJECT"] = "space_codex";
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("absolute") != std::string::npos);
    }

    SUBCASE("target path injection")
    {
        TestEnv env = fixture.valid_env();
        env.values["JCE_HEADLESS_BUILD_TARGET"] = "../SpaceCodexDemo";
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("target") != std::string::npos);
    }

    SUBCASE("missing SDK package")
    {
        TestEnv env = fixture.valid_env();
        env.values["JCE_HEADLESS_BUILD_SDK"] = fixture.root + "/missing-sdk";
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("JCEConfig.cmake") != std::string::npos);
    }

    SUBCASE("result inside public package")
    {
        TestEnv env = fixture.valid_env();
        env.values["JCE_HEADLESS_BUILD_RESULT"] =
            fixture.package + "/private-result.json";
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("package") != std::string::npos);
    }

    SUBCASE("unsupported variant")
    {
        TestEnv env = fixture.valid_env();
        env.values["JCE_HEADLESS_BUILD_VARIANT"] = "profile";
        JceEditorHeadlessBuildRequest request;
        CHECK(jce_editor_headless_build_request_parse(
                  &request, lookup_env, &env) ==
              JceEditorHeadlessBuildRequestState::Invalid);
        CHECK(request.error.find("variant") != std::string::npos);
    }
}
