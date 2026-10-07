#include <jce/renderer/jce_gpu_scene.h>
#include <unity.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void unavailable_snapshot_cannot_expose_old_counts(void)
{
    JceGpuSceneSurvivorSnapshot snapshot;
    memset(&snapshot, 0x7f, sizeof(snapshot));
    TEST_ASSERT_FALSE(jce_gpu_scene_snapshot_survivors(NULL, 250, &snapshot));
    TEST_ASSERT_EQUAL_UINT64(0, snapshot.dispatch_serial);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.records);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.groups);
    TEST_ASSERT_EQUAL_UINT16(0, snapshot.width);
    TEST_ASSERT_EQUAL_UINT16(0, snapshot.height);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, snapshot.texture.idx);
    TEST_ASSERT_FALSE(jce_gpu_scene_snapshot_survivors(NULL, 250, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(unavailable_snapshot_cannot_expose_old_counts);
    return UNITY_END();
}
