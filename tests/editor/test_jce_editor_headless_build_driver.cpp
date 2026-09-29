#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_headless_build_driver.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
}

#include <cstring>
#include <string>

namespace {

struct FakeHost {
    bool start_ok = true;
    bool quit_requested = false;
    int start_count = 0;
    JceBuildStatus status{};
};

bool fake_start(void *user,
                const JceEditorHeadlessBuildRequest *,
                std::string *error)
{
    auto *host = static_cast<FakeHost *>(user);
    ++host->start_count;
    if (!host->start_ok && error)
        *error = "synthetic start failure";
    return host->start_ok;
}

void fake_status(void *user, JceBuildStatus *out)
{
    *out = static_cast<FakeHost *>(user)->status;
}

void fake_quit(void *user)
{
    static_cast<FakeHost *>(user)->quit_requested = true;
}

struct Fixture {
    std::string root;
    std::string result;
    JceEditorHeadlessBuildRequest request;

    Fixture()
    {
        char cwd[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        root = std::string(cwd) + "/_ut_editor_headless_build_driver";
        result = root + "/result.json";
        (void)jce_fs_host_remove_recursive(root.c_str());
        REQUIRE(jce_fs_host_create_directory(root.c_str()));
        request.project = root + "/project";
        request.sdk = root + "/sdk";
        request.target = "SpaceCodexDemo";
        request.exe = "space_codex_demo.exe";
        request.variant = "dist";
        request.arch = "x86_64";
        request.package_out = root + "/package";
        request.result_path = result;
        request.clean = true;
    }

    ~Fixture()
    {
        (void)jce_fs_host_remove_recursive(root.c_str());
    }
};

JceEditorHeadlessBuildDriverOps fake_ops()
{
    JceEditorHeadlessBuildDriverOps ops{};
    ops.start = fake_start;
    ops.get_status = fake_status;
    ops.request_quit = fake_quit;
    return ops;
}

} // namespace

TEST_CASE("headless build driver writes successful authoritative outputs")
{
    Fixture fixture;
    FakeHost host;
    host.status.state = JCE_BUILD_RUNNING;
    host.status.stage = JCE_BUILD_STAGE_PREPARE_ASSETS;

    JceEditorHeadlessBuildDriver driver;
    CHECK(jce_editor_headless_build_driver_start(
        &driver, &fixture.request, fake_ops(), &host));
    CHECK(host.start_count == 1);

    jce_editor_headless_build_driver_poll(&driver);
    CHECK_FALSE(host.quit_requested);
    CHECK_FALSE(jce_fs_host_exists_file(fixture.result.c_str()));

    host.status.state = JCE_BUILD_SUCCEEDED;
    host.status.stage = JCE_BUILD_STAGE_COMPILE;
    host.status.exit_code = 0;
    std::strcpy(host.status.artifact_path, "private/build/space_codex_demo.exe");
    std::strcpy(host.status.package_path, "public/package");
    std::strcpy(host.status.asset_bom_path, "private/reports/assets.bom.json");
    std::strcpy(host.status.dist_audit_path, "private/reports/dist_audit.json");

    jce_editor_headless_build_driver_poll(&driver);
    CHECK(host.quit_requested);
    CHECK_FALSE(jce_editor_headless_build_driver_active(&driver));

    JceJson *root = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(root != nullptr);
    CHECK(std::string(jce_json_get_string(root, "schema", "")) ==
          "jce.editor.headless-build-result.v1");
    CHECK(std::string(jce_json_get_string(root, "state", "")) ==
          "succeeded");
    CHECK(std::string(jce_json_get_string(root, "artifact_path", "")) ==
          "private/build/space_codex_demo.exe");
    CHECK(std::string(jce_json_get_string(root, "package_path", "")) ==
          "public/package");
    CHECK(std::string(jce_json_get_string(root, "asset_bom_path", "")) ==
          "private/reports/assets.bom.json");
    CHECK(std::string(jce_json_get_string(root, "dist_audit_path", "")) ==
          "private/reports/dist_audit.json");
    jce_json_free(root);
}

TEST_CASE("headless build driver records a synchronous start failure")
{
    Fixture fixture;
    FakeHost host;
    host.start_ok = false;

    JceEditorHeadlessBuildDriver driver;
    CHECK_FALSE(jce_editor_headless_build_driver_start(
        &driver, &fixture.request, fake_ops(), &host));
    CHECK(host.quit_requested);
    CHECK_FALSE(jce_editor_headless_build_driver_active(&driver));

    JceJson *root = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(root != nullptr);
    CHECK(std::string(jce_json_get_string(root, "state", "")) == "failed");
    CHECK(std::string(jce_json_get_string(root, "error", "")) ==
          "synthetic start failure");
    jce_json_free(root);
}
