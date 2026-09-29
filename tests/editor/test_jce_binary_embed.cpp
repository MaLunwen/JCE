#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_binary_embed.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string test_path(const char *leaf)
{
    char cwd[1024] = {0};
    char path[1200] = {0};
    REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    std::snprintf(path, sizeof(path), "%s/_ut_binary_embed_%s", cwd, leaf);
    return path;
}

std::vector<uint8_t> read_bytes(const std::string &path)
{
    uint64_t size = 0;
    void *raw = jce_fs_host_read_all(path.c_str(), &size);
    REQUIRE(raw != nullptr);
    const uint8_t *bytes = static_cast<const uint8_t *>(raw);
    std::vector<uint8_t> result(bytes, bytes + size);
    jce_fs_buffer_free(raw);
    return result;
}

uint16_t le16(const std::vector<uint8_t> &bytes, size_t offset)
{
    REQUIRE(offset + 2 <= bytes.size());
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1] << 8);
}

uint32_t le32(const std::vector<uint8_t> &bytes, size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint64_t le64(const std::vector<uint8_t> &bytes, size_t offset)
{
    REQUIRE(offset + 8 <= bytes.size());
    return static_cast<uint64_t>(le32(bytes, offset)) |
           (static_cast<uint64_t>(le32(bytes, offset + 4)) << 32);
}

std::string as_text(const std::vector<uint8_t> &bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()),
                       bytes.size());
}

} // namespace

TEST_CASE("C source embed preserves bytes and publishes an exact size")
{
    const std::string path = test_path("payload.c");
    const uint8_t payload[] = {0x00, 0x7f, 0xff};

    REQUIRE(jce_binary_embed_write_c_source(
        path, "jce_test_payload", payload, sizeof(payload)));
    const std::string text = as_text(read_bytes(path));
    CHECK(text.find("const unsigned char jce_test_payload[]") !=
          std::string::npos);
    CHECK(text.find("0x00, 0x7f, 0xff") != std::string::npos);
    CHECK(text.find("const size_t jce_test_payload_size = 3;") !=
          std::string::npos);

    (void)jce_fs_host_remove_file(path.c_str());
}

TEST_CASE("embed writers reject a null payload with a nonzero size")
{
    const std::string c_path = test_path("null.c");
    const std::string obj_path = test_path("null.obj");

    CHECK_FALSE(jce_binary_embed_write_c_source(
        c_path, "jce_null_payload", nullptr, 4));
    CHECK_FALSE(jce_binary_embed_write_coff(
        obj_path, "jce_null_payload", nullptr, 4, "x64"));

    (void)jce_fs_host_remove_file(c_path.c_str());
    (void)jce_fs_host_remove_file(obj_path.c_str());
}

TEST_CASE("x64 COFF embed has deterministic read-only data and symbols")
{
    const std::string path = test_path("payload.obj");
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    const std::string symbol = "jce_embedded_payload_with_long_name";

    REQUIRE(jce_binary_embed_write_coff(
        path, symbol, payload, sizeof(payload), "x86_64"));
    const std::vector<uint8_t> bytes = read_bytes(path);

    REQUIRE(bytes.size() >= 116);
    CHECK(le16(bytes, 0) == 0x8664);
    CHECK(le16(bytes, 2) == 1);
    CHECK(le32(bytes, 4) == 0);
    CHECK(le32(bytes, 12) == 2);
    CHECK(std::memcmp(bytes.data() + 20, ".rdata", 6) == 0);
    CHECK(le32(bytes, 36) == 16);
    CHECK(le32(bytes, 40) == 60);
    CHECK(le32(bytes, 56) == 0x40500040);
    CHECK(std::memcmp(bytes.data() + 60, payload, sizeof(payload)) == 0);
    CHECK(le64(bytes, 68) == sizeof(payload));

    const uint32_t symbol_table = le32(bytes, 8);
    REQUIRE(symbol_table + 36 + 4 <= bytes.size());
    CHECK(le32(bytes, symbol_table) == 0);
    CHECK(le32(bytes, symbol_table + 4) == 4);
    CHECK(le32(bytes, symbol_table + 18) == 0);
    const uint32_t size_name_offset = le32(bytes, symbol_table + 22);
    const uint32_t string_table = symbol_table + 36;
    CHECK(le32(bytes, string_table) == bytes.size() - string_table);
    CHECK(std::strcmp(
              reinterpret_cast<const char *>(bytes.data() + string_table + 4),
              symbol.c_str()) == 0);
    CHECK(std::strcmp(
              reinterpret_cast<const char *>(bytes.data() + string_table +
                                             size_name_offset),
              (symbol + "_size").c_str()) == 0);

    (void)jce_fs_host_remove_file(path.c_str());
}

TEST_CASE("COFF embed rejects unsupported architectures")
{
    const std::string path = test_path("unsupported.obj");
    const uint8_t payload = 1;
    CHECK_FALSE(jce_binary_embed_write_coff(
        path, "jce_payload", &payload, 1, "mips64"));
    (void)jce_fs_host_remove_file(path.c_str());
}

TEST_CASE("incbin embed canonicalizes and quotes the source path")
{
    const std::string path = test_path("payload.S");
    REQUIRE(jce_binary_embed_write_incbin(
        path, "jce_payload", "D:\\assets\\\"quoted\"\\payload.pak", 17));

    const std::string text = as_text(read_bytes(path));
    CHECK(text.find(
              ".incbin \"D:/assets/\\\"quoted\\\"/payload.pak\"") !=
          std::string::npos);
    CHECK(text.find("jce_payload_size:\n    .quad 17") !=
          std::string::npos);

    (void)jce_fs_host_remove_file(path.c_str());
}
