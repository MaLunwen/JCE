/* Actual runtime async AudioSource owners with real Core task/barrier lifetime.
 * Scene/PCM/mixer ports do not certify decoding fidelity or audible output. */
#include "unity.h"
#include "application/jce_rt_internal.h"
#include <jce/os/core/jce_thread.h>

struct JceAudioCpu { char path[256]; };
static JceRuntime runtime;
static JceAsyncExecutor *executor;
static JceSemaphore *started, *release_decode;
static JceMutex *ledger_mutex;
static JceAudioSourceComponent sources[8];
static JceEntity entities[8];
static bool enabled[8];
static int cpu_created, cpu_freed, uploads, stops, unloads;
static int args_created, args_freed, premature_free;
static void *quarantine[128];
static int quarantine_count;
static char uploaded_path[256];
static int scene_cookie, audio_cookie, pak_cookie;

static JceAsyncExecutor *selected_executor(void) { return executor; }
static JceAudioSourceComponent *source_lookup(JceScene *scene, JceEntity entity)
{
    (void)scene;
    for (int i = 0; i < 8; ++i)
        if (entity && entities[i] == entity) return &sources[i];
    return NULL;
}
static bool source_enabled(const JceScene *scene, JceEntity entity, uint64_t flag)
{
    (void)scene; (void)flag;
    for (int i = 0; i < 8; ++i)
        if (entity && entities[i] == entity) return enabled[i];
    return false;
}
static JceAudioCpu *decode_cpu(const JcePakArchive *pak, const char *path)
{
    (void)pak;
    jce_semaphore_signal(started);
    jce_semaphore_wait(release_decode);
    JceAudioCpu *cpu = (JceAudioCpu *)jce_malloc(sizeof *cpu);
    if (cpu) {
        snprintf(cpu->path, sizeof cpu->path, "%s", path);
        jce_mutex_lock(ledger_mutex); ++cpu_created; jce_mutex_unlock(ledger_mutex);
    }
    return cpu;
}
static void free_cpu(JceAudioCpu *cpu)
{
    if (!cpu) return;
    jce_mutex_lock(ledger_mutex); ++cpu_freed; jce_mutex_unlock(ledger_mutex);
    jce_free(cpu);
}
static JceSound upload_cpu(JceAudio *audio, JceAudioCpu *cpu)
{
    (void)audio;
    ++uploads;
    snprintf(uploaded_path, sizeof uploaded_path, "%s", cpu->path);
    free_cpu(cpu);
    return (JceSound)uploads;
}
static JceSound load_streaming(JceAudio *audio, const JcePakArchive *pak, const char *path)
{
    (void)audio; (void)pak; (void)path;
    return JCE_SOUND_INVALID;
}
static void stop_voice(JceAudio *audio, JceVoice voice)
{
    (void)audio; (void)voice; ++stops;
}
static void unload_sound(JceAudio *audio, JceSound sound)
{
    (void)audio; (void)sound; ++unloads;
}
static bool voice_playing(const JceAudio *audio, JceVoice voice)
{
    (void)audio; return voice != JCE_VOICE_INVALID;
}
void rt_finish_audio_source(JceRuntime *rt, JceScene *scene, JceEntity entity,
                                   JceSound sound, const JceAudioSourceComponent *source)
{
    (void)scene; (void)source;
    if (rt->voice_count == rt->voice_cap) {
        const int cap = rt->voice_cap ? 2 * rt->voice_cap : 8;
        VoiceEntry *voices = (VoiceEntry *)jce_realloc(rt->voices, (size_t)cap * sizeof *voices);
        if (!voices) return;
        rt->voices = voices; rt->voice_cap = cap;
    }
    VoiceEntry *v = &rt->voices[rt->voice_count++];
    memset(v, 0, sizeof *v);
    v->entity = entity; v->sound = sound; v->voice = (JceVoice)sound;
}
static void *pending_alloc(size_t size)
{
    void *p = jce_malloc(size);
    if (p) ++args_created;
    return p;
}
static void pending_free(void *p)
{
    if (!p) return;
    for (int i = 0; i < runtime.pending_audio_count; ++i) {
        RtPendingAudio *slot = &runtime.pending_audio[i];
        if (slot->args == p && slot->task && !jce_async_task_is_terminal(slot->task)) {
            ++premature_free;
            if (quarantine_count < 128) quarantine[quarantine_count++] = p;
            return; /* An early-free mutant must fail an assertion, not use freed bytes. */
        }
    }
    ++args_freed;
    jce_free(p);
}

#define jce_async_default_executor selected_executor
#define jce_scene_get_audio_source source_lookup
#define jce_scene_component_enabled source_enabled
#define jce_audio_decode_cpu decode_cpu
#define jce_audio_cpu_free free_cpu
#define jce_audio_upload_cpu upload_cpu
#define jce_audio_load_streaming load_streaming
#define jce_audio_stop stop_voice
#define jce_audio_unload unload_sound
#define jce_audio_is_playing voice_playing
#define jce_malloc pending_alloc
#define jce_free pending_free
#include JCE_TEST_AUDIO_PENDING_OWNER
static void close_audio_owner(JceRuntime *rt)
{
#include JCE_TEST_AUDIO_PENDING_CLOSE
}
#undef jce_async_default_executor
#undef jce_scene_get_audio_source
#undef jce_scene_component_enabled
#undef jce_audio_decode_cpu
#undef jce_audio_cpu_free
#undef jce_audio_upload_cpu
#undef jce_audio_load_streaming
#undef jce_audio_stop
#undef jce_audio_unload
#undef jce_audio_is_playing
#undef jce_malloc
#undef jce_free

static JceEntity source(int index, const char *path)
{
    entities[index] = ((uint64_t)7u << 32) | (uint64_t)(index + 1);
    enabled[index] = true;
    snprintf(sources[index].clip_path, sizeof sources[index].clip_path, "%s", path);
    return entities[index];
}
static void wait_started(void)
{
    TEST_ASSERT_TRUE_MESSAGE(jce_semaphore_wait_timeout(started, 5000), "real decoder did not enter barrier");
}
static void release_all(void)
{
    for (int i = 0; i < 128; ++i) jce_semaphore_signal(release_decode);
}
static void complete_all(void)
{
    release_all();
    for (int round = 0; runtime.pending_audio_count && round < 128; ++round) {
        for (int i = 0; i < runtime.pending_audio_count; ++i)
            if (runtime.pending_audio[i].task)
                TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
                    jce_async_task_wait_timeout(runtime.pending_audio[i].task, 5000));
        rt_audio_poll(&runtime);
    }
    TEST_ASSERT_EQUAL_INT(0, runtime.pending_audio_count);
    (void)jce_async_executor_pump(executor, NULL);
}
void setUp(void)
{
    memset(&runtime, 0, sizeof runtime); memset(sources, 0, sizeof sources);
    memset(entities, 0, sizeof entities); memset(enabled, 0, sizeof enabled);
    cpu_created = cpu_freed = uploads = stops = unloads = 0;
    args_created = args_freed = premature_free = quarantine_count = 0;
    uploaded_path[0] = '\0';
    ledger_mutex = jce_mutex_create(); started = jce_semaphore_create(0);
    release_decode = jce_semaphore_create(0);
    TEST_ASSERT_NOT_NULL(ledger_mutex); TEST_ASSERT_NOT_NULL(started);
    TEST_ASSERT_NOT_NULL(release_decode);
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_THREADED; config.worker_count = 1;
    config.max_tasks = 32; config.debug_name = "test.audio.pending";
    executor = jce_async_executor_create(&config);
    TEST_ASSERT_NOT_NULL(executor);
    runtime.scene = (JceScene *)&scene_cookie; runtime.audio = (JceAudio *)&audio_cookie;
    runtime.pak = (JcePakArchive *)&pak_cookie;
}
void tearDown(void)
{
    release_all();
    close_audio_owner(&runtime);
    if (executor) {
        (void)jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_CANCEL_ALL, 5000);
        jce_async_executor_destroy(executor); executor = NULL;
    }
    for (int i = 0; i < quarantine_count; ++i) { jce_free(quarantine[i]); ++args_freed; }
    jce_free(runtime.pending_audio); jce_free(runtime.voices);
    jce_semaphore_destroy(started); jce_semaphore_destroy(release_decode);
    jce_mutex_destroy(ledger_mutex);
    TEST_ASSERT_EQUAL_INT(0, premature_free);
    TEST_ASSERT_EQUAL_INT(args_created, args_freed);
    TEST_ASSERT_EQUAL_INT(cpu_created, cpu_freed);
}
static void test_stop_running_decode_prevents_late_voice_and_retains_borrow(void)
{
    JceEntity e = source(0, "before.wav");
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, e)); wait_started();
    RtAudioDecodeArgs *borrow = runtime.pending_audio[0].args;
    bool stopped = jce_runtime_audio_stop(&runtime, e);
    TEST_ASSERT_EQUAL_PTR(borrow, runtime.pending_audio[0].args);
    TEST_ASSERT_EQUAL_INT(0, args_freed);
    TEST_ASSERT_FALSE(jce_runtime_audio_stop(&runtime, e));
    JceAsyncTask *task = jce_async_task_retain(runtime.pending_audio[0].task);
    complete_all();
    const JceAsyncState state = jce_async_task_state(task);
    jce_async_task_release(task);
    TEST_ASSERT_EQUAL_INT(0, uploads); TEST_ASSERT_EQUAL_INT(0, runtime.voice_count);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED, state);
    TEST_ASSERT_TRUE(stopped);
}
static void test_stop_terminal_before_poll_rejects_published_cpu(void)
{
    JceEntity e = source(0, "terminal.wav");
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, e)); wait_started(); release_all();
    jce_async_task_wait(runtime.pending_audio[0].task);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_SUCCEEDED, jce_async_task_state(runtime.pending_audio[0].task));
    (void)jce_runtime_audio_stop(&runtime, e); complete_all();
    TEST_ASSERT_EQUAL_INT(0, uploads); TEST_ASSERT_EQUAL_INT(0, runtime.voice_count);
}
static void test_stop_taskless_slot_drops_only_its_unborrowed_intent(void)
{
    for (int i = 0; i < 5; ++i) TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, source(i, "queued.wav")));
    wait_started();
    TEST_ASSERT_NULL(runtime.pending_audio[4].task);
    TEST_ASSERT_TRUE(jce_runtime_audio_stop(&runtime, entities[4]));
    TEST_ASSERT_EQUAL_INT(4, runtime.pending_audio_count);
    complete_all(); TEST_ASSERT_EQUAL_INT(4, uploads);
}
static void test_restarts_before_completion_only_play_latest_request(void)
{
    JceEntity e = source(0, "old.wav");
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, e)); wait_started();
    TEST_ASSERT_TRUE(jce_runtime_audio_stop(&runtime, e));
    snprintf(sources[0].clip_path, sizeof sources[0].clip_path, "%s", "latest.wav");
    for (int i = 0; i < 24; ++i) TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, e));
    TEST_ASSERT_TRUE(runtime.pending_audio_count <= RT_AUDIO_DECODE_MAX + 1);
    complete_all(); TEST_ASSERT_EQUAL_INT(1, uploads);
    TEST_ASSERT_EQUAL_STRING("latest.wav", uploaded_path);
    TEST_ASSERT_EQUAL_INT(1, runtime.voice_count);
}
static void test_changed_clip_does_not_play_previous_decoded_bytes(void)
{
    JceEntity e = source(0, "old.wav");
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, e)); wait_started();
    snprintf(sources[0].clip_path, sizeof sources[0].clip_path, "%s", "replacement.wav");
    complete_all(); TEST_ASSERT_EQUAL_INT(0, uploads);
}
static void test_entity_generation_reuse_and_disabled_source_reject_completion(void)
{
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, source(0, "gone.wav"))); wait_started();
    entities[0] += (uint64_t)1u << 32;
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, source(1, "disabled.wav")));
    enabled[1] = false;
    complete_all(); TEST_ASSERT_EQUAL_INT(0, uploads);
}
static void test_stop_one_source_preserves_other_and_active_voice_retirement(void)
{
    JceEntity a = source(0, "a.wav"), b = source(1, "b.wav");
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, a)); wait_started();
    TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, b));
    TEST_ASSERT_TRUE(jce_runtime_audio_stop(&runtime, a)); complete_all();
    TEST_ASSERT_EQUAL_INT(1, uploads); TEST_ASSERT_EQUAL_STRING("b.wav", uploaded_path);
    TEST_ASSERT_TRUE(jce_runtime_audio_is_playing(&runtime, b));
    TEST_ASSERT_TRUE(jce_runtime_audio_stop(&runtime, b));
    TEST_ASSERT_EQUAL_INT(1, stops); TEST_ASSERT_EQUAL_INT(1, unloads);
    TEST_ASSERT_FALSE(jce_runtime_audio_stop(&runtime, b));
    TEST_ASSERT_FALSE(jce_runtime_audio_stop(&runtime, 0));
}
static void test_close_discards_running_and_unsubmitted_work_once(void)
{
    for (int i = 0; i < 5; ++i) TEST_ASSERT_TRUE(jce_runtime_audio_play(&runtime, source(i, "close.wav")));
    wait_started();
    (void)jce_runtime_audio_stop(&runtime, entities[0]);
    release_all(); close_audio_owner(&runtime);
    TEST_ASSERT_EQUAL_INT(0, runtime.pending_audio_count);
    TEST_ASSERT_EQUAL_INT(0, uploads); TEST_ASSERT_EQUAL_INT(args_created, args_freed);
    close_audio_owner(&runtime); TEST_ASSERT_EQUAL_INT(args_created, args_freed);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stop_running_decode_prevents_late_voice_and_retains_borrow);
    RUN_TEST(test_stop_terminal_before_poll_rejects_published_cpu);
    RUN_TEST(test_stop_taskless_slot_drops_only_its_unborrowed_intent);
    RUN_TEST(test_restarts_before_completion_only_play_latest_request);
    RUN_TEST(test_changed_clip_does_not_play_previous_decoded_bytes);
    RUN_TEST(test_entity_generation_reuse_and_disabled_source_reject_completion);
    RUN_TEST(test_stop_one_source_preserves_other_and_active_voice_retirement);
    RUN_TEST(test_close_discards_running_and_unsubmitted_work_once);
    return UNITY_END();
}
