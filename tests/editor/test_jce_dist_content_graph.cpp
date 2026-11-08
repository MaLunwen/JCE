#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/jce_dist_content_graph.h"
#include "core/jce_dist_audit.h"
#include "core/jce_dist_name_policy.h"
#include "core/jce_project_prepared_assets.h"
#include "ui/jce_editor_panels.h"
#include "jce_test_owned_directory.h"
#ifndef JCE_TEST_DIST_FIXTURE_PREFIX
#error "Dist graph fixture requires its target-owned path prefix"
#endif

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_bundle_pack.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

size_t adapter_owner_logs = 0;
void log_line(JceConsoleLevel, const std::string &) { ++adapter_owner_logs; }
void dist_content_graph_log(int, const char *, void *) {}
#include JCE_TEST_DIST_MANAGER_ADAPTER

bool write_text(const std::string &path, const char *text)
{
    return jce_fs_host_write_all(path.c_str(), text,
                                 (uint64_t)std::strlen(text));
}

const JceDistContentGraphAsset *find_asset(
    const JceDistContentGraph &graph, const char *address)
{
    auto it = std::find_if(
        graph.assets.begin(), graph.assets.end(),
        [address](const JceDistContentGraphAsset &asset) {
            return asset.address == address;
        });
    return it == graph.assets.end() ? nullptr : &*it;
}

std::vector<uint8_t> make_gui_pe()
{
    std::vector<uint8_t> bytes(512, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3c] = 0x80;
    bytes[0x80] = 'P';
    bytes[0x81] = 'E';
    bytes[0x84] = 0x64;
    bytes[0x85] = 0x86;
    bytes[0x94] = 0xf0;
    bytes[0x98] = 0x0b;
    bytes[0x99] = 0x02;
    bytes[0xdc] = 2;
    return bytes;
}

} // namespace

TEST_CASE("Dist content is extracted from the Bundle dependency graph")
{
    JceTestOwnedDirectory owned{JCE_TEST_DIST_FIXTURE_PREFIX "_content"};
    const std::string &root = owned.root;
    REQUIRE(jce_fs_host_create_directory((root + "/resources/assets/scenes").c_str()));
    REQUIRE(jce_fs_host_create_directory((root + "/resources/assets/scripts").c_str()));
    REQUIRE(jce_fs_host_create_directory((root + "/resources/assets/data").c_str()));
    REQUIRE(jce_fs_host_create_directory((root + "/generated").c_str()));
    REQUIRE(jce_fs_host_create_directory((root + "/reports").c_str()));

    REQUIRE(write_text(root + "/jce_project.json",
        "{\"schema\":2,\"name\":\"graph_test\","
        "\"source_assets\":\"resources/assets\","
        "\"startup_scene\":\"scenes/main.scene\"}"));
    REQUIRE(write_text(root + "/resources/assets/scenes/main.scene",
        "{\"entities\":[{\"scriptPath\":\"scripts/main.lua\"}]}"));
    REQUIRE(write_text(root + "/resources/assets/scripts/main.lua",
        "return true\n"));
    REQUIRE(write_text(root + "/resources/assets/scripts/main.lua.deps.json",
        "{\"contract\":{\"name\":\"jce.bundle.dependencies\","
        "\"major\":1,\"minor\":0},"
        "\"assets\":[\"data/dynamic.bin\"]}"));
    REQUIRE(write_text(root + "/resources/assets/bundle_roots.json",
        "{\"contract\":{\"name\":\"jce.bundle.dependencies\","
        "\"major\":1,\"minor\":0},"
        "\"assets\":[\"data/global.bin\"]}"));
    REQUIRE(write_text(root + "/resources/assets/data/dynamic.bin", "dynamic"));
    REQUIRE(write_text(root + "/resources/assets/data/global.bin", "global"));

    JceDistContentGraph graph;
    std::string error;
    REQUIRE(jce_dist_content_graph_build(
        root, root + "/generated", root + "/reports",
        jce_dist_content_graph_host_platform(), nullptr, nullptr,
        &graph, &error));
    CHECK(error.empty());
    CHECK(jce_fs_host_exists_file(graph.snapshot_path.c_str()));
    CHECK(find_asset(graph, "scenes/main.scene") != nullptr);
    CHECK(find_asset(graph, "scripts/main.lua") != nullptr);
    CHECK(find_asset(graph, "data/dynamic.bin") != nullptr);
    CHECK(find_asset(graph, "data/global.bin") != nullptr);
    CHECK(find_asset(graph, "scripts/main.lua.deps.json") == nullptr);

    const JceDistContentGraphAsset *dynamic = find_asset(
        graph, "data/dynamic.bin");
    REQUIRE(dynamic != nullptr);
    CHECK(dynamic->content_id == jce_archive_content_hash(
        dynamic->bytes.data(), dynamic->bytes.size()));

}

namespace {

struct StockGraphFixture {
    JceTestOwnedDirectory owned{JCE_TEST_DIST_FIXTURE_PREFIX "_stock"};
    std::string project = owned.path("project");
    std::string sdk = owned.path("sdk");
    std::string resources = project + "/resources";
    StockGraphFixture()
    {
        // Real host files and the public Bundle/Archive owner; no shader/GPU parser claim.
        REQUIRE(jce_fs_get_active() == nullptr);
        REQUIRE(jce_fs_host_create_directory((resources + "/scenes").c_str()));
        REQUIRE(jce_fs_host_create_directory((resources + "/materials").c_str()));
        REQUIRE(jce_fs_host_create_directory((sdk + "/share/jce/shaders").c_str()));
        REQUIRE(write_text(project + "/jce_project.json",
            "{\"schema\":2,\"name\":\"stock_graph_test\","
            "\"source_assets\":\"resources\",\"startup_scene\":\"scenes/main.scene\"}"));
        REQUIRE(write_text(resources + "/scenes/main.scene",
            "{\"entities\":[{\"materialPath\":\"materials/stock.mat.json\"}]}"));
        REQUIRE(write_text(resources + "/materials/stock.mat.json",
            "{\"customProgramVs\":\"shaders/vs_stock_probe.bin\","
            "\"customProgramFs\":\"shaders/fs_stock_probe.bin\"}"));
    }
    void write_manifest(const std::string &member)
    {
        const std::string text = "{\"schema\":2,\"name\":\"stock_graph_test\","
            "\"source_assets\":\"resources\",\"startup_scene\":\"scenes/main.scene\"" +
            (member.empty() ? "" : "," + member) + "}";
        REQUIRE(write_text(project + "/jce_project.json", text.c_str()));
    }
    void write_stock()
    {
        REQUIRE(write_text(sdk + "/share/jce/shaders/vs_stock_probe_dx11.bin", "stock-vs"));
        REQUIRE(write_text(sdk + "/share/jce/shaders/fs_stock_probe_dx11.bin", "stock-fs"));
    }
    bool build(JceDistContentGraph *graph, std::string *error,
               JceDistContentGraphLogFn log = nullptr, void *user = nullptr)
    {
        return jce_dist_content_graph_build(project, owned.path("generated"),
            // Fixed Windows-profile byte samples, independent of the test host.
            owned.path("reports"), 0, log, user, graph, error, sdk);
    }
};

void capture_first_graph_error(int level, const char *message, void *user)
{
    // JCE_BUNDLE_PACK_LOG_ERROR is public; retain the actual producer message.
    auto *first = static_cast<std::string *>(user);
    if (level == JCE_BUNDLE_PACK_LOG_ERROR && first->empty())
        *first = message ? message : "";
}

void capture_first_pack_error(JceBundlePackLogLevel level,
                               const char *message, void *user)
{
    capture_first_graph_error(static_cast<int>(level), message, user);
}

} // namespace

TEST_CASE("Dist graph resolves SDK stock shader alternatives without an active VFS")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    JceDistContentGraph graph;
    std::string error;
    REQUIRE(fixture.build(&graph, &error));
    CHECK(error.empty());
    const auto *vs = find_asset(graph, "shaders/vs_stock_probe_dx11.bin");
    const auto *fs = find_asset(graph, "shaders/fs_stock_probe_dx11.bin");
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    CHECK(std::string(vs->bytes.begin(), vs->bytes.end()) == "stock-vs");
    CHECK(std::string(fs->bytes.begin(), fs->bytes.end()) == "stock-fs");
    CHECK(jce_fs_get_active() == nullptr);
}

TEST_CASE("Dist graph resolves a real relative SDK root through the host path facade")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    char relative_sdk[1024]{};
    REQUIRE(jce_path_relative(relative_sdk, sizeof(relative_sdk),
                              fixture.sdk.c_str(), fixture.owned.base.c_str()));
    REQUIRE_FALSE(jce_path_is_absolute(relative_sdk));
    fixture.sdk = relative_sdk;
    JceDistContentGraph graph;
    std::string error;
    REQUIRE(fixture.build(&graph, &error));
    CHECK(error.empty());
    const auto *vs = find_asset(graph, "shaders/vs_stock_probe_dx11.bin");
    REQUIRE(vs != nullptr);
    CHECK(std::string(vs->bytes.begin(), vs->bytes.end()) == "stock-vs");
    CHECK(jce_fs_get_active() == nullptr);
}

TEST_CASE("Dist graph preserves project shader bytes ahead of the SDK fallback")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    REQUIRE(jce_fs_host_create_directory((fixture.resources + "/shaders").c_str()));
    REQUIRE(write_text(fixture.resources + "/shaders/vs_stock_probe_dx11.bin", "project-vs"));
    JceDistContentGraph graph;
    std::string error;
    REQUIRE(fixture.build(&graph, &error));
    const auto *vs = find_asset(graph, "shaders/vs_stock_probe_dx11.bin");
    REQUIRE(vs != nullptr);
    CHECK(std::string(vs->bytes.begin(), vs->bytes.end()) == "project-vs");
    CHECK(vs->content_id == jce_archive_content_hash("project-vs", 10));
}

TEST_CASE("Dist graph missing SDK stock retains the first real dependency error")
{
    StockGraphFixture fixture;
    JceDistContentGraph graph;
    std::string error, first_error;
    REQUIRE_FALSE(fixture.build(&graph, &error, capture_first_graph_error, &first_error));
    REQUIRE_FALSE(first_error.empty());
    CHECK(first_error.find("missing asset shaders/") != std::string::npos);
    CHECK(first_error.find("dependency chain:") != std::string::npos);
    CHECK(error == "Dist content graph build failed: " + first_error);
    CHECK(graph.assets.empty());
    CHECK_FALSE(jce_fs_host_exists_file(fixture.owned.path("reports/project_content_graph_dist.json").c_str()));
}

TEST_CASE("Dist confidential names are optional and exact selected graph members")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    JceDistContentGraph graph;
    std::string error;
    REQUIRE(fixture.build(&graph, &error));
    CHECK(graph.confidential_paths.empty());
    fixture.write_manifest("\"confidential_asset_paths\":[\"materials/stock.mat.json\"]");
    REQUIRE(fixture.build(&graph, &error));
    REQUIRE(graph.confidential_paths.size() == 1);
    CHECK(graph.confidential_paths[0] == "materials/stock.mat.json");

    REQUIRE(jce_fs_host_create_directory((fixture.resources + "/data/v1..x").c_str()));
    REQUIRE(write_text(fixture.resources + "/data/v1..x/keep.bin", "kept"));
    REQUIRE(write_text(fixture.resources + "/bundle_roots.json",
        "{\"contract\":{\"name\":\"jce.bundle.dependencies\",\"major\":1,\"minor\":0},"
        "\"assets\":[\"data/v1..x/keep.bin\"]}"));
    fixture.write_manifest("\"confidential_asset_paths\":[\"data/v1..x/keep.bin\"],"
        "\"notes\":\"\\\\u0000\"");
    REQUIRE(fixture.build(&graph, &error));
    REQUIRE(graph.confidential_paths.size() == 1);
    CHECK(graph.confidential_paths[0] == "data/v1..x/keep.bin");
}

TEST_CASE("Dist name parser accepts a canonical segment ending in dots")
{
    StockGraphFixture fixture;
    fixture.write_manifest("\"confidential_asset_paths\":[\"data/v1../keep.bin\"]");
    std::vector<std::string> paths;
    std::string error;
    // Real manifest IO/parser/Archive normalization only: no host directory
    // named v1.. is created, and this does not certify selected-graph membership.
    REQUIRE(jce_dist_name_policy_load(fixture.project + "/jce_project.json",
                                      &paths, &error));
    CHECK(error.empty());
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == "data/v1../keep.bin");
}

TEST_CASE("Dist rejects malformed or unselected confidential name declarations")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    // Even a real source file outside the selected closure is inadmissible.
    REQUIRE(write_text(fixture.resources + "/unused.bin", "unused"));
    const char *bad[] = {
        "\"confidential_asset_paths\":\"materials/stock.mat.json\"",
        "\"confidential_asset_paths\":[7]",
        "\"confidential_asset_paths\":[\"\"]",
        "\"confidential_asset_paths\":[\"Materials/stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"materials\\\\stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"materials/./stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"materials/../materials/stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"/materials/stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"C:/materials/stock.mat.json\"]",
        "\"confidential_asset_paths\":[\"materials/*.json\"]",
        "\"confidential_asset_paths\":[\"materials/stock.mat.json\",\"materials/stock.mat.json\"]",
        "\"confidential_asset_paths\":[],\"confidential_asset_paths\":[]",
        "\"confidential_asset_paths\":[\"materials/stock.mat.json\\u0000suffix\"]",
        "\"confidential_asset_paths\":[\"unused.bin\"]"
    };
    for (const char *member : bad) {
        CAPTURE(member);
        fixture.write_manifest(member);
        JceDistContentGraph graph;
        std::string error;
        CHECK_FALSE(fixture.build(&graph, &error));
        CHECK(error.find("confidential_asset_paths:") == 0);
        CHECK(graph.assets.empty());
        CHECK(graph.confidential_paths.empty());
    }
}

TEST_CASE("Dist confidential name policy enforces bounded manifest and address budgets")
{
    StockGraphFixture fixture;
    const auto rejected = [&](const std::string &member, const char *reason) {
        fixture.write_manifest(member);
        JceDistContentGraph graph;
        std::string error;
        CHECK_FALSE(fixture.build(&graph, &error));
        CHECK(error.find(reason) != std::string::npos);
        CHECK(graph.assets.empty());
        CHECK(graph.confidential_paths.empty());
    };
    std::string member = "\"confidential_asset_paths\":[";
    for (int i = 0; i < JCE_DIST_CONFIDENTIAL_COUNT_CAP + 1; ++i)
        member += (i ? "," : "") + std::string("\"materials/stock.mat.json\"");
    rejected(member + "]", "too many confidential addresses");
    rejected("\"confidential_asset_paths\":[\"" +
        std::string(JCE_DIST_CONFIDENTIAL_PATH_CAP + 1, 'a') + "\"]", "path byte budget");
    member = "\"confidential_asset_paths\":[";
    for (int i = 0; i < 65; ++i)
        member += (i ? "," : "") + std::string("\"") +
            std::string(JCE_DIST_CONFIDENTIAL_PATH_CAP, 'a') + "\"";
    rejected(member + "]", "path byte budget");
    rejected("\"notes\":\"" + std::string(JCE_DIST_POLICY_MANIFEST_CAP, 'a') + "\"", "1 MiB");
}

TEST_CASE("actual Dist manager adapter separates secure payloads from confidential names")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    // Supply all public backend alternatives; the actual manager selects its host.
    for (const char *backend : {"spv", "glsl", "essl", "essl1", "mtl"}) {
        REQUIRE(write_text(fixture.sdk + "/share/jce/shaders/vs_stock_probe_" + backend + ".bin", "stock-vs"));
        REQUIRE(write_text(fixture.sdk + "/share/jce/shaders/fs_stock_probe_" + backend + ".bin", "stock-fs"));
    }
    static const char consumer_literal[] = "materials/stock.mat.json";
    uint8_t key[32];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i * 13u + 5u);
    for (bool confidential : {false, true}) {
        fixture.write_manifest(confidential
            ? "\"confidential_asset_paths\":[\"materials/stock.mat.json\"]" : "");
        std::vector<ProjectAssetInput> items;
        std::vector<ApprovedGraphAsset> approved;
        std::string snapshot, error;
        const size_t before_logs = adapter_owner_logs;
        REQUIRE(append_dist_bundle_graph(fixture.project, fixture.sdk,
            fixture.owned.path("generated"), fixture.owned.path("reports"),
            items, approved, snapshot, error));
        CHECK(adapter_owner_logs == before_logs + 1);
        REQUIRE(items.size() == approved.size());
        std::vector<JceCookInput> inputs;
        std::vector<JceDistGraphAsset> expected;
        std::vector<const char *> protected_paths;
        for (size_t i = 0; i < items.size(); ++i) {
            inputs.push_back({items[i].vpath.c_str(), items[i].bytes.data(), items[i].bytes.size()});
            expected.push_back({approved[i].address.c_str(), approved[i].content_id});
            if (items[i].protect_path) protected_paths.push_back(items[i].vpath.c_str());
        }
        CHECK(protected_paths.size() == (confidential ? 1u : 0u));
        if (confidential && !protected_paths.empty())
            CHECK(std::strcmp(protected_paths[0], consumer_literal) == 0);
        JceCookConfig cfg{};
        cfg.zstd_level = 1;
        cfg.compress_index = true;
        cfg.dedup_content = true;
        cfg.encrypt = true;
        cfg.encryption_key = key;
        cfg.encrypt_label = "project_assets";
        void *pak = nullptr;
        size_t pak_size = 0;
        REQUIRE(jce_archive_cook(inputs.data(), inputs.size(), &cfg, &pak, &pak_size, nullptr));
        const std::string pak_path = fixture.owned.path("secure.pak");
        REQUIRE(jce_fs_host_write_all(pak_path.c_str(), pak, pak_size));
        auto exe = make_gui_pe();
        // Compiled consumer address literal in a synthetic GUI PE byte fixture.
        exe.insert(exe.end(), consumer_literal, consumer_literal + sizeof(consumer_literal));
        exe.insert(exe.end(), static_cast<uint8_t *>(pak), static_cast<uint8_t *>(pak) + pak_size);
        jce_free(pak);
        const std::string package = fixture.owned.path("package");
        REQUIRE(jce_fs_host_create_directory(package.c_str()));
        const std::string exe_path = package + "/consumer.exe";
        REQUIRE(jce_fs_host_write_all(exe_path.c_str(), exe.data(), exe.size()));
        JceDistAuditInput audit{};
        audit.executable_path = exe_path.c_str();
        audit.pak_path = pak_path.c_str();
        audit.package_dir = package.c_str();
        audit.expected_exe_name = "consumer.exe";
        audit.graph_snapshot_path = snapshot.c_str();
        audit.protected_paths = protected_paths.data();
        audit.protected_path_count = protected_paths.size();
        audit.key = key;
        audit.graph_assets = expected.data();
        audit.graph_asset_count = expected.size();
        audit.require_secure = true;
        audit.require_gui = true;
        audit.require_single_file = true;
        audit.require_graph_parity = true;
        JceDistAuditResult result{};
        CHECK(jce_dist_audit_run(audit, &result) == !confidential);
        CHECK(result.encrypted_entries == items.size());
        CHECK(result.authenticated_entries == items.size());
        CHECK(result.verified_entries == items.size());
        CHECK(result.graph_verified == approved.size());
        CHECK(result.leaked_path_count == (confidential ? 1u : 0u));
        CHECK(result.observed_address_literal_count == (confidential ? 0u : 1u));
        CHECK(result.package_file_count == 1);
    }
    jce_archive_set_process_key(nullptr);
}

TEST_CASE("Dist shares canonical prepared runtime configuration roots and dependency closure")
{
    StockGraphFixture fixture;
    fixture.write_stock();
    const std::string prepared = fixture.owned.path("prepared");
    REQUIRE(jce_fs_host_create_directory((prepared + "/settings").c_str()));
    REQUIRE(jce_fs_host_create_directory((fixture.resources + "/data").c_str()));
    REQUIRE(jce_fs_host_create_directory((fixture.project + "/.jce").c_str()));
    REQUIRE(jce_fs_host_create_directory((fixture.project + "/Settings").c_str()));
    REQUIRE(write_text(fixture.project + "/Settings/RenderPipeline.rp.json",
        "{\"marker\":\"wrong-authored-rp\"}"));
    static const char source_rs[] = "{\"rendering\":{\"look\":{\"lutPath\":\"data/look.bin\"}},\"marker\":\"source-rs\"}";
    static const char prepared_rp[] = "{\"marker\":\"prepared-rp\"}";
    static const char authored_input[] = "{\"marker\":\"authored-input\"}";
    static const char authored_project[] = "{\"graphics\":{\"color_space\":0},\"marker\":\"authored-project\"}";
    static const char prepared_physics[] = "{\"marker\":\"prepared-physics\"}";
    REQUIRE(write_text(fixture.resources + "/render_settings.json", source_rs));
    REQUIRE(write_text(prepared + "/render_settings.json", "{\"marker\":\"wrong-prepared-rs\"}"));
    REQUIRE(write_text(prepared + "/settings/render_pipeline.rp.json", prepared_rp));
    REQUIRE(write_text(fixture.project + "/.jce/input_actions.json", authored_input));
    REQUIRE(write_text(fixture.project + "/.jce/project-settings.json", authored_project));
    REQUIRE(write_text(prepared + "/physics_layers.json", prepared_physics));
    REQUIRE(write_text(fixture.resources + "/data/look.bin", "source-lut"));
    JceDistContentGraph graph;
    std::string error;
    REQUIRE(jce_dist_content_graph_build(fixture.project,
        fixture.owned.path("generated"), fixture.owned.path("reports"), 0,
        nullptr, nullptr, &graph, &error, fixture.sdk, prepared));
    const auto *rs = find_asset(graph, "render_settings.json");
    const auto *rp = find_asset(graph, "settings/render_pipeline.rp.json");
    const auto *input = find_asset(graph, "settings/input_actions.json");
    const auto *lut = find_asset(graph, "data/look.bin");
    const auto *project_settings = find_asset(graph, "settings/project-settings.json");
    const auto *physics = find_asset(graph, "physics_layers.json");
    REQUIRE(rs != nullptr);
    REQUIRE(rp != nullptr);
    REQUIRE(input != nullptr);
    REQUIRE(lut != nullptr);
    REQUIRE(project_settings != nullptr);
    REQUIRE(physics != nullptr);
    CHECK(project_settings->content_id == jce_archive_content_hash(authored_project, sizeof(authored_project) - 1));
    CHECK(physics->content_id == jce_archive_content_hash(prepared_physics, sizeof(prepared_physics) - 1));
    CHECK(rs->content_id == jce_archive_content_hash(source_rs, sizeof(source_rs) - 1));
    CHECK(rp->content_id == jce_archive_content_hash(prepared_rp, sizeof(prepared_rp) - 1));
    CHECK(input->content_id == jce_archive_content_hash(authored_input, sizeof(authored_input) - 1));
    CHECK(std::string(lut->bytes.begin(), lut->bytes.end()) == "source-lut");
    CHECK(graph.confidential_paths.empty());
}

TEST_CASE("prepared roots compose existing required assets and reject later missing chosen bytes")
{
    JceTestOwnedDirectory owned{JCE_TEST_DIST_FIXTURE_PREFIX "_prepared"};
    const std::string project = owned.path("project");
    const std::string source = project + "/assets";
    const std::string prepared = owned.path("prepared");
    REQUIRE(jce_fs_host_create_directory((source + "/scenes").c_str()));
    REQUIRE(jce_fs_host_create_directory((source + "/data").c_str()));
    REQUIRE(jce_fs_host_create_directory((prepared + "/settings").c_str()));
    REQUIRE(write_text(project + "/jce_project.json",
        "{\"schema\":2,\"name\":\"prepared_test\",\"source_assets\":\"assets\",\"startup_scene\":\"scenes/main.scene\"}"));
    const std::string scene = source + "/scenes/main.scene";
    REQUIRE(write_text(scene, "{\"entities\":[]}"));
    REQUIRE(write_text(source + "/data/keep.bin", "existing-root"));
    const std::string chosen = prepared + "/settings/render_pipeline.rp.json";
    REQUIRE(write_text(chosen, "{}"));
    const char *scenes[] = {scene.c_str()};
    const char *existing[] = {"data/keep.bin"};
    JceBundlePackOptions options{};
    const std::string output = owned.path("output");
    options.scenes_dir = source.c_str();
    options.resource_root = source.c_str();
    options.out_dir = output.c_str();
    options.scene_files = scenes;
    options.scene_file_count = 1;
    options.single_file_mode = true;
    options.single_bundle_id = "prepared_probe";
    options.zstd_level = 1;
    options.cook_assets = true;
    options.target_platform = 0;
    options.always_included = existing;
    options.always_included_count = 1;
    JceProjectPreparedAssets inputs;
    std::string error;
    REQUIRE(jce_project_prepared_assets_bind(project, &options, &inputs, &error, prepared));
    REQUIRE(options.always_included_count == 2);
    CHECK(std::strcmp(options.always_included[0], "data/keep.bin") == 0);
    CHECK(std::strcmp(options.always_included[1], "settings/render_pipeline.rp.json") == 0);
    REQUIRE(jce_bundle_pack_run(&options, nullptr, nullptr) == 0);
    JceArchive *bundle = jce_archive_open_file((output + "/prepared_probe.jbundle").c_str());
    REQUIRE(bundle != nullptr);
    CHECK(jce_archive_find(bundle, "data/keep.bin") != nullptr);
    CHECK(jce_archive_find(bundle, "settings/render_pipeline.rp.json") != nullptr);
    CHECK(jce_archive_find(bundle, "render_settings.json") == nullptr);
    CHECK(jce_archive_find(bundle, "settings/input_actions.json") == nullptr);
    CHECK(jce_archive_find(bundle, "settings/project-settings.json") == nullptr);
    CHECK(jce_archive_find(bundle, "physics_layers.json") == nullptr);
    jce_archive_close(bundle);
    REQUIRE(jce_fs_host_remove_file(chosen.c_str()));
    std::string first_error;
    CHECK(jce_bundle_pack_run(&options, capture_first_pack_error, &first_error) != 0);
    CHECK(first_error.find("settings/render_pipeline.rp.json") != std::string::npos);
}

TEST_CASE("environment project produces a complete Dist graph")
{
    const char *project = std::getenv("JCE_DIST_GRAPH_PROJECT");
    const char *output = std::getenv("JCE_DIST_GRAPH_OUT");
    if (!project || !project[0] || !output || !output[0]) return;
    const std::string output_root = std::string(output) + "/" + JCE_TEST_DIST_FIXTURE_PREFIX;
    REQUIRE(jce_fs_host_create_directory(output_root.c_str()));
    const std::string generated = output_root + "/generated";
    const std::string reports = output_root + "/reports";
    REQUIRE(jce_fs_host_create_directory(generated.c_str()));
    REQUIRE(jce_fs_host_create_directory(reports.c_str()));

    JceDistContentGraph graph;
    std::string error;
    REQUIRE(jce_dist_content_graph_build(
        project, generated, reports, jce_dist_content_graph_host_platform(),
        nullptr, nullptr, &graph, &error));

    static const char *required[] = {
        "audio/sounds/thunder/near/thunder_strike.mp3",
        "textures/ground_baked2/spring_day.png",
        "textures/ground_baked2/spring_night.png",
        "textures/ground_baked2/winter_day.png",
        "textures/ground_baked2/winter_night.png",
        "textures/ground_baked2/autumn_day.png",
        "textures/ground_baked2/autumn_night.png",
        "textures/ground_baked2/rainy_day.png",
        "textures/ground_baked2/rainy_night.png",
        "render_settings.json"
    };
    for (const char *address : required)
        CHECK(find_asset(graph, address) != nullptr);
    CHECK(graph.assets.size() == 50);

    std::vector<JceCookInput> inputs(graph.assets.size());
    std::vector<JceDistGraphAsset> expected(graph.assets.size());
    std::vector<const char *> protected_paths(graph.assets.size());
    for (size_t i = 0; i < graph.assets.size(); ++i) {
        inputs[i] = {graph.assets[i].address.c_str(),
                     graph.assets[i].bytes.data(),
                     graph.assets[i].bytes.size()};
        expected[i] = {graph.assets[i].address.c_str(),
                       graph.assets[i].content_id};
        protected_paths[i] = graph.assets[i].address.c_str();
    }
    uint8_t key[32];
    for (size_t i = 0; i < sizeof(key); ++i)
        key[i] = (uint8_t)(i * 13u + 5u);
    JceCookConfig config{};
    config.zstd_level = 3;
    config.compress_index = true;
    config.dedup_content = true;
    config.encrypt = true;
    config.encryption_key = key;
    config.encrypt_label = "project_assets";
    void *pak_raw = nullptr;
    size_t pak_size = 0;
    REQUIRE(jce_archive_cook(inputs.data(), inputs.size(), &config,
                             &pak_raw, &pak_size, nullptr));
    const std::string pak_path = output_root + "/project_assets.pak";
    const std::string package = output_root + "/package";
    const std::string exe_path = package + "/elemental_probe.exe";
    const std::string report_path =
        output_root + "/reports/elemental_dist_audit.json";
    REQUIRE(jce_fs_host_create_directory(package.c_str()));
    REQUIRE(jce_fs_host_write_all(pak_path.c_str(), pak_raw, pak_size));
    std::vector<uint8_t> exe = make_gui_pe();
    exe.insert(exe.end(), (uint8_t *)pak_raw,
               (uint8_t *)pak_raw + pak_size);
    jce_free(pak_raw);
    REQUIRE(jce_fs_host_write_all(exe_path.c_str(), exe.data(), exe.size()));

    JceDistAuditInput audit{};
    audit.executable_path = exe_path.c_str();
    audit.pak_path = pak_path.c_str();
    audit.report_path = report_path.c_str();
    audit.package_dir = package.c_str();
    audit.expected_exe_name = "elemental_probe.exe";
    audit.graph_snapshot_path = graph.snapshot_path.c_str();
    audit.protected_paths = protected_paths.data();
    audit.protected_path_count = protected_paths.size();
    audit.key = key;
    audit.require_secure = true;
    audit.require_gui = true;
    audit.require_single_file = true;
    audit.graph_assets = expected.data();
    audit.graph_asset_count = expected.size();
    audit.require_graph_parity = true;
    JceDistAuditResult result{};
    CHECK(jce_dist_audit_run(audit, &result));
    CHECK(result.pe_valid);
    CHECK(result.pe_subsystem == 2);
    CHECK(result.pak_embedded_exactly_once);
    CHECK(result.archive_entries == 50);
    CHECK(result.encrypted_entries == 50);
    CHECK(result.authenticated_entries == 50);
    CHECK(result.verified_entries == 50);
    CHECK(result.graph_expected == 50);
    CHECK(result.graph_verified == 50);
    CHECK(result.graph_mismatch_count == 0);
    CHECK(result.leaked_path_count == 0);
    CHECK(result.package_file_count == 1);
    CHECK(jce_fs_host_exists_file(report_path.c_str()));
    jce_archive_set_process_key(nullptr);
}
