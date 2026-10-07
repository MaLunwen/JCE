#include "unity.h"
#include "jce_sr_metadata_key.h"

void setUp(void) {}
void tearDown(void) {}

static const int scene_a = 1, scene_b = 2;

static void test_only_identical_live_metadata_can_be_reused(void)
{
    SrMetadataKey key = {&scene_a, 8, 12, 3, 19};
    SrMetadataKey changed = key;
    TEST_ASSERT_TRUE(sr_metadata_key_matches(&key, &changed));
    TEST_ASSERT_FALSE(sr_metadata_key_matches(NULL, &key));
    changed.scene = NULL;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
    changed.scene = &scene_b;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
    changed = key; ++changed.list_generation;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
    changed = key; ++changed.structural_generation;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
    changed = key; ++changed.enable_generation;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
    changed = key; ++changed.component_generation;
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&key, &changed));
}

static void test_generation_fields_must_not_be_folded_into_a_collision(void)
{
    SrMetadataKey a = {&scene_a, 8, 12, 3, 1000003};
    SrMetadataKey b = a;
    ++b.enable_generation;
    b.component_generation = 0;
    /* These collide under enable * 1000003 + component. */
    TEST_ASSERT_FALSE(sr_metadata_key_matches(&a, &b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_only_identical_live_metadata_can_be_reused);
    RUN_TEST(test_generation_fields_must_not_be_folded_into_a_collision);
    return UNITY_END();
}
