/* Complete actual Engine TU; the audio service port uses its real gate owner. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_thread.h>
#include "middleware/audio/jce_audio_master_gate.h"
#include <SDL3/SDL.h>
#include <string.h>
#include <stdint.h>

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;
bool audio_engine_port_self(void);
bool audio_engine_port_device_live(void);
void audio_engine_port_open(void);
void audio_engine_port_close(void);
bool audio_engine_port_retire(void);
#define jce_audio_master_is_callback_internal audio_engine_port_self
#define jce_audio_master_device_live_internal audio_engine_port_device_live
#define jce_audio_master_session_open_internal audio_engine_port_open
#define jce_audio_master_session_close_internal audio_engine_port_close
#define jce_audio_master_try_retire_internal audio_engine_port_retire
#ifndef JCE_TEST_AUDIO_RETIREMENT_ENGINE
#define JCE_TEST_AUDIO_RETIREMENT_ENGINE "application/jce_engine.c"
#endif
#include JCE_TEST_AUDIO_RETIREMENT_ENGINE
#undef jce_audio_master_is_callback_internal
#undef jce_audio_master_device_live_internal
#undef jce_audio_master_session_open_internal
#undef jce_audio_master_session_close_internal
#undef jce_audio_master_try_retire_internal

static struct {
    JceAudioMasterGate gate;
    bool closed, gate_ok, fail_init, close_in_init, closed_in_init;
    bool retired_before_exit, reentry_refused, device_live;
    unsigned device_reads;
    unsigned opens, closes, retire_attempts, retire_completed, exits;
} probe;
static JceEngine *audio_engine_owner;
bool audio_engine_port_self(void)
{
    return jce_audio_master_gate_self(&probe.gate);
}
bool audio_engine_port_device_live(void)
{
    ++probe.device_reads;
    return probe.device_live;
}
void audio_engine_port_open(void)
{
    ++probe.opens;
    probe.closed = false;
    probe.device_live = true; /* Typed native-device lifetime port. */
    probe.gate_ok = jce_audio_master_gate_create(&probe.gate);
}
void audio_engine_port_close(void)
{
    ++probe.closes;
    probe.closed = true;
}
bool audio_engine_port_retire(void)
{
    ++probe.retire_attempts;
    if (!probe.closed || !probe.gate_ok ||
        !jce_audio_master_gate_quiesce(&probe.gate, 0)) return false;
    probe.device_live = false;
    ++probe.retire_completed;
    return jce_audio_master_gate_release(&probe.gate);
}
static void hold_audio_borrow(void)
{
    const uint64_t other = jce_thread_current_id() ^ UINT64_C(1);
    jce_atomic_i32_store(probe.gate.callback_thread_lo, (int32_t)(uint32_t)other);
    jce_atomic_i32_store(probe.gate.callback_thread_hi, (int32_t)(uint32_t)(other >> 32));
    jce_atomic_i32_store(probe.gate.inflight, 1);
}
static bool audio_app_init(const JceServices *services, void *user)
{
    (void)services; (void)user;
    if (!probe.gate_ok) return false;
    hold_audio_borrow();
    if (probe.close_in_init) {
        (void)jce_engine_try_destroy(&audio_engine_owner);
        probe.closed_in_init = probe.closed;
    }
    return !probe.fail_init;
}
static void audio_app_exit(void *user)
{
    (void)user;
    ++probe.exits;
    probe.retired_before_exit = probe.closed && probe.retire_completed == 1 && !probe.gate.inflight;
    probe.reentry_refused = !jce_engine_try_destroy(&audio_engine_owner);
}
static JceAppResult create_audio_host(void)
{
    JceAppDesc desc = {0};
    desc.name = "audio-retirement-owner";
    desc.headless = true;
    desc.init = audio_app_init;
    desc.exit = audio_app_exit;
    jce_engine_set_app_desc(&desc);
    char name[] = "test-engine-audio-retirement";
    char *argv[] = {name, NULL};
    return jce_engine_create_owned(1, argv, &audio_engine_owner);
}
void setUp(void)
{
    memset(&probe, 0, sizeof(probe));
    audio_engine_owner = NULL;
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
}
void tearDown(void)
{
    if (probe.gate.inflight) jce_atomic_i32_store(probe.gate.inflight, 0);
    probe.closed = true;
    if (audio_engine_owner) (void)jce_engine_try_destroy(&audio_engine_owner);
    (void)jce_audio_master_gate_release(&probe.gate);
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}
/* Actual Engine adapter and real trace ring. Device state is a typed port;
 * the separate complete-master fixture checks it against native uninit calls. */
static void expect_audio_observation(bool had_device, bool live_after, bool retired)
{
    JceTraceEvent events[64];
    const char *names[3] = {"audio_master.had_device", "audio_master.device_live_after", "audio_master.retired"};
    const bool values[3] = {had_device, live_after, retired};
    unsigned fields[3] = {0}, submits = 0, begins = 0, ends = 0;
    const unsigned reads = probe.device_reads;
    jce_trace_set_enabled(true); jce_trace_task_restore_id(0); jce_trace_reset();
    JceEngineTraceScope parent = engine_trace_begin("test.audio_retirement_parent");
    TEST_ASSERT_EQUAL_INT(retired, engine_audio_master_retire());
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    engine_trace_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(live_after, probe.device_live);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    const unsigned count = jce_trace_read(&cursor, events, 64);
    TEST_ASSERT_LESS_THAN_UINT(64, count);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    uint64_t active = 0;
    for (unsigned i = 0; i < count; ++i) {
        const JceTraceEvent *event = &events[i];
        if (strcmp(event->name, "engine.audio_master_retire") == 0) {
            if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                ++submits; TEST_ASSERT_EQUAL_UINT64(parent.id, event->parent_id);
            } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
                ++begins; TEST_ASSERT_EQUAL_UINT64(0, active); active = event->id;
                TEST_ASSERT_NOT_EQUAL(0, active);
            } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
                ++ends; TEST_ASSERT_EQUAL_UINT64(active, event->id); active = 0;
                TEST_ASSERT_EQUAL_UINT(retired ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED,
                                       event->value_u32[0]);
            }
        } else if (event->type == JCE_TRACE_EVENT_COUNTER) {
            for (unsigned field = 0; field < 3; ++field) {
                if (strcmp(event->name, names[field]) != 0) continue;
                ++fields[field]; TEST_ASSERT_NOT_EQUAL(0, active);
                TEST_ASSERT_EQUAL_UINT(JCE_TRACE_CATEGORY_CORE, event->category);
                TEST_ASSERT_EQUAL_UINT64(active, event->id);
                TEST_ASSERT_EQUAL_UINT64(active, event->parent_id);
                TEST_ASSERT_EQUAL_UINT64(values[field], event->value_u64);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, submits); TEST_ASSERT_EQUAL_UINT(1, begins);
    TEST_ASSERT_EQUAL_UINT(1, ends); TEST_ASSERT_EQUAL_UINT64(0, active);
    for (unsigned field = 0; field < 3; ++field) TEST_ASSERT_EQUAL_UINT(1, fields[field]);
    TEST_ASSERT_EQUAL_UINT(reads + 2, probe.device_reads);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_work_items);
}
static void test_scoped_device_observation_retains_timeout_then_native_transition(void)
{
    probe.closed = true; probe.device_live = true;
    probe.gate_ok = jce_audio_master_gate_create(&probe.gate);
    TEST_ASSERT_TRUE(probe.gate_ok);
    hold_audio_borrow();
    expect_audio_observation(true, true, false);
    TEST_ASSERT_EQUAL_UINT(0, probe.retire_completed);
    jce_atomic_i32_store(probe.gate.inflight, 0);
    expect_audio_observation(true, false, true);
    TEST_ASSERT_EQUAL_UINT(1, probe.retire_completed);
    expect_audio_observation(false, false, true); /* Successful no-device no-op. */
}
static void test_scoped_no_device_success_is_explicitly_zero(void)
{
    probe.closed = true;
    probe.gate_ok = jce_audio_master_gate_create(&probe.gate);
    TEST_ASSERT_TRUE(probe.gate_ok);
    expect_audio_observation(false, false, true);
}
static void test_disabled_observation_preserves_actual_retirement_without_getters(void)
{
    probe.closed = true; probe.device_live = true;
    probe.gate_ok = jce_audio_master_gate_create(&probe.gate);
    TEST_ASSERT_TRUE(probe.gate_ok);
    TEST_ASSERT_TRUE(engine_audio_master_retire());
    TEST_ASSERT_FALSE(probe.device_live);
    TEST_ASSERT_EQUAL_UINT(1, probe.retire_attempts);
    TEST_ASSERT_EQUAL_UINT(1, probe.retire_completed);
    TEST_ASSERT_EQUAL_UINT(0, probe.device_reads);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
}
static void assert_refusal_preserves_app_then_retry(void)
{
    TEST_ASSERT_NOT_NULL(audio_engine_owner);
    TEST_ASSERT_FALSE(jce_engine_try_destroy(&audio_engine_owner));
    TEST_ASSERT_NOT_NULL(audio_engine_owner); /* Never dereference a mutant's freed owner. */
    TEST_ASSERT_EQUAL_UINT(0, probe.exits);
    TEST_ASSERT_EQUAL_UINT(0, probe.retire_completed);
    TEST_ASSERT_TRUE(probe.closed);
    jce_atomic_i32_store(probe.gate.inflight, 0);
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&audio_engine_owner));
    TEST_ASSERT_NULL(audio_engine_owner);
    TEST_ASSERT_EQUAL_UINT(1, probe.exits);
    TEST_ASSERT_EQUAL_UINT(1, probe.retire_completed);
    TEST_ASSERT_TRUE(probe.retired_before_exit);
    TEST_ASSERT_TRUE(probe.reentry_refused);
}
static void test_audio_borrow_refuses_before_irreversible_engine_tail(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, create_audio_host());
    TEST_ASSERT_TRUE(probe.gate_ok);
    assert_refusal_preserves_app_then_retry();
}
static void test_failed_constructor_retains_audio_and_same_engine_owner(void)
{
    probe.fail_init = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, create_audio_host());
    TEST_ASSERT_NOT_NULL(audio_engine_owner);
    TEST_ASSERT_EQUAL_UINT(0, probe.exits);
    assert_refusal_preserves_app_then_retry();
}
static void test_constructor_borrow_closes_audio_session_before_early_refusal(void)
{
    probe.close_in_init = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, create_audio_host());
    TEST_ASSERT_TRUE(probe.closed_in_init);
    TEST_ASSERT_NOT_NULL(audio_engine_owner);
    assert_refusal_preserves_app_then_retry();
}
static void test_only_an_admitted_new_engine_reopens_audio(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, create_audio_host());
    assert_refusal_preserves_app_then_retry();
    TEST_ASSERT_TRUE(probe.closed);
    const unsigned opens = probe.opens;
    char name[] = "invalid-audio-owner"; char *argv[] = {name, NULL};
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(1, argv, NULL));
    TEST_ASSERT_EQUAL_UINT(opens, probe.opens);
    probe.exits = probe.retire_completed = 0;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, create_audio_host());
    TEST_ASSERT_EQUAL_UINT(opens + 1, probe.opens);
    TEST_ASSERT_FALSE(probe.closed);
    assert_refusal_preserves_app_then_retry();
}
static void test_callback_refuses_before_engine_creation_or_teardown_work(void)
{
    TEST_ASSERT_TRUE(jce_audio_master_gate_create(&probe.gate));
    TEST_ASSERT_TRUE(jce_audio_master_gate_enter(&probe.gate));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, create_audio_host());
    TEST_ASSERT_NULL(audio_engine_owner);
    TEST_ASSERT_EQUAL_UINT(0, probe.opens);
    jce_audio_master_gate_leave(&probe.gate);
    TEST_ASSERT_TRUE(jce_audio_master_gate_release(&probe.gate));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, create_audio_host());
    jce_atomic_i32_store(probe.gate.inflight, 0);
    TEST_ASSERT_TRUE(jce_audio_master_gate_enter(&probe.gate));
    TEST_ASSERT_FALSE(jce_engine_try_destroy(&audio_engine_owner));
    TEST_ASSERT_NOT_NULL(audio_engine_owner);
    TEST_ASSERT_EQUAL_UINT(0, probe.closes);
    TEST_ASSERT_EQUAL_UINT(0, probe.retire_attempts);
    jce_audio_master_gate_leave(&probe.gate);
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&audio_engine_owner));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scoped_device_observation_retains_timeout_then_native_transition);
    RUN_TEST(test_scoped_no_device_success_is_explicitly_zero);
    RUN_TEST(test_disabled_observation_preserves_actual_retirement_without_getters);
    RUN_TEST(test_callback_refuses_before_engine_creation_or_teardown_work);
    RUN_TEST(test_audio_borrow_refuses_before_irreversible_engine_tail);
    RUN_TEST(test_failed_constructor_retains_audio_and_same_engine_owner);
    RUN_TEST(test_constructor_borrow_closes_audio_session_before_early_refusal);
    RUN_TEST(test_only_an_admitted_new_engine_reopens_audio);
    return UNITY_END();
}
