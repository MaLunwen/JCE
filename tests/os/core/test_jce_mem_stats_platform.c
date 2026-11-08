/* Actual public mem_stats function, compiled twice with typed provider ports.
 * Web=1 models provider qualification, not a real Web architecture or browser.
 * Native OS counters and browser execution require separate artifact evidence. */
#include "unity.h"
#include <jce/os/core/jce_allocator.h>
#include <string.h>
#include <stdint.h>

static size_t reported[8];
static unsigned process_info_calls;
/* Exact mi_process_info ABI: elapsed/user/system/RSS/peak/commit/peak/faults. */
static void counted_process_info(size_t *elapsed, size_t *user, size_t *system,
    size_t *rss, size_t *peak_rss, size_t *commit, size_t *peak_commit, size_t *faults)
{
    ++process_info_calls;
    *elapsed=reported[0]; *user=reported[1]; *system=reported[2];
    *rss=reported[3]; *peak_rss=reported[4];
    *commit=reported[5]; *peak_commit=reported[6]; *faults=reported[7];
}

/* Public declarations precede typed owner substitutions. Only the extracted
 * function's platform qualification is modeled; allocator implementations are
 * neither copied nor overridden. */
#undef JCE_PLATFORM_WEB
#define JCE_PLATFORM_WEB 0
#define mi_process_info counted_process_info
#define jce_mem_stats native_mem_stats
#include JCE_TEST_MEM_STATS_OWNER
#undef jce_mem_stats
#undef JCE_PLATFORM_WEB
#define JCE_PLATFORM_WEB 1
#define jce_mem_stats web_mem_stats
#include JCE_TEST_MEM_STATS_OWNER
#undef jce_mem_stats
#undef mi_process_info

void setUp(void)
{
    process_info_calls=0;
    reported[0]=91;reported[1]=17;reported[2]=23;
    reported[3]=SIZE_MAX-64;reported[4]=SIZE_MAX-32;
    reported[5]=SIZE_MAX-128;reported[6]=SIZE_MAX-16;reported[7]=29;
}
void tearDown(void) {}
static void expect_web_unavailable(const JceMemStats *s)
{
    TEST_ASSERT_EQUAL_UINT64(0,s->current_rss);
    TEST_ASSERT_EQUAL_UINT64(0,s->peak_rss);
    TEST_ASSERT_EQUAL_UINT64(0,s->current_commit);
    TEST_ASSERT_EQUAL_UINT64(0,s->peak_commit);
}
static void test_native_provider_snapshot_retains_all_public_values(void)
{
    JceMemStats s;memset(&s,0xa5,sizeof(s));
    TEST_ASSERT_TRUE(native_mem_stats(&s));
    TEST_ASSERT_EQUAL_UINT64(reported[3],s.current_rss);
    TEST_ASSERT_EQUAL_UINT64(reported[4],s.peak_rss);
    TEST_ASSERT_EQUAL_UINT64(reported[5],s.current_commit);
    TEST_ASSERT_EQUAL_UINT64(reported[6],s.peak_commit);
    TEST_ASSERT_EQUAL_UINT64(reported[7],s.page_faults);
    TEST_ASSERT_EQUAL_UINT64(reported[0],s.elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(1,process_info_calls);
}
static void test_web_positive_allocator_estimates_are_not_process_memory(void)
{
    JceMemStats s;memset(&s,0xa5,sizeof(s));
    TEST_ASSERT_TRUE(web_mem_stats(&s));
    expect_web_unavailable(&s);
    TEST_ASSERT_EQUAL_UINT64(reported[7],s.page_faults);
    TEST_ASSERT_EQUAL_UINT64(reported[0],s.elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(1,process_info_calls);
}
static void test_null_output_refuses_before_either_provider_query(void)
{
    TEST_ASSERT_FALSE(native_mem_stats(NULL));
    TEST_ASSERT_FALSE(web_mem_stats(NULL));
    TEST_ASSERT_EQUAL_UINT32(0,process_info_calls);
}
static void test_web_snapshot_cannot_retain_previous_native_memory(void)
{
    JceMemStats s;
    TEST_ASSERT_TRUE(native_mem_stats(&s));
    TEST_ASSERT_TRUE(s.current_rss>0 && s.current_commit>0);
    reported[0]=27;reported[7]=1;
    TEST_ASSERT_TRUE(web_mem_stats(&s));
    expect_web_unavailable(&s);
    TEST_ASSERT_EQUAL_UINT64(1,s.page_faults);
    TEST_ASSERT_EQUAL_UINT64(27,s.elapsed_ms);
    TEST_ASSERT_EQUAL_UINT32(2,process_info_calls);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_native_provider_snapshot_retains_all_public_values);
    RUN_TEST(test_web_positive_allocator_estimates_are_not_process_memory);
    RUN_TEST(test_null_output_refuses_before_either_provider_query);
    RUN_TEST(test_web_snapshot_cannot_retain_previous_native_memory);
    return UNITY_END();
}
