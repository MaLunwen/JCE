#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/jce_dist_content_graph.h"
#include "core/jce_dist_audit.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

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
    const std::string root = "_ut_dist_content_graph";
    (void)jce_fs_host_remove_recursive(root.c_str());
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

    (void)jce_fs_host_remove_recursive(root.c_str());
}

TEST_CASE("environment project produces a complete Dist graph")
{
    const char *project = std::getenv("JCE_DIST_GRAPH_PROJECT");
    const char *output = std::getenv("JCE_DIST_GRAPH_OUT");
    if (!project || !project[0] || !output || !output[0]) return;
    REQUIRE(jce_fs_host_create_directory(output));
    const std::string generated = std::string(output) + "/generated";
    const std::string reports = std::string(output) + "/reports";
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
    const std::string pak_path = std::string(output) + "/project_assets.pak";
    const std::string package = std::string(output) + "/package";
    const std::string exe_path = package + "/elemental_probe.exe";
    const std::string report_path =
        std::string(output) + "/reports/elemental_dist_audit.json";
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
