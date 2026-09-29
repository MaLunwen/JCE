#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/jce_dist_audit.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> make_pe(uint16_t subsystem)
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
    bytes[0xdc] = (uint8_t)subsystem;
    bytes[0xdd] = (uint8_t)(subsystem >> 8);
    return bytes;
}

} // namespace

TEST_CASE("PE subsystem parser distinguishes GUI from console")
{
    uint16_t subsystem = 0;
    std::vector<uint8_t> gui = make_pe(2);
    CHECK(jce_dist_audit_pe_subsystem(gui.data(), gui.size(), &subsystem));
    CHECK(subsystem == 2);

    std::vector<uint8_t> console = make_pe(3);
    CHECK(jce_dist_audit_pe_subsystem(console.data(), console.size(),
                                      &subsystem));
    CHECK(subsystem == 3);

    gui[0] = 0;
    CHECK_FALSE(jce_dist_audit_pe_subsystem(gui.data(), gui.size(),
                                            &subsystem));
}

TEST_CASE("leak scanner ignores the encrypted PAK interval only")
{
    static const char path[] = "textures/secret_asset.png";
    std::vector<uint8_t> bytes(256, 0);
    std::memcpy(bytes.data() + 96, path, sizeof(path) - 1u);

    CHECK(jce_dist_audit_find_outside_range(
              bytes.data(), bytes.size(), (const uint8_t *)path,
              sizeof(path) - 1u, 64, 128) == SIZE_MAX);

    std::memcpy(bytes.data() + 16, path, sizeof(path) - 1u);
    CHECK(jce_dist_audit_find_outside_range(
              bytes.data(), bytes.size(), (const uint8_t *)path,
              sizeof(path) - 1u, 64, 128) == 16);
}

TEST_CASE("dist audit requires Bundle graph address and content parity")
{
    static const char a[] = "cooked-a";
    static const char b[] = "cooked-b";
    const JceCookInput inputs[] = {
        {"project/a.bin", a, sizeof(a) - 1u},
        {"project/b.bin", b, sizeof(b) - 1u}
    };
    uint8_t key[32];
    for (size_t i = 0; i < sizeof(key); ++i)
        key[i] = (uint8_t)(i * 7u + 3u);

    JceCookConfig cfg{};
    cfg.zstd_level = 1;
    cfg.compress_index = true;
    cfg.dedup_content = true;
    cfg.encrypt = true;
    cfg.encryption_key = key;
    cfg.encrypt_label = "project_assets";

    void *pak_raw = nullptr;
    size_t pak_size = 0;
    REQUIRE(jce_archive_cook(inputs, 2, &cfg, &pak_raw, &pak_size, nullptr));
    std::vector<uint8_t> pak((uint8_t *)pak_raw,
                             (uint8_t *)pak_raw + pak_size);
    jce_free(pak_raw);

    std::vector<uint8_t> exe = make_pe(2);
    exe.insert(exe.end(), pak.begin(), pak.end());
    const char *exe_path = "_ut_dist_graph.exe";
    const char *pak_path = "_ut_dist_graph.pak";
    REQUIRE(jce_fs_host_write_all(exe_path, exe.data(), exe.size()));
    REQUIRE(jce_fs_host_write_all(pak_path, pak.data(), pak.size()));

    JceDistGraphAsset graph[] = {
        {"project/a.bin", jce_archive_content_hash(a, sizeof(a) - 1u)},
        {"project/b.bin", jce_archive_content_hash(b, sizeof(b) - 1u)}
    };
    JceDistAuditInput input{};
    input.executable_path = exe_path;
    input.pak_path = pak_path;
    input.key = key;
    input.require_secure = true;
    input.require_gui = true;
    input.graph_assets = graph;
    input.graph_asset_count = 2;
    input.require_graph_parity = true;

    JceDistAuditResult result{};
    CHECK(jce_dist_audit_run(input, &result));
    CHECK(result.graph_expected == 2);
    CHECK(result.graph_verified == 2);
    CHECK(result.graph_mismatch_count == 0);

    graph[1].content_id ^= 1u;
    CHECK_FALSE(jce_dist_audit_run(input, &result));
    CHECK(result.graph_mismatch_count == 1);

    jce_archive_set_process_key(nullptr);
    (void)jce_fs_host_remove_file(exe_path);
    (void)jce_fs_host_remove_file(pak_path);
}
