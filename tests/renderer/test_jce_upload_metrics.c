#include "renderer/jce_upload_metrics.h"
#include "unity.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}

static void test_admission_and_categories(void)
{
    JceSubmittedPayloadStats stats;
    jce_upload_metrics_reset(&stats, 17);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_VERTEX, 64, false);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_VERTEX, 0, true);
    TEST_ASSERT_EQUAL_UINT64(0, stats.static_vertex_bytes);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_VERTEX, 64, true);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_INDEX, 12, true);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_TEXTURE_CREATE, 84, true);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_TEXTURE_UPDATE, 16, true);
    jce_upload_metrics_add(&stats, (JceUploadPayloadKind)99, 900, true);
    TEST_ASSERT_EQUAL_UINT64(64, stats.static_vertex_bytes);
    TEST_ASSERT_EQUAL_UINT64(12, stats.static_index_bytes);
    TEST_ASSERT_EQUAL_UINT64(84, stats.texture_create_bytes);
    TEST_ASSERT_EQUAL_UINT64(16, stats.texture_update_bytes);
}
static void test_overflow_is_explicit_and_independent(void)
{
    JceSubmittedPayloadStats stats;
    jce_upload_metrics_reset(&stats, 1);
    stats.static_index_bytes = UINT64_MAX - 2;
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_INDEX, 3, true);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, stats.static_index_bytes);
    TEST_ASSERT_TRUE(stats.saturated);
    jce_upload_metrics_add(&stats, JCE_UPLOAD_STATIC_VERTEX, 9, true);
    TEST_ASSERT_EQUAL_UINT64(9, stats.static_vertex_bytes);
    jce_upload_metrics_reset(&stats, 2);
    TEST_ASSERT_FALSE(stats.saturated);
    TEST_ASSERT_EQUAL_UINT64(0, stats.static_index_bytes);
}
static void test_context_binding_refuses_replacement_and_resets(void)
{
    JceSubmittedPayloadStats a, b;
    JceUploadMetricsBridge bridge = {0};
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    TEST_ASSERT_FALSE(jce_upload_metrics_bind(&bridge, NULL));
    TEST_ASSERT_TRUE(jce_upload_metrics_bind(&bridge, &a));
    uint64_t first = a.context_generation;
    TEST_ASSERT_FALSE(jce_upload_metrics_bind(&bridge, &b));
    jce_upload_metrics_unbind(&bridge, &b);
    jce_upload_metrics_add(bridge.active, JCE_UPLOAD_STATIC_VERTEX, 41, true);
    TEST_ASSERT_EQUAL_UINT64(41, a.static_vertex_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, b.static_vertex_bytes);
    jce_upload_metrics_unbind(&bridge, &a);
    jce_upload_metrics_add(bridge.active, JCE_UPLOAD_STATIC_VERTEX, 99, true);
    TEST_ASSERT_EQUAL_UINT64(41, a.static_vertex_bytes);
    TEST_ASSERT_FALSE(a.valid);
    TEST_ASSERT_TRUE(jce_upload_metrics_bind(&bridge, &a));
    TEST_ASSERT_TRUE(a.context_generation > first);
    TEST_ASSERT_EQUAL_UINT64(0, a.static_vertex_bytes);
    jce_upload_metrics_unbind(&bridge, &a);
    jce_upload_metrics_unbind(&bridge, NULL);
    bridge.generation = UINT64_MAX;
    TEST_ASSERT_FALSE(jce_upload_metrics_bind(&bridge, &b));
    TEST_ASSERT_NULL(bridge.active);
    TEST_ASSERT_FALSE(jce_upload_metrics_bind(NULL, &b));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_admission_and_categories);
    RUN_TEST(test_overflow_is_explicit_and_independent);
    RUN_TEST(test_context_binding_refuses_replacement_and_resets);
    return UNITY_END();
}
