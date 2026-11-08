/* Actual master service, with only native device/clock ports replaced. */
#include "unity.h"
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <miniaudio.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <stdint.h>

struct JceAudio { ma_engine engine; };
static struct {
    uint64_t clock, thread, release_at;
    unsigned init, start, uninit, unsafe_uninit, pulls, tap_calls;
    unsigned changed_old_pair, atomic_creates, atomic_live, fail_atomic_at, gate_oom_errors;
    bool fail_init, fail_start, watch_old_pair;
    JceAudioMasterTapFn old_fn;
    void *old_ud;
} probe;
static JceAtomicI32 *audio_port_atomic_create(int32_t value);
static void audio_port_atomic_destroy(JceAtomicI32 *atomic);
static void audio_port_log(JceLogLevel level, const char *tag, const char *file, int line, const char *format, ...);
static ma_device_config audio_port_config(ma_device_type kind);
static ma_result audio_port_init(ma_context *context, const ma_device_config *config, ma_device *device);
static ma_result audio_port_start(ma_device *device);
static void audio_port_uninit(ma_device *device);
static ma_result audio_port_read(ma_engine *engine, void *output, ma_uint64 count, ma_uint64 *read);
static void audio_port_sleep(uint32_t ms);
static uint64_t audio_port_clock(void);
static uint64_t audio_port_thread(void);
#define jce_atomic_i32_create audio_port_atomic_create
#define jce_atomic_i32_destroy audio_port_atomic_destroy
#define jce_log_write audio_port_log
#define ma_device_config_init audio_port_config
#define ma_device_init audio_port_init
#define ma_device_start audio_port_start
#define ma_device_uninit audio_port_uninit
#define ma_engine_read_pcm_frames audio_port_read
#define jce_thread_sleep_ms audio_port_sleep
#define jce_time_ticks_ms audio_port_clock
#define jce_thread_current_id audio_port_thread
#ifndef JCE_TEST_AUDIO_MASTER_SOURCE
#define JCE_TEST_AUDIO_MASTER_SOURCE "middleware/audio/jce_audio_master.inc.h"
#endif
#include JCE_TEST_AUDIO_MASTER_SOURCE
#undef jce_atomic_i32_create
#undef jce_atomic_i32_destroy
#undef jce_log_write
#undef ma_device_config_init
#undef ma_device_init
#undef ma_device_start
#undef ma_device_uninit
#undef ma_engine_read_pcm_frames
#undef jce_thread_sleep_ms
#undef jce_time_ticks_ms
#undef jce_thread_current_id

static JceAtomicI32 *audio_port_atomic_create(int32_t value)
{
    ++probe.atomic_creates;
    if (probe.fail_atomic_at && probe.atomic_creates == probe.fail_atomic_at) return NULL;
    JceAtomicI32 *result = jce_atomic_i32_create(value);
    if (result) ++probe.atomic_live;
    return result;
}
static void audio_port_atomic_destroy(JceAtomicI32 *atomic)
{
    if (atomic) --probe.atomic_live;
    jce_atomic_i32_destroy(atomic);
}
static void audio_port_log(JceLogLevel level, const char *tag, const char *file,
                           int line, const char *format, ...)
{
    if (level == JCE_LOG_LEVEL_ERROR && strstr(format, "master callback admission allocation failed"))
        ++probe.gate_oom_errors;
    va_list args;
    va_start(args, format);
    jce_log_write_v(level, tag, file, line, format, args);
    va_end(args);
}
static ma_device_config audio_port_config(ma_device_type kind)
{
    ma_device_config result;
    memset(&result, 0, sizeof(result));
    result.deviceType = kind;
    return result;
}
static ma_result audio_port_init(ma_context *context, const ma_device_config *config, ma_device *device)
{
    (void)context; (void)device;
    ++probe.init;
    TEST_ASSERT_EQUAL_UINT(48000, config->sampleRate);
    TEST_ASSERT_EQUAL_UINT(2, config->playback.channels);
    TEST_ASSERT_NOT_NULL(config->dataCallback);
    return probe.fail_init ? MA_ERROR : MA_SUCCESS;
}
static ma_result audio_port_start(ma_device *device)
{
    (void)device;
    ++probe.start;
    return probe.fail_start ? MA_ERROR : MA_SUCCESS;
}
static void audio_port_uninit(ma_device *device)
{
    (void)device;
    ++probe.uninit;
    if (s_master.gate.inflight && jce_atomic_i32_load(s_master.gate.inflight))
        ++probe.unsafe_uninit;
}
static ma_result audio_port_read(ma_engine *engine, void *output, ma_uint64 count, ma_uint64 *read)
{
    (void)engine;
    ++probe.pulls;
    float *pcm = output;
    for (ma_uint64 i = 0; i < count * 2; ++i) pcm[i] = 0.25f;
    *read = count;
    return MA_SUCCESS;
}
static uint64_t audio_port_clock(void) { return probe.clock; }
static uint64_t audio_port_thread(void) { return probe.thread; }
static void audio_port_sleep(uint32_t ms)
{
    probe.clock += ms;
    if (probe.watch_old_pair && (s_master.tap_fn != probe.old_fn || s_master.tap_ud != probe.old_ud))
        ++probe.changed_old_pair;
    if (probe.release_at && probe.clock >= probe.release_at)
        jce_atomic_i32_store(s_master.gate.inflight, 0);
}
static void hold_foreign_callback(void)
{
    jce_atomic_i32_store(s_master.gate.callback_thread_lo, 2);
    jce_atomic_i32_store(s_master.gate.callback_thread_hi, 0);
    jce_atomic_i32_store(s_master.gate.inflight, 1);
}
static void count_tap(void *ud, const float *pcm, uint32_t frames, uint32_t rate, uint32_t channels)
{
    (void)ud; (void)pcm; (void)frames; (void)rate; (void)channels;
    ++probe.tap_calls;
}
static void other_tap(void *ud, const float *pcm, uint32_t frames, uint32_t rate, uint32_t channels)
{
    count_tap(ud, pcm, frames, rate, channels);
}
void setUp(void)
{
    memset(&probe, 0, sizeof(probe));
    memset(&s_master, 0, sizeof(s_master));
    probe.thread = 1;
}
void tearDown(void)
{
    /* A failing ordinary control never leaves a callback or native resource. */
    if (s_master.gate.inflight) jce_atomic_i32_store(s_master.gate.inflight, 0);
    s_master.session_closed = true;
    (void)jce_audio_master_try_retire_internal();
    (void)jce_audio_master_gate_release(&s_master.gate);
}
static void test_device_live_observation_tracks_actual_master_without_control(void)
{
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(0, probe.atomic_creates);
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_TRUE(jce_audio_master_device_live_internal());
    TEST_ASSERT_TRUE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.init);
    TEST_ASSERT_EQUAL_UINT(1, probe.start);
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    hold_foreign_callback();
    jce_audio_master_session_close_internal();
    TEST_ASSERT_TRUE(jce_audio_master_device_live_internal());
    TEST_ASSERT_FALSE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_TRUE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    jce_atomic_i32_store(s_master.gate.inflight, 0);
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
}
static void test_last_graph_keeps_device_and_silence_tap(void)
{
    JceAudio graph;
    memset(&graph, 0, sizeof(graph));
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_TRUE(master_register(&graph));
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(count_tap, NULL));
    float pcm[16];
    master_device_cb(NULL, pcm, NULL, 8);
    TEST_ASSERT_EQUAL_UINT(1, probe.pulls);
    TEST_ASSERT_TRUE(master_unregister(&graph));
    master_device_cb(NULL, pcm, NULL, 8);
    TEST_ASSERT_TRUE(s_master.device_inited);
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    TEST_ASSERT_EQUAL_UINT(2, probe.tap_calls);
    TEST_ASSERT_EQUAL_UINT(1, probe.pulls);
    for (unsigned i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_FLOAT(0, pcm[i]);
}
static void test_legacy_graph_fence_waits_past_old_timeout(void)
{
    JceAudio graph;
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_TRUE(master_register(&graph));
    hold_foreign_callback();
    probe.release_at = 2501;
    TEST_ASSERT_TRUE(master_unregister(&graph));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(2501, probe.clock);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(s_master.gate.inflight));
    TEST_ASSERT_NULL(s_master.slots[0].ptr);
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
}
static void test_tap_replacement_waits_before_changing_either_field(void)
{
    int old_ud = 1, new_ud = 2;
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(count_tap, &old_ud));
    hold_foreign_callback();
    probe.old_fn = count_tap; probe.old_ud = &old_ud; probe.watch_old_pair = true;
    probe.release_at = 3;
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(other_tap, &new_ud));
    TEST_ASSERT_EQUAL_UINT(0, probe.changed_old_pair);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(s_master.gate.inflight));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(3, probe.clock);
    TEST_ASSERT_EQUAL_PTR(other_tap, s_master.tap_fn);
    TEST_ASSERT_EQUAL_PTR(&new_ud, s_master.tap_ud);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&s_master.tap_live));
}
static void test_final_refusal_retains_targets_then_retry_retires_once(void)
{
    JceAudio graph;
    int ud = 7;
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_TRUE(master_register(&graph));
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(count_tap, &ud));
    hold_foreign_callback();
    jce_audio_master_session_close_internal();
    TEST_ASSERT_FALSE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    TEST_ASSERT_TRUE(s_master.device_inited);
    TEST_ASSERT_EQUAL_PTR(&graph, s_master.slots[0].ptr);
    TEST_ASSERT_EQUAL_PTR(count_tap, s_master.tap_fn);
    TEST_ASSERT_EQUAL_PTR(&ud, s_master.tap_ud);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(s_master.gate.closed));
    jce_atomic_i32_store(s_master.gate.inflight, 0);
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
    TEST_ASSERT_EQUAL_UINT(0, probe.unsafe_uninit);
    TEST_ASSERT_FALSE(s_master.device_inited);
    TEST_ASSERT_NULL(s_master.gate.inflight);
    TEST_ASSERT_EQUAL_PTR(&graph, s_master.slots[0].ptr);
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&s_master.slots[0].live));
    TEST_ASSERT_TRUE(master_unregister(&graph));
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(NULL, NULL));
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
    TEST_ASSERT_FALSE(master_device_ensure());
    jce_audio_master_session_open_internal();
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_EQUAL_UINT(2, probe.init);
}
static void test_self_wait_refuses_before_mutation_including_zero_thread_id(void)
{
    JceAudio graph;
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_TRUE(master_register(&graph));
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(count_tap, &graph));
    probe.thread = 0; /* Actual non-pthread Web IDs are allowed to be zero. */
    TEST_ASSERT_TRUE(jce_audio_master_gate_enter(&s_master.gate));
    jce_audio_master_session_close_internal();
    TEST_ASSERT_FALSE(master_unregister(&graph));
    TEST_ASSERT_FALSE(jce_audio_master_tap_set(NULL, NULL));
    TEST_ASSERT_FALSE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&s_master.slots[0].live));
    TEST_ASSERT_EQUAL_PTR(count_tap, s_master.tap_fn);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(s_master.gate.closed));
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    jce_audio_master_gate_leave(&s_master.gate);
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
}
static void test_closing_denies_reopen_and_closed_callback_only_outputs_silence(void)
{
    JceAudio graph;
    TEST_ASSERT_TRUE(master_device_ensure());
    jce_audio_master_session_close_internal();
    TEST_ASSERT_FALSE(master_device_ensure());
    TEST_ASSERT_FALSE(master_register(&graph));
    TEST_ASSERT_FALSE(jce_audio_master_tap_set(count_tap, NULL));
    TEST_ASSERT_TRUE(jce_audio_master_gate_quiesce(&s_master.gate, 0));
    float pcm[16]; for (unsigned i = 0; i < 16; ++i) pcm[i] = 1;
    master_device_cb(NULL, pcm, NULL, 8);
    TEST_ASSERT_EQUAL_UINT(0, probe.pulls);
    TEST_ASSERT_EQUAL_UINT(0, probe.tap_calls);
    for (unsigned i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_FLOAT(0, pcm[i]);
}
static void test_device_init_failure_releases_gate_and_next_session_retries(void)
{
    probe.fail_init = true;
    TEST_ASSERT_FALSE(master_device_ensure());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_NOT_NULL(s_master.gate.inflight);
    jce_audio_master_session_close_internal();
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_NULL(s_master.gate.inflight);
    TEST_ASSERT_EQUAL_UINT(0, probe.uninit);
    probe.fail_init = false;
    jce_audio_master_session_open_internal();
    TEST_ASSERT_TRUE(master_device_ensure());
}
static void test_start_rollback_does_not_double_uninit_at_final_retirement(void)
{
    probe.fail_start = true;
    TEST_ASSERT_FALSE(master_device_ensure());
    TEST_ASSERT_FALSE(jce_audio_master_device_live_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
    jce_audio_master_session_close_internal();
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    TEST_ASSERT_EQUAL_UINT(1, probe.uninit);
    TEST_ASSERT_NULL(s_master.gate.inflight);
    probe.fail_start = false;
    jce_audio_master_session_open_internal();
    TEST_ASSERT_TRUE(master_device_ensure());
}
static void test_gate_allocation_failure_is_reported_once_and_next_session_recovers(void)
{
    probe.fail_atomic_at = 2;
    TEST_ASSERT_FALSE(master_device_ensure());
    TEST_ASSERT_TRUE(s_master.device_failed);
    TEST_ASSERT_EQUAL_UINT(1, probe.gate_oom_errors);
    TEST_ASSERT_EQUAL_UINT(0, probe.init);
    TEST_ASSERT_EQUAL_UINT(0, probe.atomic_live);
    TEST_ASSERT_NULL(s_master.gate.closed);
    TEST_ASSERT_NULL(s_master.gate.inflight);
    TEST_ASSERT_NULL(s_master.gate.callback_thread_lo);
    TEST_ASSERT_NULL(s_master.gate.callback_thread_hi);
    const unsigned attempts = probe.atomic_creates;
    TEST_ASSERT_FALSE(master_device_ensure());
    TEST_ASSERT_EQUAL_UINT(attempts, probe.atomic_creates);
    TEST_ASSERT_EQUAL_UINT(1, probe.gate_oom_errors);
    jce_audio_master_session_close_internal();
    TEST_ASSERT_TRUE(jce_audio_master_try_retire_internal());
    probe.fail_atomic_at = 0;
    jce_audio_master_session_open_internal();
    TEST_ASSERT_TRUE(master_device_ensure());
    TEST_ASSERT_EQUAL_UINT(1, probe.init);
    TEST_ASSERT_EQUAL_UINT(1, probe.gate_oom_errors);
}
static void test_callback_identity_retains_nonzero_high_half(void)
{
    TEST_ASSERT_TRUE(master_device_ensure());
    probe.thread = UINT64_C(0x100000002);
    TEST_ASSERT_TRUE(jce_audio_master_gate_enter(&s_master.gate));
    TEST_ASSERT_TRUE(jce_audio_master_gate_self(&s_master.gate));
    probe.thread = UINT64_C(2);
    TEST_ASSERT_FALSE(jce_audio_master_gate_self(&s_master.gate));
    probe.thread = UINT64_C(0x100000002);
    jce_audio_master_gate_leave(&s_master.gate);
    TEST_ASSERT_FALSE(jce_audio_master_gate_self(&s_master.gate));
}
int main(void)
{
#ifndef JCE_TEST_AUDIO_UNMARKED_OWNER
    jce_thread_mark_main();
#endif
    UNITY_BEGIN();
    RUN_TEST(test_device_live_observation_tracks_actual_master_without_control);
    RUN_TEST(test_callback_identity_retains_nonzero_high_half);
    RUN_TEST(test_gate_allocation_failure_is_reported_once_and_next_session_recovers);
    RUN_TEST(test_last_graph_keeps_device_and_silence_tap);
    RUN_TEST(test_legacy_graph_fence_waits_past_old_timeout);
    RUN_TEST(test_tap_replacement_waits_before_changing_either_field);
    RUN_TEST(test_final_refusal_retains_targets_then_retry_retires_once);
    RUN_TEST(test_self_wait_refuses_before_mutation_including_zero_thread_id);
    RUN_TEST(test_closing_denies_reopen_and_closed_callback_only_outputs_silence);
    RUN_TEST(test_device_init_failure_releases_gate_and_next_session_retries);
    RUN_TEST(test_start_rollback_does_not_double_uninit_at_final_retirement);
    return UNITY_END();
}
