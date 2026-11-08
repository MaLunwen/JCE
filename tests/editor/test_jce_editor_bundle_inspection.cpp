#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "jce_test_owned_directory.h"
#ifndef JCE_TEST_BUNDLE_INSPECTION_OWNER
#error "Bundle inspection fixture requires the actual private owner"
#endif
#ifndef JCE_TEST_BUNDLE_INSPECTION_PREFIX
#error "Bundle inspection fixture requires a per-target directory namespace"
#endif
#include JCE_TEST_BUNDLE_INSPECTION_OWNER
#include <jce/resource/jce_bundle_pack.h>
#include <jce/os/core/jce_json.h>
#include <cstring>

namespace {
struct BundleFixture {
    JceTestOwnedDirectory directory{JCE_TEST_BUNDLE_INSPECTION_PREFIX};
    std::string path;
    BundleFixture() {
        const std::string scene = directory.path("base.scene");
        const char text[] = "{\"entities\":[]}";
        REQUIRE(jce_fs_host_write_all(scene.c_str(), text, sizeof(text)-1));
        const char *scenes[] = {scene.c_str()};
        JceBundlePackOptions options{};
        options.scenes_dir = directory.root.c_str();
        options.resource_root = directory.root.c_str();
        options.out_dir = directory.root.c_str();
        options.scene_files = scenes; options.scene_file_count = 1;
        options.single_file_mode = true; options.single_bundle_id = "inspection_test";
        options.zstd_level = 1; options.cook_assets = true;
        options.target_platform = 5; // The actual Bundler Web target, not a policy mock.
        REQUIRE(jce_bundle_pack_run(&options, nullptr, nullptr) == 0);
        path = directory.path("inspection_test.jbundle");
        REQUIRE(jce_fs_host_exists_file(path.c_str()));
    }
    JceEditorBundleInspectionRequest request(bool snapshot) const {
        JceEditorBundleInspectionRequest request;
        request.path = path; request.target = "web"; request.use_snapshot = snapshot;
        return request;
    }
};
JceAsyncState run(JceEditorBundleInspectionRequest &request, bool cancel=false) {
    JceAsyncExecutorConfig config;
    jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_COOPERATIVE;
    JceAsyncExecutor *executor = jce_async_executor_create(&config);
    REQUIRE(executor != nullptr);
    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = jce_editor_bundle_inspection_work; desc.user_data = &request;
    JceAsyncTask *task = jce_async_submit(executor, &desc);
    REQUIRE(task != nullptr);
    if (cancel) CHECK(jce_async_task_cancel(task));
    for (unsigned i=0; i<16 && !jce_async_task_is_terminal(task); ++i)
        jce_async_executor_pump(executor, nullptr);
    const JceAsyncState state = jce_async_task_state(task);
    CHECK(jce_async_task_is_terminal(task));
    jce_async_task_release(task);
    jce_async_executor_destroy(executor);
    return state;
}
}
TEST_CASE("physical inspection uses actual target/sidecar owner and stable summary") {
    BundleFixture fixture; auto request = fixture.request(false);
    CHECK(run(request) == JCE_ASYNC_STATE_SUCCEEDED);
    CHECK(request.accepted); CHECK_FALSE(request.summary.snapshot);
    CHECK(request.summary.id == "inspection_test");
    CHECK(request.summary.kind == "scene"); CHECK(request.summary.scene == "base.scene");
    CHECK(request.summary.entries >= 2);
}
TEST_CASE("file inspection refuses explicit wrong target and clears stale summary") {
    BundleFixture fixture; auto request = fixture.request(false);
    request.target = "windows"; request.summary.id = "stale";
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED);
    CHECK_FALSE(request.accepted); CHECK(request.summary.id.empty());
}
TEST_CASE("available malformed sidecar refuses file acceptance; absent sidecar remains legal") {
    BundleFixture fixture; auto request = fixture.request(false);
    const std::string sidecar = fixture.path + ".json";
    const char malformed[] = "{}";
    REQUIRE(jce_fs_host_write_all(sidecar.c_str(), malformed, sizeof(malformed)-1));
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED); CHECK_FALSE(request.accepted);
    REQUIRE(jce_fs_host_remove_file(sidecar.c_str()));
    CHECK(run(request) == JCE_ASYNC_STATE_SUCCEEDED);
}
TEST_CASE("snapshot captures owned bytes once and remains independent of removed disk file") {
    BundleFixture fixture; auto request = fixture.request(true);
    CHECK(run(request) == JCE_ASYNC_STATE_SUCCEEDED); REQUIRE(request.bytes != nullptr);
    const void *identity = request.bytes.get(); const uint64_t hash = request.summary.snapshot_content_id;
    REQUIRE(request.summary.snapshot); CHECK(request.summary.snapshot_bytes == request.byte_count);
    REQUIRE(jce_fs_host_remove_file(fixture.path.c_str()));
    CHECK(run(request) == JCE_ASYNC_STATE_SUCCEEDED);
    CHECK(request.bytes.get() == identity); CHECK(request.summary.snapshot_content_id == hash);
    CHECK(request.summary.id == "inspection_test");
}
TEST_CASE("cached memory inspection preserves explicit target instead of file or previous acceptance") {
    BundleFixture fixture; auto request = fixture.request(true);
    REQUIRE(run(request) == JCE_ASYNC_STATE_SUCCEEDED);
    request.target = "windows";
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED);
    CHECK_FALSE(request.accepted); CHECK(request.summary.id.empty());
    request.target = "auto";
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED); // Auto is portable-only, not any-target.
}
TEST_CASE("snapshot cap refuses partial new bytes and oversized cached bytes") {
    BundleFixture fixture; auto request = fixture.request(true);
    request.snapshot_limit = 16;
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED); CHECK_FALSE(request.bytes);
    request.snapshot_limit = 64u*1024u*1024u;
    REQUIRE(run(request) == JCE_ASYNC_STATE_SUCCEEDED);
    request.snapshot_limit = 16;
    CHECK(run(request) == JCE_ASYNC_STATE_FAILED); CHECK_FALSE(request.accepted);
}
TEST_CASE("inspection leaves the active scene filesystem and mount table untouched") {
    BundleFixture fixture;
    JceFileSystem *previous = jce_fs_get_active();
    const JceFsActivePolicy policy = jce_fs_get_active_policy();
    JceFileSystem *active = jce_fs_create(); REQUIRE(active != nullptr);
    jce_fs_set_active(active);
    auto file = fixture.request(false); auto memory = fixture.request(true);
    CHECK(run(file) == JCE_ASYNC_STATE_SUCCEEDED);
    CHECK(run(memory) == JCE_ASYNC_STATE_SUCCEEDED);
    CHECK(jce_fs_get_active() == active); CHECK(jce_fs_mounted_pak_count(active) == 0);
    jce_fs_set_active_policy(previous, policy); jce_fs_destroy(active);
}
TEST_CASE("cancelled pending inspection cannot publish accepted content") {
    BundleFixture fixture; auto request = fixture.request(true);
    CHECK(run(request, true) == JCE_ASYNC_STATE_CANCELLED);
    CHECK_FALSE(request.accepted); CHECK_FALSE(request.bytes);
}
TEST_CASE("exact target tokens and request generation refuse stale A-to-B-to-A completion") {
    const char *tokens[] = {"windows","linux","macos","android","ios","web","auto"};
    for (int i=0; i<7; ++i) CHECK(std::strcmp(jce_editor_bundle_inspection_target(i),tokens[i]) == 0);
    CHECK(jce_editor_bundle_inspection_target(-1) == nullptr);
    CHECK(jce_editor_bundle_inspection_target(7) == nullptr);
    JceEditorBundleInspectionRequest request; request.path="A";request.target="web";request.generation=1;
    CHECK(jce_editor_bundle_inspection_matches(request,"A","web",1));
    CHECK_FALSE(jce_editor_bundle_inspection_matches(request,"B","web",1));
    CHECK_FALSE(jce_editor_bundle_inspection_matches(request,"A","windows",1));
    CHECK_FALSE(jce_editor_bundle_inspection_matches(request,"A","web",3));
}
