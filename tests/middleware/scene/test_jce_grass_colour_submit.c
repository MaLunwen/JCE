/* Exact production CPU admission owner; no shader/backend/GPU evidence. */
#include "unity.h"
#include "middleware/scene/jce_sr_grass_submit_internal.h"
#include <string.h>
static uint32_t budget, batch, emitted, next_offset, emissions;
static unsigned fail_emission;
static uint32_t available(uint32_t wanted,uint16_t stride,void *user)
{ (void)wanted; TEST_ASSERT_EQUAL_UINT16(80,stride); TEST_ASSERT_EQUAL_PTR(&budget,user); return budget<batch?budget:batch; }
static bool emit(uint32_t offset,uint32_t count,void *user)
{
    TEST_ASSERT_EQUAL_PTR(&budget,user); TEST_ASSERT_EQUAL_UINT32(next_offset,offset);
    if(++emissions==fail_emission) return false;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(budget,count);
    budget-=count; emitted+=count; next_offset+=count; return true;
}
static const JceGrassColourProvider provider={available,emit,&budget};
void setUp(void) {budget=100;batch=4;emitted=next_offset=emissions=0;fail_emission=0;}
void tearDown(void) {}
static void assert_receipt(JceGrassColourReceipt r,uint32_t asked,uint32_t accepted,unsigned reason)
{
    TEST_ASSERT_EQUAL_UINT32(asked,r.requested); TEST_ASSERT_EQUAL_UINT32(accepted,r.accepted);
    TEST_ASSERT_EQUAL_UINT32(asked-accepted,r.rejected); TEST_ASSERT_EQUAL_UINT(reason,r.reason);
    TEST_ASSERT_EQUAL_UINT32(accepted,emitted);
}
static void zero_request_is_legal_without_dispatch(void)
{assert_receipt(sr_grass_colour_dispatch(0,80,&provider),0,0,0);TEST_ASSERT_EQUAL_UINT32(0,emissions);}
static void complete_split_keeps_contiguous_accepted_offsets(void)
{assert_receipt(sr_grass_colour_dispatch(13,80,&provider),13,13,0);TEST_ASSERT_EQUAL_UINT32(4,emissions);}
static void no_capacity_reports_all_requested_as_rejected(void)
{budget=0;assert_receipt(sr_grass_colour_dispatch(17,80,&provider),17,0,1);TEST_ASSERT_EQUAL_UINT32(0,emissions);}
static void partial_capacity_preserves_accepted_and_rejected_tail(void)
{budget=7;batch=3;assert_receipt(sr_grass_colour_dispatch(10,80,&provider),10,7,1);TEST_ASSERT_EQUAL_UINT32(3,emissions);}
static void first_emission_failure_is_not_accepted(void)
{fail_emission=1;assert_receipt(sr_grass_colour_dispatch(10,80,&provider),10,0,2);}
static void later_emission_failure_preserves_prior_acceptance(void)
{batch=3;fail_emission=2;assert_receipt(sr_grass_colour_dispatch(10,80,&provider),10,3,2);}
static void invalid_ports_and_stride_fail_closed(void)
{
    assert_receipt(sr_grass_colour_dispatch(10,0,&provider),10,0,2);
    assert_receipt(sr_grass_colour_dispatch(10,80,NULL),10,0,2);
    TEST_ASSERT_EQUAL_UINT32(0,emissions);
}
static void overreported_capacity_cannot_exceed_request(void)
{batch=UINT32_MAX;budget=UINT32_MAX;assert_receipt(sr_grass_colour_dispatch(17,80,&provider),17,17,0);}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(zero_request_is_legal_without_dispatch);
    RUN_TEST(complete_split_keeps_contiguous_accepted_offsets);
    RUN_TEST(no_capacity_reports_all_requested_as_rejected);
    RUN_TEST(partial_capacity_preserves_accepted_and_rejected_tail);
    RUN_TEST(first_emission_failure_is_not_accepted);
    RUN_TEST(later_emission_failure_preserves_prior_acceptance);
    RUN_TEST(invalid_ports_and_stride_fail_closed);
    RUN_TEST(overreported_capacity_cannot_exceed_request);
    return UNITY_END();
}
