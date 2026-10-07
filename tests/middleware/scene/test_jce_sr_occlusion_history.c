#include "unity.h"
#include "jce_sr_occlusion_history.h"

void setUp(void) {}
void tearDown(void) {}
static const int scene_a = 1, scene_b = 2, culler_a = 3, culler_b = 4;

static SrOcclusionHistoryKey key(void)
{
    SrOcclusionHistoryKey value = {0};
    value.metadata = (SrMetadataKey){&scene_a, 4, 5, 6, 7};
    value.culler = &culler_a;
    value.transform_generation = 8;
    value.width = 1280;
    value.height = 720;
    value.frame_buffer = 2;
    for (int i = 0; i < 16; ++i) value.view[i] = value.projection[i] = i % 5 == 0 ? 1 : 0;
    return value;
}

static void test_static_history_reused_but_every_visibility_change_invalidates(void)
{
    SrOcclusionHistory history = {0};
    SrOcclusionHistoryKey initial = key(), changed;
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &initial));
    TEST_ASSERT_FALSE(sr_occlusion_history_update(&history, &initial));
#define EXPECT_INVALIDATION(edit) do { \
    changed = initial; edit; \
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &changed)); \
    TEST_ASSERT_FALSE(sr_occlusion_history_update(&history, &changed)); \
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &initial)); \
} while (0)
    EXPECT_INVALIDATION(changed.metadata.scene = &scene_b);
    EXPECT_INVALIDATION(++changed.metadata.list_generation);
    EXPECT_INVALIDATION(++changed.metadata.structural_generation);
    EXPECT_INVALIDATION(++changed.metadata.enable_generation);
    EXPECT_INVALIDATION(++changed.metadata.component_generation);
    EXPECT_INVALIDATION(changed.culler = &culler_b);
    EXPECT_INVALIDATION(++changed.transform_generation);
    EXPECT_INVALIDATION(++changed.content_generation);
    EXPECT_INVALIDATION(++changed.width);
    EXPECT_INVALIDATION(++changed.height);
    EXPECT_INVALIDATION(++changed.camera_mask);
    EXPECT_INVALIDATION(++changed.frame_buffer);
    EXPECT_INVALIDATION(++changed.view_id);
    for (int i = 0; i < 16; ++i) {
        EXPECT_INVALIDATION(changed.view[i] += 0.001f);
        EXPECT_INVALIDATION(changed.projection[i] += 0.001f);
    }
#undef EXPECT_INVALIDATION
    history.valid = false;
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &initial));
}

static void test_invalid_identity_cannot_prove_reusable_depth(void)
{
    SrOcclusionHistory history = {0};
    SrOcclusionHistoryKey value = key();
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &value));
    TEST_ASSERT_TRUE(sr_occlusion_history_update(NULL, &value));
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, NULL));
    TEST_ASSERT_FALSE(history.valid);
    value.metadata.scene = NULL;
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &value));
    TEST_ASSERT_FALSE(history.valid);
    value = key(); value.culler = NULL;
    TEST_ASSERT_TRUE(sr_occlusion_history_update(&history, &value));
    TEST_ASSERT_FALSE(history.valid);
}

static void test_depth_history_requires_a_stable_producer_and_no_deformation(void)
{
    SrOcclusionHistory history = {0};
    SrOcclusionHistoryKey value = key();
    TEST_ASSERT_FALSE(sr_depth_history_reusable(&history, &value, false));
    TEST_ASSERT_TRUE(sr_depth_history_reusable(&history, &value, false));
    ++value.transform_generation; /* moving an occluder reveals immediately */
    TEST_ASSERT_FALSE(sr_depth_history_reusable(&history, &value, false));
    TEST_ASSERT_TRUE(sr_depth_history_reusable(&history, &value, false));
    TEST_ASSERT_FALSE(sr_depth_history_reusable(&history, &value, true));
    TEST_ASSERT_FALSE(history.valid);
    TEST_ASSERT_FALSE(sr_depth_history_reusable(&history, &value, false));
    TEST_ASSERT_TRUE(sr_depth_history_reusable(&history, &value, false));
    history.valid = false; /* replaced target, even if a handle index is reused */
    TEST_ASSERT_FALSE(sr_depth_history_reusable(&history, &value, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_static_history_reused_but_every_visibility_change_invalidates);
    RUN_TEST(test_invalid_identity_cannot_prove_reusable_depth);
    RUN_TEST(test_depth_history_requires_a_stable_producer_and_no_deformation);
    return UNITY_END();
}
