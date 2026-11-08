#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_headless_build_driver.h"
#include "core/jce_build_log.h"
#include "ui/jce_editor_panels.h"
#include JCE_TEST_BUILD_LOG_ACTUAL_HEADER

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_path.h>
}

#include <cstring>
#include <cstdlib>
#include <string>

namespace {

struct FakeHost {
    bool start_ok = true;
    bool quit_requested = false;
    int start_count = 0;
    int quit_count = 0;
    bool result_present_at_start = false;
    JceEditorHeadlessBuildDriver *driver = nullptr;
    JceAppResult application_result = JCE_APP_CONTINUE;
    JceBuildStatus status{};
    JceBuildLog log;
    void (*log_getter)(JceBuildLogView *) = nullptr;
    int log_count = 0;
};

bool fake_start(void *user,
                const JceEditorHeadlessBuildRequest *request,
                std::string *error)
{
    auto *host = static_cast<FakeHost *>(user);
    ++host->start_count;
    host->result_present_at_start = jce_fs_host_exists_file(request->result_path.c_str());
    if (!host->start_ok && error)
        *error = "synthetic start failure";
    return host->start_ok;
}

void fake_status(void *user, JceBuildStatus *out)
{
    *out = static_cast<FakeHost *>(user)->status;
}

void fake_log(void *user, JceBuildLogView *out)
{
    auto *host = static_cast<FakeHost *>(user);
    ++host->log_count;
    if (host->log_getter) host->log_getter(out);
    else *out = host->log.view();
}

void fake_quit(void *user)
{
    auto *host = static_cast<FakeHost *>(user);
    host->quit_requested = true;
    ++host->quit_count;
    if (host->driver)
        host->application_result = jce_editor_headless_build_driver_app_result(host->driver);
}

struct Fixture {
    std::string base;
    std::string root;
    std::string result;
    JceEditorHeadlessBuildRequest request;

    Fixture()
    {
        char cwd[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        char normalized[1024] = {}, unique[1024] = {};
        REQUIRE(jce_path_normalize(normalized, sizeof(normalized), cwd));
        base = normalized;
        REQUIRE(jce_fs_host_make_unique_path(
            (base + "/_ut_editor_headless_build_driver").c_str(), unique, sizeof(unique)));
        root = unique;
        result = root + "/result.json";
        REQUIRE_FALSE(jce_fs_host_exists_dir(root.c_str()));
        REQUIRE_FALSE(jce_fs_host_exists_file(root.c_str()));
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
        const bool owned = !base.empty() &&
            root.rfind(base + "/_ut_editor_headless_build_driver", 0) == 0 &&
            root.find('/', base.size() + 1) == std::string::npos;
        CHECK(owned);
        if (owned) CHECK(jce_fs_host_remove_recursive(root.c_str()));
    }
};

JceEditorHeadlessBuildDriverOps fake_ops()
{
    JceEditorHeadlessBuildDriverOps ops{};
    ops.start = fake_start;
    ops.get_status = fake_status;
    ops.get_log = fake_log;
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


namespace {
void stale_pass(const std::string &path)
{
    const char pass[] = "{\"schema\":\"jce.editor.headless-build-result.v1\","
                        "\"state\":\"succeeded\",\"exit_code\":0}";
    REQUIRE(jce_fs_host_write_all(path.c_str(), pass, sizeof(pass) - 1));
}
void require_failed_result(JceEditorHeadlessBuildDriver &driver, const FakeHost &host)
{
    JceBuildStatus status{};
    REQUIRE(jce_editor_headless_build_driver_get_result(&driver, &status));
    CHECK(status.state == JCE_BUILD_FAILED);
    CHECK(status.exit_code != 0);
    CHECK_FALSE(jce_editor_headless_build_driver_active(&driver));
    CHECK(host.quit_count == 1);
    CHECK(host.application_result == JCE_APP_FAILURE);
}
} // namespace

TEST_CASE("old PASS is removed before any build host starts")
{
    Fixture fixture;
    stale_pass(fixture.result);
    FakeHost host;
    host.status.state = JCE_BUILD_RUNNING;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    REQUIRE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    CHECK_FALSE(host.result_present_at_start);
    CHECK_FALSE(jce_fs_host_exists_file(fixture.result.c_str()));
    JceBuildStatus pending{};
    CHECK_FALSE(jce_editor_headless_build_driver_get_result(&driver, &pending));
    host.status.state = JCE_BUILD_SUCCEEDED;
    jce_editor_headless_build_driver_poll(&driver);
    CHECK(driver.result_published);
    CHECK(host.application_result == JCE_APP_SUCCESS);
    CHECK(host.quit_count == 1);
    jce_editor_headless_build_driver_poll(&driver);
    CHECK(host.quit_count == 1);
}
TEST_CASE("a receipt path that is a directory fails without deleting its contents")
{
    Fixture fixture;
    REQUIRE(jce_fs_host_create_directory(fixture.result.c_str()));
    stale_pass(fixture.result + "/keep.json");
    FakeHost host;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    CHECK_FALSE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    CHECK(host.start_count == 0);
    CHECK_FALSE(driver.result_published);
    require_failed_result(driver, host);
    CHECK(jce_fs_host_exists_file((fixture.result + "/keep.json").c_str()));
    CHECK(std::string(driver.result.last_error).find("result_preflight_reset") != std::string::npos);
}
TEST_CASE("real atomic-write failure cannot leave a stale PASS as current success")
{
    Fixture fixture;
    stale_pass(fixture.result);
    FakeHost host;
    host.status.state = JCE_BUILD_RUNNING;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    REQUIRE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    CHECK_FALSE(host.result_present_at_start);
    // A nonempty directory at the host helper's temp-file path causes a real
    // write failure. The test does not replace the writer with a bool stub.
    REQUIRE(jce_fs_host_create_directory((fixture.result + ".tmp").c_str()));
    stale_pass(fixture.result + ".tmp/keep.json");
    // Even a stale PASS reintroduced during the run must be invalidated.
    stale_pass(fixture.result);
    host.status.state = JCE_BUILD_SUCCEEDED;
    host.status.exit_code = 0;
    jce_editor_headless_build_driver_poll(&driver);
    require_failed_result(driver, host);
    CHECK_FALSE(driver.result_published);
    CHECK_FALSE(jce_fs_host_exists_file(fixture.result.c_str()));
    CHECK(jce_fs_host_exists_file((fixture.result + ".tmp/keep.json").c_str()));
    CHECK(std::string(driver.result.last_error).find("host_atomic_write") != std::string::npos);
    jce_editor_headless_build_driver_poll(&driver);
    CHECK(host.quit_count == 1);
}
TEST_CASE("a file blocking the receipt parent produces a traced terminal failure")
{
    Fixture fixture;
    const auto blocker = fixture.root + "/blocked-parent";
    stale_pass(blocker);
    fixture.request.result_path = blocker + "/result.json";
    FakeHost host;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    REQUIRE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    host.status.state = JCE_BUILD_SUCCEEDED;
    jce_editor_headless_build_driver_poll(&driver);
    require_failed_result(driver, host);
    CHECK_FALSE(driver.result_published);
    CHECK(jce_fs_host_exists_file(blocker.c_str()));
    CHECK_FALSE(jce_fs_host_exists_file(fixture.request.result_path.c_str()));
}
TEST_CASE("failed post-build audit with tool exit zero publishes an overall nonzero failure")
{
    Fixture fixture;
    FakeHost host;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    REQUIRE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    host.status.state = JCE_BUILD_FAILED;
    host.status.exit_code = 0;
    std::strcpy(host.status.last_error, "synthetic post-build audit failure");
    jce_editor_headless_build_driver_poll(&driver);
    require_failed_result(driver, host);
    CHECK(driver.result_published);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    CHECK(std::string(jce_json_get_string(result, "state", "")) == "failed");
    CHECK(jce_json_get_int(result, "exit_code", 0) != 0);
    jce_json_free(result);
}
TEST_CASE("synchronous startup failure replaces old PASS with this invocation failure")
{
    Fixture fixture;
    stale_pass(fixture.result);
    const std::string old_log_path = fixture.result + ".build.log";
    REQUIRE(jce_fs_host_write_all(old_log_path.c_str(), "PREVIOUS", 8));
    build_log_actual::g_build.log.reset();
    build_log_actual::log_line(JCE_CONSOLE_INFO, "previous accepted build");
    FakeHost host;
    host.start_ok = false;
    host.log_getter = build_log_actual::jce_build_manager_get_log;
    JceEditorHeadlessBuildDriver driver;
    host.driver = &driver;
    CHECK_FALSE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    require_failed_result(driver, host);
    CHECK_FALSE(host.result_present_at_start);
    CHECK(driver.result_published);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    CHECK(std::string(jce_json_get_string(result, "state", "")) == "failed");
    CHECK(std::string(jce_json_get_string(result, "error", "")) == "synthetic start failure");
    CHECK(host.log_count == 0);
    CHECK_FALSE(jce_json_get_bool(result, "log_complete", true));
    CHECK(std::string(jce_json_get_string(result, "log_status", "")) == "unavailable");
    CHECK(std::string(jce_json_get_string(result, "log_path", "not empty")).empty());
    CHECK(std::string(jce_json_get_string(result, "log_error", "")).find("not accepted") != std::string::npos);
    jce_json_free(result);
    uint64_t old_size = 0;
    auto *old_bytes = static_cast<char *>(jce_fs_host_read_all(old_log_path.c_str(), &old_size));
    REQUIRE(old_bytes != nullptr);
    CHECK(std::string(old_bytes, static_cast<size_t>(old_size)) == "PREVIOUS");
    jce_fs_buffer_free(old_bytes);
}


namespace {
void finish_failed_build(Fixture &fixture, FakeHost &host, JceEditorHeadlessBuildDriver &driver)
{
    REQUIRE(jce_editor_headless_build_driver_start(&driver, &fixture.request, fake_ops(), &host));
    host.status.state = JCE_BUILD_FAILED; host.status.exit_code = 17;
    std::strcpy(host.status.last_error, "original compiler failure");
    jce_editor_headless_build_driver_poll(&driver);
}
std::string read_host_text(const std::string &path)
{
    uint64_t size = 0;
    auto *raw = static_cast<char *>(jce_fs_host_read_all(path.c_str(), &size));
    REQUIRE(raw != nullptr);
    std::string text(raw, static_cast<size_t>(size));
    jce_fs_buffer_free(raw);
    return text;
}
void check_original_failure(JceJson *result)
{
    CHECK(jce_json_get_int(result, "exit_code", 0) == 17);
    CHECK(std::string(jce_json_get_string(result, "state", "")) == "failed");
    CHECK(std::string(jce_json_get_string(result, "error", "")) == "original compiler failure");
}
}

TEST_CASE("actual Build log capture reaches the terminal receipt")
{
    Fixture fixture;
    const char *old = std::getenv("JCE_TEST_BUILD_LOG_OLD_WIRE");
    const bool old_wire = old && std::strcmp(old, "1") == 0;
    auto &log = old_wire ? build_log_old_wire::g_build.log : build_log_actual::g_build.log;
    log.reset();
    if (old_wire) build_log_old_wire::log_line(JCE_CONSOLE_ERROR, "CMake Error: owner witness");
    else build_log_actual::log_line(JCE_CONSOLE_ERROR, "CMake Error: owner witness");
    FakeHost host;
    host.log_getter = old_wire ? build_log_old_wire::jce_build_manager_get_log :
                                build_log_actual::jce_build_manager_get_log;
    JceEditorHeadlessBuildDriver driver;
    finish_failed_build(fixture, host, driver);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    // The optional same-binary old-wire mode must fail these ordinary doctest
    // assertions, never count an empty transcript as complete evidence.
    CHECK(jce_json_get_bool(result, "log_complete", false));
    const std::string path = jce_json_get_string(result, "log_path", "");
    CHECK(path == fixture.result + ".build.log");
    CHECK(jce_json_get_int(result, "log_bytes", 0) > 0);
    check_original_failure(result);
    jce_json_free(result);
    if (!path.empty()) CHECK(read_host_text(path) == "CMake Error: owner witness\n");
    jce_editor_headless_build_driver_poll(&driver);
    CHECK(host.log_count == 1); CHECK(host.quit_count == 1);
}

TEST_CASE("removed actual Build capture cannot certify complete log evidence")
{
    Fixture fixture;
    build_log_old_wire::g_build.log.reset();
    build_log_old_wire::log_line(JCE_CONSOLE_ERROR, "CMake Error: must survive");
    FakeHost host;
    host.log_getter = build_log_old_wire::jce_build_manager_get_log;
    JceEditorHeadlessBuildDriver driver;
    finish_failed_build(fixture, host, driver);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    CHECK_FALSE(jce_json_get_bool(result, "log_complete", true));
    CHECK(std::string(jce_json_get_string(result, "log_status", "")) == "unavailable");
    CHECK(std::string(jce_json_get_string(result, "log_path", "not empty")).empty());
    check_original_failure(result);
    jce_json_free(result);
}

TEST_CASE("real log atomic-write failure preserves old evidence and the build result")
{
    Fixture fixture;
    const auto path = fixture.result + ".build.log";
    REQUIRE(jce_fs_host_write_all(path.c_str(), "OLD", 3));
    REQUIRE(jce_fs_host_create_directory((path + ".tmp").c_str()));
    REQUIRE(jce_fs_host_write_all((path + ".tmp/keep").c_str(), "KEEP", 4));
    FakeHost host; host.log.reset(); host.log.append_line("new compiler failure");
    JceEditorHeadlessBuildDriver driver;
    finish_failed_build(fixture, host, driver);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    CHECK(std::string(jce_json_get_string(result, "log_status", "")) == "write_failed");
    CHECK(std::string(jce_json_get_string(result, "log_path", "not empty")).empty());
    CHECK_FALSE(jce_json_get_bool(result, "log_complete", true));
    check_original_failure(result);
    jce_json_free(result);
    CHECK(read_host_text(path) == "OLD");
    CHECK(jce_fs_host_exists_file((path + ".tmp/keep").c_str()));
}

TEST_CASE("bounded build transcript cannot certify a truncated capture")
{
    Fixture fixture;
    FakeHost host; host.log.reset();
    host.log.append_line(std::string(JCE_BUILD_LOG_CAPACITY + 10u, 'x'));
    REQUIRE(host.log.text.size() == JCE_BUILD_LOG_CAPACITY); REQUIRE(host.log.truncated);
    JceEditorHeadlessBuildDriver driver;
    finish_failed_build(fixture, host, driver);
    JceJson *result = jce_json_parse_file(fixture.result.c_str());
    REQUIRE(result != nullptr);
    CHECK(jce_json_get_int(result, "log_bytes", 0) == JCE_BUILD_LOG_CAPACITY);
    CHECK(jce_json_get_bool(result, "log_truncated", false));
    CHECK_FALSE(jce_json_get_bool(result, "log_complete", true));
    CHECK(read_host_text(jce_json_get_string(result, "log_path", "")).size() == JCE_BUILD_LOG_CAPACITY);
    check_original_failure(result);
    jce_json_free(result);
}

TEST_CASE("actual pipe EOF and oversized partial lines remain explicitly incomplete")
{
    build_log_actual::g_build.log.reset();
    std::string partial;
    build_log_actual::emit_lines(partial, "last error", 10, JCE_CONSOLE_ERROR);
    CHECK_FALSE(build_log_actual::g_build.log.incomplete);
    build_log_actual::flush_partial(partial, JCE_CONSOLE_ERROR);
    CHECK(build_log_actual::g_build.log.incomplete); CHECK(partial.empty());
    CHECK(build_log_actual::g_build.log.text == "[build] [stderr] last error\n");
    {
        Fixture fixture;
        FakeHost host; host.log_getter = build_log_actual::jce_build_manager_get_log;
        JceEditorHeadlessBuildDriver driver;
        finish_failed_build(fixture, host, driver);
        JceJson *result = jce_json_parse_file(fixture.result.c_str());
        REQUIRE(result != nullptr);
        CHECK(jce_json_get_bool(result, "log_incomplete", false));
        CHECK_FALSE(jce_json_get_bool(result, "log_complete", true));
        check_original_failure(result); jce_json_free(result);
    }
    build_log_actual::g_build.log.reset();
    const std::string huge(JCE_BUILD_LOG_CAPACITY + 9u, 'x');
    build_log_actual::emit_lines(partial, huge.data(), huge.size(), JCE_CONSOLE_INFO);
    CHECK(partial.size() == 9); CHECK(build_log_actual::g_build.log.incomplete);
    CHECK(build_log_actual::g_build.log.truncated);
    CHECK(build_log_actual::g_build.log.text.size() <= JCE_BUILD_LOG_CAPACITY);
}

TEST_CASE("actual Build worker redirect stays off Console and retains bounded replay")
{
    build_log_actual::g_build.log.reset(); build_log_actual::console_count = 0;
    build_log_actual::WorkerLogSink sink;
    build_log_actual::t_log_sink = &sink;
    build_log_actual::log_line(JCE_CONSOLE_ERROR, "worker error");
    build_log_actual::t_log_sink = nullptr;
    CHECK(build_log_actual::console_count == 0); CHECK(build_log_actual::g_build.log.text.empty());
    REQUIRE(sink.entries.size() == 1);
    build_log_actual::log_line(sink.entries[0].level, sink.entries[0].text);
    CHECK(build_log_actual::console_count == 1); CHECK(build_log_actual::g_build.log.text == "worker error\n");
    sink.reset();
    for (int i = 0; i < 4097; ++i) sink.append(JCE_CONSOLE_INFO, "x");
    CHECK(sink.entries.size() == 4096); CHECK(sink.truncated);
    CHECK(sink.retained_bytes <= JCE_BUILD_LOG_CAPACITY);
}
