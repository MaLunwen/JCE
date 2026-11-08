/* Actual runtime pause TU and real device-free miniaudio voice generations.
 * No OS device, native focus event, speaker output or GPU evidence. */
#include "unity.h"
#include "application/jce_rt_internal.h"
#include <string.h>

static JceRuntime runtime;
static VoiceEntry entries[3];
static int16_t pcm[4800];
static JceSound sound;

void setUp(void)
{
    memset(&runtime, 0, sizeof runtime);
    memset(entries, 0, sizeof entries);
    for (unsigned i = 0; i < 4800; ++i) pcm[i] = (int16_t)((i % 64u) * 256u);
    runtime.audio = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(runtime.audio);
    sound = jce_audio_load_pcm(runtime.audio, pcm, sizeof pcm, 1u, 48000u, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, sound);
    runtime.voices = entries;
    runtime.voice_cap = 3;
}
void tearDown(void)
{
    if (runtime.audio) jce_audio_destroy(runtime.audio);
    memset(&runtime, 0, sizeof runtime);
}
static JceVoice add_voice(void)
{
    const JceVoice voice = jce_audio_play(runtime.audio, sound, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, voice);
    TEST_ASSERT_TRUE(jce_audio_is_playing(runtime.audio, voice));
    return voice;
}
static JceVoice bind_voice(int index)
{
    const JceVoice voice = add_voice();
    entries[index].voice = voice;
    entries[index].sound = sound;
    entries[index].paused_by_runtime = JCE_VOICE_INVALID;
    if (runtime.voice_count <= index) runtime.voice_count = index + 1;
    return voice;
}
static void test_user_paused_voice_is_never_acquired_or_resumed(void)
{
    const JceVoice manual = bind_voice(0), playing = bind_voice(1);
    jce_audio_pause(runtime.audio, manual);
    jce_runtime_pause_audio(&runtime);
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
    TEST_ASSERT_EQUAL_UINT32(playing, entries[1].paused_by_runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, playing));
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, manual));
    TEST_ASSERT_TRUE(jce_audio_is_playing(runtime.audio, playing));
}
static void test_repeated_pause_and_resume_release_each_record_once(void)
{
    const JceVoice voice = bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    jce_runtime_pause_audio(&runtime);
    TEST_ASSERT_EQUAL_UINT32(voice, entries[0].paused_by_runtime);
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_TRUE(jce_audio_is_playing(runtime.audio, voice));
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
    jce_audio_pause(runtime.audio, voice);
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, voice));
}
static void test_stopped_generation_does_not_resume_recycled_paused_voice(void)
{
    const JceVoice old = bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    jce_audio_stop(runtime.audio, old);
    const JceVoice replacement = add_voice();
    TEST_ASSERT_EQUAL_UINT32(old & 0xffffu, replacement & 0xffffu);
    TEST_ASSERT_NOT_EQUAL(old, replacement);
    jce_audio_pause(runtime.audio, replacement);
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, old));
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, replacement));
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
}
static void test_changed_binding_does_not_resume_replacement_or_old_voice(void)
{
    const JceVoice old = bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    const JceVoice replacement = add_voice();
    jce_audio_pause(runtime.audio, replacement);
    entries[0].voice = replacement; /* A different full binding handle. */
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, old));
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, replacement));
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
}
static void test_unloaded_sound_invalidates_saved_voice(void)
{
    const JceVoice voice = bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    jce_audio_unload(runtime.audio, sound);
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, voice));
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
}
static void test_binding_move_keeps_ownership_with_full_handle(void)
{
    const JceVoice manual = bind_voice(0), owned = bind_voice(1);
    jce_audio_pause(runtime.audio, manual);
    jce_runtime_pause_audio(&runtime);
    const VoiceEntry temporary = entries[0];
    entries[0] = entries[1]; entries[1] = temporary;
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_TRUE(jce_audio_is_playing(runtime.audio, owned));
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, manual));
}
static void test_retired_runtime_does_not_resume_after_audio_destruction(void)
{
    bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    jce_audio_destroy(runtime.audio);
    runtime.audio = NULL;
    jce_runtime_resume_audio(&runtime);
    jce_runtime_pause_audio(&runtime);
    jce_runtime_resume_audio(NULL); jce_runtime_pause_audio(NULL);
}
static void test_new_pause_episode_only_acquires_currently_playing_voice(void)
{
    const JceVoice voice = bind_voice(0);
    jce_runtime_pause_audio(&runtime);
    jce_runtime_resume_audio(&runtime);
    jce_audio_pause(runtime.audio, voice); /* New user pause before next episode. */
    jce_runtime_pause_audio(&runtime);
    TEST_ASSERT_EQUAL_UINT32(JCE_VOICE_INVALID, entries[0].paused_by_runtime);
    jce_runtime_resume_audio(&runtime);
    TEST_ASSERT_FALSE(jce_audio_is_playing(runtime.audio, voice));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_user_paused_voice_is_never_acquired_or_resumed);
    RUN_TEST(test_repeated_pause_and_resume_release_each_record_once);
    RUN_TEST(test_stopped_generation_does_not_resume_recycled_paused_voice);
    RUN_TEST(test_changed_binding_does_not_resume_replacement_or_old_voice);
    RUN_TEST(test_unloaded_sound_invalidates_saved_voice);
    RUN_TEST(test_binding_move_keeps_ownership_with_full_handle);
    RUN_TEST(test_retired_runtime_does_not_resume_after_audio_destruction);
    RUN_TEST(test_new_pause_episode_only_acquires_currently_playing_voice);
    return UNITY_END();
}
