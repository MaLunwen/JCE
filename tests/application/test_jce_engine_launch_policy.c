/* Complete actual launch parser and FPS owner, real Core trace ring. No host. */
#include "unity.h"
#include "application/jce_engine_launch_policy.h"
#include "application/jce_engine_frame_cap.h"
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>
#ifndef JCE_TEST_LAUNCH_FPS_OWNER
#define JCE_TEST_LAUNCH_FPS_OWNER "application/jce_engine_frame_cap.inc.h"
#endif
#include JCE_TEST_LAUNCH_FPS_OWNER

static int owner_a, owner_b;
static JceTraceEvent events[256];
void setUp(void)
{
    memset(&s_frame_cap, 0, sizeof s_frame_cap);
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
}
void tearDown(void)
{
    jce_engine_launch_fps_end(&owner_a); jce_engine_launch_fps_end(&owner_b);
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}

static void test_strict_parse_missing_zero_boundaries_and_atomic_failure(void)
{
    JceEngineLaunchPolicy policy;
    TEST_ASSERT_NULL(engine_launch_parse(NULL, NULL, &policy));
    TEST_ASSERT_FALSE(policy.vsync_present); TEST_ASSERT_FALSE(policy.fps_present);
    TEST_ASSERT_NULL(engine_launch_parse("0", "0", &policy));
    TEST_ASSERT_TRUE(policy.vsync_present); TEST_ASSERT_FALSE(policy.vsync);
    TEST_ASSERT_TRUE(policy.fps_present); TEST_ASSERT_EQUAL_INT(0, policy.fps);
    TEST_ASSERT_NULL(engine_launch_parse("1", "600", &policy));
    TEST_ASSERT_TRUE(policy.vsync); TEST_ASSERT_EQUAL_INT(600, policy.fps);
    TEST_ASSERT_NULL(engine_launch_parse("1", "2147483647", &policy));
    TEST_ASSERT_EQUAL_INT(INT_MAX, policy.fps);
    const char *bad[] = {"", " ", " 0", "0 ", "+1", "-1", "1.0", "0x10", "false", "off", "60fps", "2147483648", "999999999999999999999"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        unsigned char saved[sizeof policy]; memcpy(saved, &policy, sizeof policy);
        TEST_ASSERT_NOT_NULL(engine_launch_parse(NULL, bad[i], &policy));
        TEST_ASSERT_EQUAL_MEMORY(saved, &policy, sizeof policy);
        TEST_ASSERT_NOT_NULL(engine_launch_parse(bad[i], NULL, &policy));
        TEST_ASSERT_EQUAL_MEMORY(saved, &policy, sizeof policy);
    }
    TEST_ASSERT_NOT_NULL(engine_launch_parse("2", NULL, &policy));
    TEST_ASSERT_NOT_NULL(engine_launch_parse(NULL, NULL, NULL));
}

static void test_finite_duration_retains_scalar_bounds_and_complete_parse(void)
{
    const char *invalid[] = {NULL, "", "0", "-1", "nan", "inf", "86400.1",
                            "0.5s", "0.5 ", "missing"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        TEST_ASSERT_EQUAL_DOUBLE(0.0, engine_launch_positive_seconds(invalid[i]));
    TEST_ASSERT_EQUAL_DOUBLE(0.5, engine_launch_positive_seconds("0.5"));
    TEST_ASSERT_EQUAL_DOUBLE(86400.0, engine_launch_positive_seconds("86400"));
    /* strtod's accepted scalar spellings predate this ownership move. */
    TEST_ASSERT_EQUAL_DOUBLE(0.5, engine_launch_positive_seconds(" +0.5"));
    TEST_ASSERT_EQUAL_DOUBLE(100.0, engine_launch_positive_seconds("1e2"));
}

static void test_zero_override_survives_later_project_pause_and_background_requests(void)
{
    jce_engine_set_target_fps(144); s_frame_cap.deadline = 99;
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&owner_a, true, 0));
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps()); TEST_ASSERT_EQUAL_UINT64(0, s_frame_cap.deadline);
    const int requests[] = {60, 30, 15, 5, 0, 90};
    for (unsigned i = 0; i < sizeof requests / sizeof requests[0]; ++i) {
        jce_engine_set_target_fps(requests[i]);
        TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
        TEST_ASSERT_EQUAL_INT(requests[i], s_frame_cap.requested_fps);
    }
    TEST_ASSERT_FALSE(jce_engine_launch_fps_begin(&owner_b, true, 600));
    jce_engine_launch_fps_end(&owner_b);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    jce_engine_launch_fps_end(&owner_a);
    TEST_ASSERT_EQUAL_INT(90, jce_engine_get_target_fps());
    TEST_ASSERT_NULL(s_frame_cap.launch_owner);
    TEST_ASSERT_EQUAL_UINT64(0, s_frame_cap.deadline);
    // Next ordinary session/public call has no sticky launch value.
    jce_engine_set_target_fps(60); TEST_ASSERT_EQUAL_INT(60, jce_engine_get_target_fps());
}

static void test_positive_override_preserves_latest_request_and_background_permission(void)
{
    TEST_ASSERT_FALSE(jce_engine_launch_fps_begin(NULL, true, 600));
    TEST_ASSERT_FALSE(jce_engine_launch_fps_begin(&owner_a, true, -1));
    jce_engine_set_run_in_background(true);
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&owner_a, true, 600));
    jce_engine_set_target_fps(-10);
    TEST_ASSERT_EQUAL_INT(600, jce_engine_get_target_fps());
    TEST_ASSERT_EQUAL_INT(0, s_frame_cap.requested_fps);
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    jce_engine_launch_fps_end(&owner_a);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    TEST_ASSERT_TRUE(jce_engine_get_run_in_background());
    // A Vsync-only launch observes FPS but does not force it.
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&owner_b, false, 0));
    jce_engine_set_target_fps(165); TEST_ASSERT_EQUAL_INT(165, jce_engine_get_target_fps());
    jce_engine_launch_fps_end(&owner_b); TEST_ASSERT_EQUAL_INT(165, jce_engine_get_target_fps());
}

static void test_default_no_policy_keeps_zero_negative_and_same_rate_deadline(void)
{
    jce_trace_set_enabled(true);
    jce_engine_set_target_fps(-1); TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    jce_engine_set_target_fps(60); s_frame_cap.deadline = 123;
    jce_engine_set_target_fps(60); TEST_ASSERT_EQUAL_UINT64(123, s_frame_cap.deadline);
    jce_engine_set_target_fps(0); TEST_ASSERT_EQUAL_UINT64(0, s_frame_cap.deadline);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
}

static void test_trace_uses_real_effective_getter_closed_parent_and_transition_only(void)
{
    jce_trace_set_enabled(true); jce_trace_task_restore_id(901);
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&owner_a, true, 0));
    jce_engine_set_target_fps(60);
    JceTraceStats before, after; jce_trace_get_stats(&before);
    jce_engine_set_target_fps(60); jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    jce_engine_launch_fps_observe(&owner_a, 3);
    jce_engine_launch_fps_end(&owner_a);
    TEST_ASSERT_EQUAL_UINT64(901, jce_trace_task_current_id());
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    unsigned count = jce_trace_read(&cursor, events, 256), begins = 0, ends = 0, effective = 0;
    TEST_ASSERT_LESS_THAN_UINT(256, count); TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    uint64_t active = 0;
    for (unsigned i = 0; i < count; ++i) {
        JceTraceEvent *event = &events[i];
        if (event->type == JCE_TRACE_EVENT_TASK_BEGIN && strcmp(event->name, "engine.launch_fps") == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, active); active = event->id; ++begins;
        } else if (event->type == JCE_TRACE_EVENT_COUNTER && strcmp(event->name, "launch.fps.effective") == 0) {
            TEST_ASSERT_EQUAL_UINT64(active, event->id); TEST_ASSERT_EQUAL_UINT64(active, event->parent_id);
            TEST_ASSERT_EQUAL_INT(effective == 3 ? 60 : 0, (int)event->value_u64); ++effective;
        } else if (event->type == JCE_TRACE_EVENT_TASK_END && strcmp(event->name, "engine.launch_fps") == 0) {
            TEST_ASSERT_EQUAL_UINT64(active, event->id); active = 0; ++ends;
            TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SUCCEEDED, event->value_u32[0]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(4, begins); TEST_ASSERT_EQUAL_UINT(4, ends); TEST_ASSERT_EQUAL_UINT(4, effective);
    jce_trace_get_stats(&after); TEST_ASSERT_EQUAL_UINT(0, after.active_work_items);
}

static void test_trace_disabled_and_failed_session_ownership_do_not_publish(void)
{
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&owner_a, true, 0));
    jce_engine_set_target_fps(120);
    TEST_ASSERT_FALSE(jce_engine_launch_fps_begin(&owner_b, true, 600));
    jce_engine_launch_fps_end(&owner_b);
    jce_engine_launch_fps_end(&owner_a);
    TEST_ASSERT_EQUAL_INT(120, jce_engine_get_target_fps());
    JceTraceStats stats; jce_trace_get_stats(&stats); TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_strict_parse_missing_zero_boundaries_and_atomic_failure);
    RUN_TEST(test_finite_duration_retains_scalar_bounds_and_complete_parse);
    RUN_TEST(test_zero_override_survives_later_project_pause_and_background_requests);
    RUN_TEST(test_positive_override_preserves_latest_request_and_background_permission);
    RUN_TEST(test_default_no_policy_keeps_zero_negative_and_same_rate_deadline);
    RUN_TEST(test_trace_uses_real_effective_getter_closed_parent_and_transition_only);
    RUN_TEST(test_trace_disabled_and_failed_session_ownership_do_not_publish);
    return UNITY_END();
}
