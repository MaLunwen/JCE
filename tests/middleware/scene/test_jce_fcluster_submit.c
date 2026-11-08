/* Exact production CPU preparation; no backend/shader/GPU claim. */
#include "unity.h"
#ifndef JCE_FCLUSTER_SUBMIT_HEADER
#define JCE_FCLUSTER_SUBMIT_HEADER "middleware/scene/jce_sr_fcluster_submit_internal.h"
#endif
#include JCE_FCLUSTER_SUBMIT_HEADER

static uint8_t input[128], output[128];
static uint32_t capacity, allocated;
static unsigned queries, allocations;
static bool null_data;
static uint32_t available(uint32_t wanted, uint16_t stride, void *user)
{
    TEST_ASSERT_EQUAL_UINT32(3, wanted);
    TEST_ASSERT_EQUAL_UINT16(32, stride);
    TEST_ASSERT_EQUAL_PTR(&capacity, user);
    ++queries;
    return capacity;
}
static JceFclusterAllocation allocate(uint32_t wanted, uint16_t stride, void *user)
{
    TEST_ASSERT_EQUAL_UINT32(3, wanted);
    TEST_ASSERT_EQUAL_UINT16(32, stride);
    TEST_ASSERT_EQUAL_PTR(&capacity, user);
    ++allocations;
    JceFclusterAllocation result = {null_data ? NULL : output, allocated};
    return result;
}
static const JceFclusterProvider provider = {available, allocate, &capacity};
void setUp(void)
{
    memset(input, 0x3c, sizeof(input));
    memset(output, 0xa5, sizeof(output));
    capacity = allocated = 3;
    queries = allocations = 0;
    null_data = false;
}
void tearDown(void) {}
static void assert_rejected(JceGrassColourReceipt receipt, unsigned reason)
{
    TEST_ASSERT_EQUAL_UINT32(3, receipt.requested);
    TEST_ASSERT_EQUAL_UINT32(0, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT32(3, receipt.rejected);
    TEST_ASSERT_EQUAL_UINT(reason, receipt.reason);
    for (size_t i = 0; i < sizeof(output); ++i)
        TEST_ASSERT_EQUAL_UINT8(0xa5, output[i]);
}
static void zero_request_needs_no_provider_or_allocation(void)
{
    const JceGrassColourReceipt receipt = sr_fcluster_prepare_instances(0, 0, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, receipt.requested);
    TEST_ASSERT_EQUAL_UINT32(0, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT32(0, receipt.rejected);
    TEST_ASSERT_EQUAL_UINT(0, receipt.reason);
    TEST_ASSERT_EQUAL_UINT(0, queries);
    TEST_ASSERT_EQUAL_UINT(0, allocations);
}
static void capacity_shortfall_rejects_whole_cluster_before_allocation(void)
{
    capacity = 2;
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, &provider), JCE_FCLUSTER_CAPACITY);
    TEST_ASSERT_EQUAL_UINT(1, queries);
    TEST_ASSERT_EQUAL_UINT(0, allocations);
}
static void zero_capacity_is_not_partial_acceptance(void)
{
    capacity = 0;
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, &provider), JCE_FCLUSTER_CAPACITY);
    TEST_ASSERT_EQUAL_UINT(0, allocations);
}
static void null_allocation_is_rejected_before_copy(void)
{
    null_data = true;
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, &provider), JCE_FCLUSTER_ALLOCATION_NULL);
    TEST_ASSERT_EQUAL_UINT(1, queries);
    TEST_ASSERT_EQUAL_UINT(1, allocations);
}
static void short_allocation_is_rejected_before_copy(void)
{
    /* Physical storage stays bounded and large enough even for the old guard
     * mutant. Its wrong acceptance fails an ordinary assertion, never crashes. */
    allocated = 2;
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, &provider), JCE_FCLUSTER_ALLOCATION_SHORT);
    TEST_ASSERT_EQUAL_UINT(1, allocations);
}
static void full_allocation_prepares_exactly_requested_bytes(void)
{
    const JceGrassColourReceipt receipt = sr_fcluster_prepare_instances(3, 32, input, &provider);
    TEST_ASSERT_EQUAL_UINT32(3, receipt.requested);
    TEST_ASSERT_EQUAL_UINT32(3, receipt.accepted);
    TEST_ASSERT_EQUAL_UINT32(0, receipt.rejected);
    TEST_ASSERT_EQUAL_UINT(0, receipt.reason);
    TEST_ASSERT_EQUAL_UINT(1, queries);
    TEST_ASSERT_EQUAL_UINT(1, allocations);
    TEST_ASSERT_EQUAL_MEMORY(input, output, 96);
    for (size_t i = 96; i < sizeof(output); ++i) TEST_ASSERT_EQUAL_UINT8(0xa5, output[i]);
}
static void larger_capacity_and_allocation_do_not_copy_extra_bytes(void)
{
    capacity = allocated = 4;
    full_allocation_prepares_exactly_requested_bytes();
}
static void invalid_source_stride_or_ports_never_allocate(void)
{
    assert_rejected(sr_fcluster_prepare_instances(3, 0, input, &provider), JCE_FCLUSTER_INVALID_INPUT);
    assert_rejected(sr_fcluster_prepare_instances(3, 32, NULL, &provider), JCE_FCLUSTER_INVALID_INPUT);
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, NULL), JCE_FCLUSTER_INVALID_INPUT);
    JceFclusterProvider invalid = {available, NULL, &capacity};
    assert_rejected(sr_fcluster_prepare_instances(3, 32, input, &invalid), JCE_FCLUSTER_INVALID_INPUT);
    TEST_ASSERT_EQUAL_UINT(0, queries);
    TEST_ASSERT_EQUAL_UINT(0, allocations);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(zero_request_needs_no_provider_or_allocation);
    RUN_TEST(capacity_shortfall_rejects_whole_cluster_before_allocation);
    RUN_TEST(zero_capacity_is_not_partial_acceptance);
    RUN_TEST(null_allocation_is_rejected_before_copy);
    RUN_TEST(short_allocation_is_rejected_before_copy);
    RUN_TEST(full_allocation_prepares_exactly_requested_bytes);
    RUN_TEST(larger_capacity_and_allocation_do_not_copy_extra_bytes);
    RUN_TEST(invalid_source_stride_or_ports_never_allocate);
    return UNITY_END();
}
