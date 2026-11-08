/* Only the new actual no-audio lifecycle entries, not the historical backend. */
#include "unity.h"
#include "middleware/audio/jce_audio_master_lifecycle.inc.h"
void setUp(void) {}
void tearDown(void) {}
static void test_disabled_master_hooks_link_and_retire(void)
{
    TEST_ASSERT_FALSE(jce_audio_master_is_callback_internal());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    jce_audio_master_session_open_internal();
    jce_audio_master_session_close_internal();
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_disabled_master_hooks_link_and_retire);
    return UNITY_END();
}
