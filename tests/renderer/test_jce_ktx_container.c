/* Real bridge IO plus independent KTX1 wire fields and side-major pixels.
 * No GPU device is initialized. Malformed inputs stay bounded and small. */
#include "unity.h"
#include "renderer/jce_ktx2_writer.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t wire[2048];
static uint8_t source[1024];
static void *read_file;
static uint8_t *parsed;

void setUp(void) { read_file = NULL; parsed = NULL; }
void tearDown(void)
{
    if (read_file) jce_fs_buffer_free(read_file);
    if (parsed) jce_free(parsed);
    remove("cube.ktx");
    remove("invalid.ktx");
}

static uint32_t read_le(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8u |
        (uint32_t)data[2] << 16u | (uint32_t)data[3] << 24u;
}

static void write_le(uint8_t *data, uint32_t value)
{
    for (uint32_t byte = 0; byte < 4u; ++byte)
        data[byte] = (uint8_t)(value >> (byte * 8u));
}

static size_t make_cube(uint32_t bpp)
{
    static const uint8_t identifier[12] = {
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x31,
        0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a
    };
    memset(wire, 0, sizeof(wire));
    memcpy(wire, identifier, sizeof(identifier));
    write_le(wire + 12, 0x04030201u);
    write_le(wire + 16, bpp == 8u ? 0x140bu : 0x1401u);
    write_le(wire + 20, bpp == 8u ? 2u : 1u);
    write_le(wire + 24, 0x1908u);
    write_le(wire + 28, bpp == 8u ? 0x881au : 0x8058u);
    write_le(wire + 32, 0x1908u);
    write_le(wire + 36, 4u);
    write_le(wire + 40, 4u);
    write_le(wire + 52, 6u);
    write_le(wire + 56, 3u);
    size_t offset = 64u, input = 0u;
    for (uint32_t mip = 0; mip < 3u; ++mip) {
        const uint32_t edge = 4u >> mip;
        const uint32_t face_bytes = edge * edge * bpp;
        write_le(wire + offset, face_bytes);
        offset += 4u;
        for (uint32_t face = 0; face < 6u; ++face)
            for (uint32_t byte = 0; byte < face_bytes; ++byte) {
                const uint8_t value = (uint8_t)(mip * 51u + face * 13u + byte);
                wire[offset++] = value;
                source[input++] = value;
            }
    }
    return offset;
}

static void assert_rejected(const uint8_t *bytes, size_t length)
{
    uint32_t edge = 77u, mips = 88u, bpp = 99u;
    size_t count = 123u;
    uint8_t *output = (uint8_t *)(uintptr_t)1u;
    const bool accepted = jce__ktx_parse_cubemap(bytes, length, &edge, &mips,
                                                &output, &count, &bpp);
    if (accepted) parsed = output;
    TEST_ASSERT_FALSE_MESSAGE(accepted, "Malformed container must fail preflight");
    TEST_ASSERT_EQUAL_UINT32(77u, edge);
    TEST_ASSERT_EQUAL_UINT32(88u, mips);
    TEST_ASSERT_EQUAL_UINT32(99u, bpp);
    TEST_ASSERT_EQUAL_UINT(123u, count);
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)1u, output);
}

static void assert_pixels(uint32_t bpp)
{
    size_t mip_source = 0u;
    const size_t per_face = (16u + 4u + 1u) * bpp;
    size_t side_offset = 0u;
    for (uint32_t mip = 0; mip < 3u; ++mip) {
        const size_t face_bytes = (size_t)(4u >> mip) * (4u >> mip) * bpp;
        for (uint32_t face = 0; face < 6u; ++face)
            TEST_ASSERT_EQUAL_MEMORY(source + mip_source + face * face_bytes,
                parsed + face * per_face + side_offset, face_bytes);
        mip_source += face_bytes * 6u;
        side_offset += face_bytes;
    }
}

static void test_real_writer_has_typed_header_and_exact_chain(void)
{
    for (uint32_t bpp = 4u; bpp <= 8u; bpp += 4u) {
        const size_t expected = make_cube(bpp);
        TEST_ASSERT_TRUE(jce__ktx2_write_cubemap("cube.ktx", 4u, 3u, source, bpp));
        uint64_t length = 0u;
        read_file = jce_fs_host_read_all("cube.ktx", &length);
        TEST_ASSERT_NOT_NULL(read_file);
        TEST_ASSERT_EQUAL_UINT64(expected, length);
        TEST_ASSERT_EQUAL_HEX32(bpp == 8u ? 0x140bu : 0x1401u,
                              read_le((const uint8_t *)read_file + 16u));
        TEST_ASSERT_EQUAL_UINT32(bpp == 8u ? 2u : 1u,
                                read_le((const uint8_t *)read_file + 20u));
        TEST_ASSERT_EQUAL_HEX32(0x1908u, read_le((const uint8_t *)read_file + 24u));
        TEST_ASSERT_EQUAL_MEMORY(wire, read_file, expected);
        uint32_t edge = 0u, mips = 0u, format = 0u;
        size_t count = 0u;
        TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(read_file, (size_t)length,
                           &edge, &mips, &parsed, &count, &format));
        TEST_ASSERT_EQUAL_UINT32(4u, edge);
        TEST_ASSERT_EQUAL_UINT32(3u, mips);
        TEST_ASSERT_EQUAL_UINT32(bpp, format);
        TEST_ASSERT_EQUAL_UINT(126u * bpp, count);
        assert_pixels(bpp);
        jce_free(parsed); parsed = NULL;
        jce_fs_buffer_free(read_file); read_file = NULL;
    }
}

static void test_legacy_exact_header_and_typed_pixels_remain_readable(void)
{
    for (uint32_t bpp = 4u; bpp <= 8u; bpp += 4u) {
        const size_t length = make_cube(bpp);
        write_le(wire + 16u, 0u);
        write_le(wire + 20u, 1u);
        write_le(wire + 24u, 0u);
        TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(wire, length, NULL, NULL,
                                              &parsed, NULL, NULL));
        assert_pixels(bpp);
        jce_free(parsed); parsed = NULL;
    }
}

static void test_shape_and_format_fields_are_not_silently_ignored(void)
{
    static const uint32_t changes[][2] = {
        {16u, 0u}, {20u, 2u}, {24u, 0u}, {28u, 0x8051u},
        {32u, 0x1907u}, {40u, 3u}, {44u, 1u}, {48u, 1u}, {52u, 1u}
    };
    for (size_t index = 0; index < sizeof(changes) / sizeof(changes[0]); ++index) {
        const size_t length = make_cube(4u);
        write_le(wire + changes[index][0], changes[index][1]);
        assert_rejected(wire, length);
    }
    const size_t length = make_cube(8u);
    write_le(wire + 20u, 1u);
    assert_rejected(wire, length);
}

static void test_implicit_single_mip_and_aligned_metadata_remain_supported(void)
{
    make_cube(4u);
    memmove(wire + 72u, wire + 552u, 28u);
    write_le(wire + 36u, 1u);
    write_le(wire + 40u, 1u);
    write_le(wire + 56u, 0u);
    write_le(wire + 60u, 8u);
    write_le(wire + 64u, 4u);
    memcpy(wire + 68u, "a\0b\0", 4u);
    uint32_t edge = 0u, mips = 0u;
    TEST_ASSERT_TRUE(jce__ktx_parse_cubemap(wire, 100u, &edge, &mips,
                                          &parsed, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, edge);
    TEST_ASSERT_EQUAL_UINT32(1u, mips);
    TEST_ASSERT_EQUAL_MEMORY(source + 480u, parsed, 24u);
}

static void test_size_mip_and_payload_limits_refuse_before_allocation(void)
{
    static const uint32_t changes[][2] = {
        {36u, 0u}, {36u, 65536u}, {36u, UINT32_MAX},
        {56u, 4u}, {56u, 32u}, {56u, UINT32_MAX}, {60u, 1u},
        {60u, UINT32_MAX}, {64u, UINT32_MAX}
    };
    for (size_t index = 0; index < sizeof(changes) / sizeof(changes[0]); ++index) {
        const size_t length = make_cube(8u);
        write_le(wire + changes[index][0], changes[index][1]);
        assert_rejected(wire, length);
    }
    const size_t length = make_cube(8u);
    assert_rejected(wire, length - 1u);
    assert_rejected(wire, length + 1u);
    assert_rejected(wire, 63u);
    assert_rejected(NULL, length);
    TEST_ASSERT_FALSE(jce__ktx_parse_cubemap(wire, length, NULL, NULL,
                                           NULL, NULL, NULL));
    make_cube(8u);
    write_le(wire + 36u, 32768u);
    write_le(wire + 40u, 32768u);
    assert_rejected(wire, length);
    const size_t full_length = make_cube(4u);
    write_le(wire + 56u, 2u);
    assert_rejected(wire, full_length - 4u - 6u * 4u);
}

static void test_writer_refuses_narrowing_and_excess_mips_before_reading(void)
{
    const uint8_t one = 0u;
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 65536u, 1u, &one, 4u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 4u, 256u, &one, 4u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 4u, 4u, &one, 4u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 32768u, 1u, &one, 8u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 1u, 1u, &one, 7u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("", 1u, 1u, &one, 4u));
    make_cube(4u);
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 4u, 2u, source, 4u));
    TEST_ASSERT_FALSE(jce__ktx2_write_cubemap("invalid.ktx", 8192u, 14u, &one, 4u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_writer_has_typed_header_and_exact_chain);
    RUN_TEST(test_legacy_exact_header_and_typed_pixels_remain_readable);
    RUN_TEST(test_shape_and_format_fields_are_not_silently_ignored);
    RUN_TEST(test_implicit_single_mip_and_aligned_metadata_remain_supported);
    RUN_TEST(test_size_mip_and_payload_limits_refuse_before_allocation);
    RUN_TEST(test_writer_refuses_narrowing_and_excess_mips_before_reading);
    return UNITY_END();
}
