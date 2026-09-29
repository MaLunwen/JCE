#include <jce/renderer/jce_render_readback.h>

#include <unity.h>

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_default_descriptor_is_invalid_until_authored(void)
{
    JceRenderReadbackDesc desc = jce_render_readback_desc_default();

    TEST_ASSERT_EQUAL_UINT32(sizeof(desc), desc.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_READBACK_ABI_VERSION, desc.version);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, desc.source.idx);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_FORMAT_RGBA32F, desc.format);
    TEST_ASSERT_EQUAL_UINT32(0, desc.width);
    TEST_ASSERT_EQUAL_UINT32(0, desc.height);
}

static void test_typed_layout_is_exact_and_overflow_checked(void)
{
    uint64_t row_pitch = 0;
    uint64_t byte_size = 0;

    TEST_ASSERT_TRUE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, 64, 64, &row_pitch, &byte_size));
    TEST_ASSERT_EQUAL_UINT64(64u * 16u, row_pitch);
    TEST_ASSERT_EQUAL_UINT64(64u * 64u * 16u, byte_size);

    TEST_ASSERT_TRUE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_R16F, 7, 5, &row_pitch, &byte_size));
    TEST_ASSERT_EQUAL_UINT64(14, row_pitch);
    TEST_ASSERT_EQUAL_UINT64(70, byte_size);

    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_DEPTH24_STENCIL8, 1, 1,
        &row_pitch, &byte_size));
    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, 0, 1, &row_pitch, &byte_size));
    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, UINT32_MAX, UINT32_MAX,
        &row_pitch, &byte_size));
}

static void test_invalid_ticket_operations_are_total(void)
{
    JceRenderReadbackInfo info;
    unsigned char dst[16];

    memset(&info, 0xa5, sizeof(info));
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_INVALID,
        jce_render_readback_poll(NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_FALSE(jce_render_readback_get_info(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET, &info));
    TEST_ASSERT_FALSE(jce_render_readback_copy(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET, dst, sizeof(dst)));
    TEST_ASSERT_FALSE(jce_render_readback_cancel(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_FALSE(jce_render_readback_release(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_EQUAL_UINT32(4, JCE_RENDER_READBACK_QUEUE_CAPACITY);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_descriptor_is_invalid_until_authored);
    RUN_TEST(test_typed_layout_is_exact_and_overflow_checked);
    RUN_TEST(test_invalid_ticket_operations_are_total);
    return UNITY_END();
}
