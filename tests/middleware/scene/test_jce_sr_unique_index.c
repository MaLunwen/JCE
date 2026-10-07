#include "jce_sr_unique_index.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_duplicate_repairs_preserve_full_build_membership(void)
{
    uint32_t indices[4096];
    uint64_t stamps[4096] = {0};
    uint32_t count = 0;
    uint32_t i;
    for (i = 0; i < 4096; ++i)
        TEST_ASSERT_TRUE(sr_unique_index_append(indices, &count, 4096, i,
                                                7, &stamps[i]));
    for (i = 4096; i > 0; --i)
        TEST_ASSERT_TRUE(sr_unique_index_append(indices, &count, 4096, i - 1,
                                                7, &stamps[i - 1]));
    TEST_ASSERT_EQUAL_UINT32(4096, count);
    for (i = 0; i < count; ++i)
        TEST_ASSERT_EQUAL_UINT32(i, indices[i]);

    /* A new collected list can reuse these slots in a different order. */
    count = 0;
    for (i = 4096; i > 0; --i)
        TEST_ASSERT_TRUE(sr_unique_index_append(indices, &count, 4096, i - 1,
                                                8, &stamps[i - 1]));
    TEST_ASSERT_EQUAL_UINT32(4096, count);
    TEST_ASSERT_EQUAL_UINT32(4095, indices[0]);
}

static void test_capacity_failure_does_not_poison_retry(void)
{
    uint32_t indices[2] = {0};
    uint32_t count = 0;
    uint64_t stamp = 0;
    TEST_ASSERT_FALSE(sr_unique_index_append(indices, &count, 0, 42, 1, &stamp));
    TEST_ASSERT_EQUAL_UINT64(0, stamp);
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_TRUE(sr_unique_index_append(indices, &count, 2, 42, 1, &stamp));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_UINT32(42, indices[0]);
    TEST_ASSERT_EQUAL_UINT64(1, stamp);
}

static void test_invalid_epoch_and_storage_are_rejected(void)
{
    uint32_t indices[1] = {0};
    uint32_t count = 0;
    uint64_t stamp = 0;
    TEST_ASSERT_FALSE(sr_unique_index_append(indices, &count, 1, 0, 0, &stamp));
    TEST_ASSERT_FALSE(sr_unique_index_append(NULL, &count, 1, 0, 1, &stamp));
    count = 2;
    TEST_ASSERT_FALSE(sr_unique_index_append(indices, &count, 1, 0, 1, &stamp));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_duplicate_repairs_preserve_full_build_membership);
    RUN_TEST(test_capacity_failure_does_not_poison_retry);
    RUN_TEST(test_invalid_epoch_and_storage_are_rejected);
    return UNITY_END();
}
