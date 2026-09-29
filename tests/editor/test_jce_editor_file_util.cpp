// test_jce_editor_file_util.cpp — round-trip ed_read_file / ed_write_file.
//
// These header-only helpers wrap jce_fs_host_* so editor code never
// touches stdio / Win32 directly.  We use the engine's portable temp
// dir (jce_path_temp_dir) instead of hardcoding any platform path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "io/jce_editor_file_util.h"

#include <jce/os/core/jce_filesystem.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string temp_path(const char *leaf)
{
    char base[1024] = {0};
    char out[1200]  = {0};
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    std::snprintf(out, sizeof(out), "%s/_ut_editor_fileutil_%s", base, leaf);
    return std::string(out);
}

}  // namespace

TEST_CASE("ed_write_file -> ed_read_file round-trip")
{
    const std::string p = temp_path("jce_editor_file_util_roundtrip.bin");
    const char payload[] = "hello editor file util";
    constexpr size_t payload_len = sizeof(payload) - 1;

    REQUIRE(ed_write_file(p.c_str(), payload, payload_len));

    size_t got = 0;
    void *buf = ed_read_file(p.c_str(), &got);
    REQUIRE(buf != nullptr);
    CHECK(got == payload_len);
    CHECK(std::memcmp(buf, payload, payload_len) == 0);
    /* ed_read_file appends a NUL sentinel beyond out_size. */
    CHECK(static_cast<char *>(buf)[got] == '\0');
    ED_FREE(buf);
}

TEST_CASE("ed_read_file returns NULL for missing path")
{
    size_t got = 12345;
    void *buf = ed_read_file("definitely/does/not/exist/zzz_jce_xx.bin", &got);
    CHECK(buf == nullptr);
    CHECK(got == 0);
}

TEST_CASE("ed_read_file is null-safe")
{
    size_t got = 12345;
    CHECK(ed_read_file(nullptr, &got) == nullptr);
    CHECK(got == 0);
    /* Also tolerates a null out_size. */
    CHECK(ed_read_file(nullptr, nullptr) == nullptr);
}

TEST_CASE("ed_write_file rejects null path")
{
    const char data[] = "x";
    CHECK_FALSE(ed_write_file(nullptr, data, 1));
}

TEST_CASE("ed_write_file rejects null data with non-zero size")
{
    const std::string p = temp_path("jce_editor_file_util_null_data.bin");
    CHECK_FALSE(ed_write_file(p.c_str(), nullptr, 8));
}

TEST_CASE("ed_read_file_capped caps the read at max_bytes and reports total")
{
    const std::string p = temp_path("jce_editor_file_util_capped.bin");
    /* Build a 1 KiB blob. */
    constexpr size_t total_bytes = 1024;
    unsigned char src[total_bytes];
    for (size_t i = 0; i < total_bytes; ++i)
        src[i] = static_cast<unsigned char>(i & 0xFF);
    REQUIRE(ed_write_file(p.c_str(), src, total_bytes));

    size_t got   = 0;
    size_t total = 0;
    void *buf = ed_read_file_capped(p.c_str(), 64, &got, &total);
    REQUIRE(buf != nullptr);
    CHECK(got   == 64);
    CHECK(total == total_bytes);
    CHECK(std::memcmp(buf, src, 64) == 0);
    CHECK(static_cast<char *>(buf)[got] == '\0');
    ED_FREE(buf);
}
