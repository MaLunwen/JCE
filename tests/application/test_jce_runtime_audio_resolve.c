#include "application/jce_rt_internal.h"

#include "unity.h"

#include <stdint.h>
#include <string.h>

typedef struct AudioProbe {
    int calls;
    const char *expected_path;
    JceSound result;
} AudioProbe;

static uint32_t probe_load(void *user, JceAudio *audio, const char *path)
{
    AudioProbe *probe = (AudioProbe *)user;
    TEST_ASSERT_NOT_NULL(audio);
    TEST_ASSERT_EQUAL_STRING(probe->expected_path, path);
    probe->calls++;
    return probe->result;
}

void setUp(void) {}
void tearDown(void) {}

static void test_host_resolver_is_the_canonical_sync_load_path(void)
{
    AudioProbe probe = {0, "audio/thunder.mp3", 77u};
    JceRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.audio = (JceAudio *)(uintptr_t)1;
    runtime.audio_load_fn = probe_load;
    runtime.user_data = &probe;

    TEST_ASSERT_EQUAL_UINT32(77u,
        rt_load_sound(&runtime, "audio/thunder.mp3"));
    TEST_ASSERT_EQUAL_INT(1, probe.calls);
}

static void test_missing_runtime_audio_fails_closed(void)
{
    JceRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    TEST_ASSERT_EQUAL_UINT32(JCE_SOUND_INVALID,
                             rt_load_sound(&runtime, "audio/missing.mp3"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_host_resolver_is_the_canonical_sync_load_path);
    RUN_TEST(test_missing_runtime_audio_fails_closed);
    return UNITY_END();
}
