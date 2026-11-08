#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/jce_dist_audit.h"
#include "jce_test_owned_directory.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
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

void put16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8u);
}

void put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4u; ++i) bytes[offset + i] = (uint8_t)(value >> (i * 8u));
}

std::vector<uint8_t> make_delivery_pe(uint16_t machine,
    const std::vector<const char *> &ordinary,
    const std::vector<const char *> &delayed = {}, bool delay_va = false)
{
    std::vector<uint8_t> bytes(4096, 0);
    bytes[0] = 'M'; bytes[1] = 'Z';
    put32(bytes, 0x3c, 0x80);
    std::memcpy(bytes.data() + 0x80, "PE\0\0", 4);
    put16(bytes, 0x84, machine);
    put16(bytes, 0x86, 1);
    const bool pe32 = machine == 0x014cu;
    const size_t optional_size = pe32 ? 224u : 240u;
    const size_t directory_base = 0x98u + (pe32 ? 96u : 112u);
    put16(bytes, 0x94, (uint16_t)optional_size);
    put16(bytes, 0x98, pe32 ? 0x10b : 0x20b);
    put16(bytes, 0x98 + 68, 2);
    if (pe32) put32(bytes, 0x98 + 28, 0x400000);
    else {
        put32(bytes, 0x98 + 24, 0x40000000);
        put32(bytes, 0x98 + 28, 1);
    }
    put32(bytes, directory_base - 4, 16);
    const size_t section = 0x98 + optional_size;
    put32(bytes, section + 8, 3584);
    put32(bytes, section + 12, 0x1000);
    put32(bytes, section + 16, 3584);
    put32(bytes, section + 20, 512);
    size_t cursor = 2048;
    for (unsigned directory : {1u, 13u}) {
        const auto &names = directory == 1u ? ordinary : delayed;
        if (names.empty()) continue;
        const size_t table = directory == 1u ? 512u : 1024u;
        const size_t width = directory == 1u ? 20u : 32u;
        put32(bytes, directory_base + directory * 8, (uint32_t)(table - 512 + 0x1000));
        put32(bytes, directory_base + directory * 8 + 4, (uint32_t)((names.size() + 1) * width));
        for (size_t i = 0; i < names.size(); ++i) {
            const size_t length = std::strlen(names[i]) + 1;
            REQUIRE(cursor + length < bytes.size());
            std::memcpy(bytes.data() + cursor, names[i], length);
            uint32_t name_rva = (uint32_t)(cursor - 512 + 0x1000);
            if (directory == 1u) put32(bytes, table + i * width + 12, name_rva);
            else {
                put32(bytes, table + i * width, delay_va ? 0u : 1u);
                put32(bytes, table + i * width + 4,
                      delay_va ? name_rva + 0x400000u : name_rva);
            }
            cursor += length;
        }
    }
    return bytes;
}

struct PeAuditFixture {
    JceTestOwnedDirectory owned{"jce_dist_pe_delivery"};
    std::vector<uint8_t> pak;
    uint8_t key[32]{};
    std::string exe_path = owned.path("consumer.exe");
    std::string pak_path = owned.path("secure.pak");
    std::string report_path = owned.path("audit.json");
    JceDistAuditResult result{};

    PeAuditFixture()
    {
        for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i * 11u + 3u);
        static const char payload[] = "owned PE delivery fixture";
        const JceCookInput entry{"fixture/payload.bin", payload, sizeof(payload) - 1u};
        JceCookConfig config{};
        config.zstd_level = 1;
        config.encrypt = true;
        config.encryption_key = key;
        config.encrypt_label = "project_assets";
        void *raw = nullptr;
        size_t size = 0;
        REQUIRE(jce_archive_cook(&entry, 1, &config, &raw, &size, nullptr));
        pak.assign((const uint8_t *)raw, (const uint8_t *)raw + size);
        jce_free(raw);
        REQUIRE(jce_fs_host_write_all(pak_path.c_str(), pak.data(), pak.size()));
    }

    ~PeAuditFixture() { jce_archive_set_process_key(nullptr); }

    bool audit(std::vector<uint8_t> bytes, uint16_t expected_machine = 0x8664u)
    {
        bytes.insert(bytes.end(), pak.begin(), pak.end());
        REQUIRE(jce_fs_host_write_all(exe_path.c_str(), bytes.data(), bytes.size()));
        JceDistAuditInput input{};
        input.executable_path = exe_path.c_str();
        input.pak_path = pak_path.c_str();
        input.report_path = report_path.c_str();
        input.key = key;
        input.require_gui = true;
        input.expected_pe_machine = expected_machine;
        return jce_dist_audit_run(input, &result);
    }
};

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

    JceTestOwnedDirectory owned{"jce_dist_audit"};
    static const char consumer_literal[] = "project/a.bin";
    std::vector<uint8_t> exe = make_pe(2);
    exe.insert(exe.end(), consumer_literal, consumer_literal + sizeof(consumer_literal));
    exe.insert(exe.end(), pak.begin(), pak.end());
    const std::string exe_path = owned.path("consumer.exe");
    const std::string pak_path = owned.path("secure.pak");
    const std::string report_path = owned.path("audit.json");
    REQUIRE(jce_fs_host_write_all(exe_path.c_str(), exe.data(), exe.size()));
    REQUIRE(jce_fs_host_write_all(pak_path.c_str(), pak.data(), pak.size()));

    JceDistGraphAsset graph[] = {
        {"project/a.bin", jce_archive_content_hash(a, sizeof(a) - 1u)},
        {"project/b.bin", jce_archive_content_hash(b, sizeof(b) - 1u)}
    };
    JceDistAuditInput input{};
    input.executable_path = exe_path.c_str();
    input.pak_path = pak_path.c_str();
    input.report_path = report_path.c_str();
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
    CHECK(result.encrypted_entries == 2);
    CHECK(result.authenticated_entries == 2);
    CHECK(result.verified_entries == 2);
    CHECK(result.leaked_path_count == 0);
    CHECK(result.observed_address_literal_count == 1);
    JceJson *report = jce_json_parse_file(report_path.c_str());
    REQUIRE(report != nullptr);
    CHECK(jce_json_array_size(jce_json_get(report, "leaked_paths")) == 0);
    CHECK(jce_json_array_size(jce_json_get(report, "address_literals_outside_project_pak")) == 1);
    CHECK(std::strstr(jce_json_get_string(report, "address_literal_scope", ""),
                      "does not prove plaintext payload leakage") != nullptr);
    jce_json_free(report);

    // The same compiled consumer literal becomes fatal only when explicitly
    // confidential. This does not change encrypted/authenticated payload counts.
    const char *protected_paths[] = {consumer_literal};
    input.protected_paths = protected_paths;
    input.protected_path_count = 1;
    CHECK_FALSE(jce_dist_audit_run(input, &result));
    CHECK(result.leaked_path_count == 1);
    CHECK(result.observed_address_literal_count == 0);
    CHECK(result.encrypted_entries == 2);
    CHECK(result.authenticated_entries == 2);
    CHECK(result.verified_entries == 2);
    input.protected_paths = nullptr;
    input.protected_path_count = 0;

    graph[1].content_id ^= 1u;
    CHECK_FALSE(jce_dist_audit_run(input, &result));
    CHECK(result.graph_mismatch_count == 1);

    jce_archive_set_process_key(nullptr);
}


TEST_CASE("PE delivery accepts each requested Windows machine and OS-only imports")
{
    PeAuditFixture fixture;
    for (uint16_t machine : {0x014cu, 0x8664u, 0xaa64u}) {
        CAPTURE(machine);
        CHECK(fixture.audit(make_delivery_pe(machine, {"KERNEL32.dll", "USER32.dll"},
                             {"d3d11.dll", "KERNEL32.dll"}), machine));
        CHECK(fixture.result.pe_valid);
        CHECK(fixture.result.pe_machine == machine);
        CHECK(fixture.result.pe_imports_verified);
        CHECK(fixture.result.pe_import_count == 3);
    }
    REQUIRE(fixture.audit(make_delivery_pe(0x8664, {"KERNEL32.dll"}, {"d3d11.dll"})));
    JceJson *report = jce_json_parse_file(fixture.report_path.c_str());
    REQUIRE(report != nullptr);
    const JceJson *pe = jce_json_get(report, "pe");
    REQUIRE(pe != nullptr);
    CHECK(jce_json_get_int(pe, "machine", 0) == 0x8664);
    CHECK(jce_json_get_int(pe, "expected_machine", 0) == 0x8664);
    CHECK(jce_json_get_bool(pe, "imports_verified", false));
    CHECK(jce_json_array_size(jce_json_get(pe, "imports")) == 2);
    jce_json_free(report);
}

TEST_CASE("PE delivery rejects a wrong or unknown requested machine")
{
    PeAuditFixture fixture;
    for (uint16_t expected : std::vector<uint16_t>{0x014cu, 0xaa64u, UINT16_MAX}) {
        CAPTURE(expected);
        CHECK_FALSE(fixture.audit(make_delivery_pe(0x8664, {"kernel32.dll"}), expected));
        CHECK(fixture.result.pe_valid);
        CHECK(fixture.result.pe_machine == 0x8664);
        CHECK(fixture.result.pe_imports_verified);
        CHECK(std::strstr(fixture.result.error, "requested architecture") != nullptr);
    }
}

TEST_CASE("PE delivery rejects external ordinary and delayed dependencies")
{
    PeAuditFixture fixture;
    for (const char *dependency : {"python312.dll", "nethost.dll", "MSVCP140.dll",
                                  "VCRUNTIME140.dll", "jce_script_api.dll"}) {
        CAPTURE(dependency);
        for (bool delayed : {false, true}) {
            CAPTURE(delayed);
            const std::vector<const char *> ordinary = delayed
                ? std::vector<const char *>{"kernel32.dll"}
                : std::vector<const char *>{"kernel32.dll", dependency};
            const std::vector<const char *> delay = delayed
                ? std::vector<const char *>{dependency} : std::vector<const char *>{};
            CHECK_FALSE(fixture.audit(make_delivery_pe(0x8664, ordinary, delay)));
            CHECK(fixture.result.pe_valid);
            CHECK_FALSE(fixture.result.pe_imports_verified);
            CHECK(fixture.result.pe_import_count == 2);
            CHECK(std::strstr(fixture.result.error, "external DLLs") != nullptr);
        }
    }
    CHECK_FALSE(fixture.audit(make_delivery_pe(0x8664, {})));
    CHECK(std::strstr(fixture.result.error, "no imports") != nullptr);
}

TEST_CASE("PE delivery preserves CLI case and API-set prefix policy")
{
    PeAuditFixture fixture;
    CHECK(fixture.audit(make_delivery_pe(0x8664,
        {"KeRnEl32.DlL", "API-MS-WIN-CORE-FILE-L1-1-0.DLL", "EXT-MS-WIN-GDI-TEST.DLL"})));
    for (const char *dependency : {"api-ms-win-crt-runtime-l1-1-0.dll",
                                  "api-ms-win-core", "ext-ms-win", "kernel32.exe",
                                  "dir/kernel32.dll"}) {
        CAPTURE(dependency);
        CHECK_FALSE(fixture.audit(make_delivery_pe(0x8664, {dependency})));
        CHECK(std::strstr(fixture.result.error, "external DLLs") != nullptr);
    }
}

TEST_CASE("PE32 delayed VA imports use the image base without underflow")
{
    PeAuditFixture fixture;
    CHECK(fixture.audit(make_delivery_pe(0x014c, {"kernel32.dll"}, {"d3d11.dll"}, true), 0x014c));
    auto bytes = make_delivery_pe(0x014c, {"kernel32.dll"}, {"d3d11.dll"}, true);
    put32(bytes, 1024 + 4, 0x3fffff);
    CHECK_FALSE(fixture.audit(bytes, 0x014c));
    CHECK(std::strstr(fixture.result.error, "precedes image base") != nullptr);
}

TEST_CASE("PE delivery rejects truncated or out-of-section import evidence")
{
    PeAuditFixture fixture;
    const auto valid = make_delivery_pe(0x8664, {"kernel32.dll"});
    auto bytes = valid;
    put32(bytes, 0x3c, UINT32_MAX);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put16(bytes, 0x94, UINT16_MAX);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put32(bytes, 0x98 + 108, 17);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put16(bytes, 0x86, UINT16_MAX);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put16(bytes, 0x98, 0x10b);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put32(bytes, 0x98 + 112 + 8, 0xfffffff0);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put32(bytes, 0x98 + 112 + 8 + 4, 19);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put32(bytes, 0x98 + 112 + 8 + 4, 20);
    CHECK_FALSE(fixture.audit(bytes));
    CHECK(std::strstr(fixture.result.error, "unterminated PE import table") != nullptr);
    bytes = valid;
    put32(bytes, 512 + 12, 0x1e00); // First byte after the file-backed section.
    CHECK_FALSE(fixture.audit(bytes));
    CHECK(std::strstr(fixture.result.error, "outside mapped file section") != nullptr);
    bytes = valid;
    put32(bytes, 512 + 12, 0x1dfc);
    std::memcpy(bytes.data() + 4092, "abcd", 4); // No NUL before embedded PAK.
    CHECK_FALSE(fixture.audit(bytes));
    CHECK(std::strstr(fixture.result.error, "unterminated PE import name") != nullptr);
    bytes = valid;
    std::memset(bytes.data() + 2048, 'a', 260);
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    bytes[2048] = 0xff;
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    bytes[2048] = 0;
    CHECK_FALSE(fixture.audit(bytes));
    bytes = valid;
    put32(bytes, 0x98 + 240 + 20, UINT32_MAX);
    CHECK_FALSE(fixture.audit(bytes));
}
