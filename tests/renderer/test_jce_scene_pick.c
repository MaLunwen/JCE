/*
 * test_jce_scene_pick.c
 *
 * Pure protocol tests for GPU object-ID picking.  No bgfx context is
 * required here: the render pass uses the same encode/decode contract.
 */

#include <jce/renderer/jce_pick_id.h>

#include <stdint.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_pick_id_zero_decodes_to_no_hit(void)
{
    uint8_t rgba[4] = { 0, 0, 0, 255 };
    TEST_ASSERT_EQUAL_UINT64(0, jce_scene_pick_decode_rgba(rgba));
}

static void test_pick_id_round_trips_24_bit_entities(void)
{
    const uint64_t ids[] = {
        1u,
        0x42u,
        0x010203u,
        0x00FFFFFEu,
        0x00FFFFFFu,
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint8_t rgba[4] = { 0, 0, 0, 0 };
        TEST_ASSERT_TRUE(jce_scene_pick_encode_rgba(ids[i], rgba));
        TEST_ASSERT_EQUAL_UINT64(ids[i], jce_scene_pick_decode_rgba(rgba));
        TEST_ASSERT_EQUAL_UINT8(255u, rgba[3]);
    }
}

static void test_pick_id_rejects_zero_and_overflow(void)
{
    uint8_t rgba[4];
    memset(rgba, 0xAA, sizeof(rgba));

    TEST_ASSERT_FALSE(jce_scene_pick_encode_rgba(0, rgba));
    TEST_ASSERT_EQUAL_UINT8(0xAAu, rgba[0]);

    TEST_ASSERT_FALSE(jce_scene_pick_encode_rgba(0x01000000u, rgba));
    TEST_ASSERT_EQUAL_UINT8(0xAAu, rgba[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pick_id_zero_decodes_to_no_hit);
    RUN_TEST(test_pick_id_round_trips_24_bit_entities);
    RUN_TEST(test_pick_id_rejects_zero_and_overflow);
    return UNITY_END();
}
