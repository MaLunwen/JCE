/*
 * test_jce_engine_frame_cap.c — the frame-rate cap's policy.
 *
 * The engine had NO frame limiter.  A shipped game sitting on a menu, or any
 * scene the GPU finished early, ran the loop as fast as it could -- thousands
 * of frames a second -- burning a laptop's battery and spinning its fans for
 * frames nobody sees.  Project Settings > Quality has carried a Target
 * Framerate field per quality level all along, with nothing to hand it to:
 * jce_editor_effective_render_settings never exported it and no engine call
 * accepted it.
 *
 * The original controls test the decision. Asserting that a capped loop
 * takes 16.6 ms per frame measures this machine's scheduler, and would be a
 * flaky test of an OS.  The half that can actually be WRONG is where the next
 * deadline goes -- and getting that wrong is not subtle: repaying a hitch by
 * advancing the deadline one budget at a time gives a burst of zero-length
 * frames, which the player sees as a speed-up.  So the policy is a pure
 * function and this drives it with a fake clock. Additional controls include
 * the actual sole wait owner with typed clock/sleep ports to verify interval
 * witnesses, preserving the distinction from native scheduler measurements.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_engine_frame_cap.h"
#include "application/jce_engine_memory_policy.h"
#include "application/jce_engine_window_policy.h"

#include "unity.h"

#include <jce/application/jce_engine.h>

#include <stddef.h>
#include <stdint.h>
#include "application/jce_engine_perf_hitch.h"
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>

/* Actual owner with scoped typed clock/sleep/trace ports; linked production API
 * above remains the setter/getter and pure-policy control. No OS timing. */
static uint64_t wait_frequency, wait_counters[3];
static unsigned wait_reads, wait_freq_reads, wait_sleeps;
static uint32_t wait_sleep_ms;
static struct {
    bool enabled;
    uint64_t next_id, parent_id, begin_id, begin_parent, end_id, end_parent, ns;
    unsigned enabled_reads, id_reads, parent_reads, begins, ends;
    unsigned begins_at_sleep, ends_at_sleep;
    const char *begin_name, *end_name;
} wait_trace;
static uint64_t wait_freq(void) { ++wait_freq_reads; return wait_frequency; }
static uint64_t wait_counter(void)
{
    unsigned slot = wait_reads++;
    return wait_counters[slot < 3u ? slot : 2u];
}
static void wait_sleep(uint32_t ms)
{
    wait_sleep_ms = ms; ++wait_sleeps;
    wait_trace.begins_at_sleep = wait_trace.begins;
    wait_trace.ends_at_sleep = wait_trace.ends;
}
static bool wait_trace_enabled(void) { ++wait_trace.enabled_reads; return wait_trace.enabled; }
static uint64_t wait_trace_next_id(void) { ++wait_trace.id_reads; return wait_trace.next_id; }
static uint64_t wait_trace_parent_id(void) { ++wait_trace.parent_reads; return wait_trace.parent_id; }
static void wait_trace_begin(uint64_t id, uint64_t parent, const char *name)
{
    ++wait_trace.begins; wait_trace.begin_id = id;
    wait_trace.begin_parent = parent; wait_trace.begin_name = name;
}
static void wait_trace_end(uint64_t id, uint64_t parent, const char *name, uint64_t ns)
{
    ++wait_trace.ends; wait_trace.end_id = id; wait_trace.end_parent = parent;
    wait_trace.end_name = name; wait_trace.ns = ns;
}
#define jce_time_perf_freq wait_freq
#define jce_time_perf_counter wait_counter
#define jce_thread_sleep_ms wait_sleep
#define jce_engine_set_target_fps fixture_set_target_fps
#define jce_engine_get_target_fps fixture_get_target_fps
#define jce_engine_set_run_in_background fixture_set_run_in_background
#define jce_engine_get_run_in_background fixture_get_run_in_background
#define jce_engine_frame_cap_advance fixture_frame_cap_advance
#define jce_engine_launch_fps_begin fixture_launch_fps_begin
#define jce_engine_launch_fps_observe fixture_launch_fps_observe
#define jce_engine_launch_fps_end fixture_launch_fps_end
#define jce_trace_enabled wait_trace_enabled
#define jce_trace_next_id wait_trace_next_id
#define jce_trace_task_current_id wait_trace_parent_id
#define jce_trace_wait_begin wait_trace_begin
#define jce_trace_wait_end wait_trace_end
#ifndef JCE_TEST_FRAME_CAP_OWNER
#define JCE_TEST_FRAME_CAP_OWNER "application/jce_engine_frame_cap.inc.h"
#endif
#include JCE_TEST_FRAME_CAP_OWNER
#undef jce_time_perf_freq
#undef jce_time_perf_counter
#undef jce_thread_sleep_ms
#undef jce_engine_set_target_fps
#undef jce_engine_get_target_fps
#undef jce_engine_set_run_in_background
#undef jce_engine_get_run_in_background
#undef jce_engine_frame_cap_advance
#undef jce_engine_launch_fps_begin
#undef jce_engine_launch_fps_observe
#undef jce_engine_launch_fps_end
#undef jce_trace_enabled
#undef jce_trace_next_id
#undef jce_trace_task_current_id
#undef jce_trace_wait_begin
#undef jce_trace_wait_end

static void wait_clock(uint64_t hz, uint64_t decision, uint64_t start, uint64_t end)
{
    wait_frequency = hz; wait_reads = wait_freq_reads = wait_sleeps = 0;
    wait_sleep_ms = 0;
    wait_counters[0] = decision; wait_counters[1] = start; wait_counters[2] = end;
}

/* Linking jce_engine.c drags the embedded-PAK symbols a shipped exe
 * supplies; an empty pak is what the other engine-level tests provide
 * (test_jce_headless_boot.c, test_jce_engine_input_backend.c). */
const unsigned char assets_pak_data[1] = { 0 };
const size_t        assets_pak_data_size = 0;

/* 1 MHz fake clock, 60 fps: a budget of 16666 ticks.  Round numbers would hide
 * a truncation bug in the caller's freq/fps. */
#define TICK_HZ  1000000u
#define BUDGET   (TICK_HZ / 60u)

void setUp(void) { memset(&wait_trace, 0, sizeof wait_trace); }
void tearDown(void) {}

static void test_the_first_capped_frame_does_not_stall(void)
{
    /* A cap that waited on its very first frame would make setting the cap
     * look like a hitch. */
    uint64_t dl = 0;
    const uint64_t now = 5000000u;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0,
        jce_engine_frame_cap_advance(now, BUDGET, &dl),
        "an unset deadline must anchor, not wait");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(now + BUDGET, dl,
        "and the window must start one budget from now");
}

static void test_a_frame_that_finished_early_waits_to_its_deadline(void)
{
    uint64_t dl = 0;
    (void)jce_engine_frame_cap_advance(1000000u, BUDGET, &dl);
    const uint64_t first_deadline = dl;

    /* Finished 10000 ticks early. */
    const uint64_t until =
        jce_engine_frame_cap_advance(first_deadline - 10000u, BUDGET, &dl);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(first_deadline, until,
        "the wait target is this frame's deadline");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(first_deadline + BUDGET, dl,
        "and the NEXT deadline comes from the previous deadline, not from now "
        "-- otherwise the cap drifts late by the sleep's own overshoot every "
        "frame");
}

static void test_the_cap_does_not_drift_over_many_frames(void)
{
    /* The drift bug is invisible in one frame and obvious in a hundred: a
     * deadline anchored on `now` after each wait accumulates the overshoot.
     * Simulate a loop that always wakes 300 ticks LATE and check the hundredth
     * frame is still on the original grid. */
    uint64_t dl = 0;
    const uint64_t start = 2000000u;
    (void)jce_engine_frame_cap_advance(start, BUDGET, &dl);

    uint64_t woke = dl;
    for (int i = 0; i < 100; ++i) {
        const uint64_t until =
            jce_engine_frame_cap_advance(woke - 1000u, BUDGET, &dl);
        TEST_ASSERT_NOT_EQUAL_UINT64(0, until);
        woke = until + 300u;                 /* the OS always overshoots */
    }
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(start + BUDGET * 101u, dl,
        "100 frames of a 300-tick oversleep must not move the grid at all");
}

static void test_a_blown_budget_reanchors_instead_of_catching_up(void)
{
    /* A 200 ms hitch at a 60 cap is twelve budgets.  Advancing one budget at a
     * time would return twelve immediate no-waits -- the loop runs flat out to
     * "catch up", which is a visible speed-up and the one thing a frame cap
     * must never do. */
    uint64_t dl = 0;
    (void)jce_engine_frame_cap_advance(3000000u, BUDGET, &dl);
    const uint64_t hitched = dl + BUDGET * 12u;

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0,
        jce_engine_frame_cap_advance(hitched, BUDGET, &dl),
        "the overrun frame itself must not wait");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(hitched + BUDGET, dl,
        "and the window must restart from the hitch, not from the stale grid");

    /* The very next frame is capped normally again -- the re-anchor must not
     * disable the cap. */
    const uint64_t until =
        jce_engine_frame_cap_advance(hitched + 100u, BUDGET, &dl);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(hitched + BUDGET, until,
        "the frame after a hitch is capped like any other");
}

static void test_a_slightly_late_frame_keeps_the_grid(void)
{
    /* Late, but by less than a whole budget: that is the common case (a frame
     * that took 18 ms at a 16.6 ms cap) and it must NOT re-anchor, or the cap
     * would ratchet slower every time the machine was briefly busy. */
    uint64_t dl = 0;
    (void)jce_engine_frame_cap_advance(4000000u, BUDGET, &dl);
    const uint64_t grid = dl;

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0,
        jce_engine_frame_cap_advance(grid + BUDGET / 2u, BUDGET, &dl),
        "a late frame must not wait");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(grid + BUDGET, dl,
        "but the grid must advance by exactly one budget, not move to `now`");
}

static void test_a_zero_budget_never_waits(void)
{
    /* budget == 0 is how "uncapped" reaches this function if a caller ever
     * divides by a huge fps.  It must be a no-op, not a division by zero or an
     * infinite wait. */
    uint64_t dl = 12345u;
    TEST_ASSERT_EQUAL_UINT64(0, jce_engine_frame_cap_advance(999u, 0u, &dl));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(12345u, dl,
        "and it must not disturb the deadline");
    TEST_ASSERT_EQUAL_UINT64(0, jce_engine_frame_cap_advance(999u, BUDGET, NULL));
}

static void test_the_public_setter_round_trips_and_clamps(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_engine_get_target_fps(),
        "uncapped is the default -- a cap nobody asked for would change every "
        "existing game's frame pacing");
    jce_engine_set_target_fps(30);
    TEST_ASSERT_EQUAL_INT(30, jce_engine_get_target_fps());
    jce_engine_set_target_fps(-1);          /* Unity's "uncapped" */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_engine_get_target_fps(),
        "a negative target is uncapped, not a negative budget");
    jce_engine_set_target_fps(0);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
}

static void test_memory_policy_resolves_each_lifecycle(void)
{
    TEST_ASSERT_TRUE(jce_engine_low_memory_policy(512, 8,
        JCE_MACHINE_CLASS_AUTO, NULL));
    TEST_ASSERT_TRUE(jce_engine_low_memory_policy(8192, 1,
        JCE_MACHINE_CLASS_AUTO, ""));
    TEST_ASSERT_FALSE(jce_engine_low_memory_policy(8192, 8,
        JCE_MACHINE_CLASS_AUTO, NULL));
    TEST_ASSERT_FALSE(jce_engine_low_memory_policy(512, 1,
        JCE_MACHINE_CLASS_FULL, NULL));
    TEST_ASSERT_TRUE(jce_engine_low_memory_policy(8192, 8,
        JCE_MACHINE_CLASS_LOW, NULL));
    TEST_ASSERT_FALSE(jce_engine_low_memory_policy(512, 1,
        JCE_MACHINE_CLASS_LOW, "0"));
    TEST_ASSERT_TRUE(jce_engine_low_memory_policy(8192, 8,
        JCE_MACHINE_CLASS_FULL, "1"));
    /* An earlier LOW result cannot stick when a later boot selects FULL. */
    TEST_ASSERT_TRUE(jce_engine_low_memory_policy(8192, 8,
        JCE_MACHINE_CLASS_LOW, NULL));
    TEST_ASSERT_FALSE(jce_engine_low_memory_policy(8192, 8,
        JCE_MACHINE_CLASS_FULL, NULL));
}

static void test_error_dialog_policy_preserves_only_interactive_requests(void)
{
    for (unsigned mask=0;mask<8;++mask)
        TEST_ASSERT_EQUAL_INT(mask==0,jce_engine_window_policy_error_dialog(
            (mask&1)!=0,(mask&2)!=0,(mask&4)!=0));
}

static void test_nonfocus_window_policy_is_explicit_and_ignored_headless(void)
{
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(false, NULL));
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(false, ""));
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(false, "0"));
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(false, "00enabled"));
    TEST_ASSERT_TRUE(jce_engine_window_policy_not_focusable(false, "1"));
    TEST_ASSERT_TRUE(jce_engine_window_policy_not_focusable(false, "enabled"));
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(true, "1"));
    /* Reuses the existing boolean contract rather than a second word parser. */
    TEST_ASSERT_TRUE(jce_engine_window_policy_not_focusable(false, "false"));
    TEST_ASSERT_FALSE(jce_engine_window_policy_not_focusable(false, "0"));
}

static void test_actual_wait_owner_preserves_grid_and_same_fps(void)
{
    JceEnginePerfHitch s; jce_engine_perf_hitch_init(&s, false, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    fixture_set_target_fps(0); fixture_set_target_fps(30);
    wait_clock(TICK_HZ, 1000000u, 1000000u, 1000000u);
    engine_frame_cap_wait(&s);
    const uint64_t grid = s_frame_cap.deadline;
    TEST_ASSERT_EQUAL_UINT(1, wait_reads); TEST_ASSERT_EQUAL_UINT(0, wait_sleeps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, s.current_pacing.planned_ms);
    fixture_set_target_fps(30);
    TEST_ASSERT_EQUAL_UINT64(grid, s_frame_cap.deadline);
    wait_clock(TICK_HZ, grid - 1000u, grid - 1000u, grid + 300u);
    engine_frame_cap_wait(&s);
    TEST_ASSERT_EQUAL_UINT64(grid + TICK_HZ / 30u, s_frame_cap.deadline);
    TEST_ASSERT_EQUAL_UINT(3, wait_reads); TEST_ASSERT_EQUAL_UINT(0, wait_sleeps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 1.0, s.current_pacing.planned_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 1.3, s.current_pacing.actual_ms);
    TEST_ASSERT_EQUAL_INT(30, s.current_pacing.fps);
    fixture_set_target_fps(10); TEST_ASSERT_EQUAL_UINT64(0, s_frame_cap.deadline);
    wait_clock(TICK_HZ, 2000000u, 2000000u, 2000000u); engine_frame_cap_wait(&s);
    TEST_ASSERT_EQUAL_UINT64(2100000u, s_frame_cap.deadline);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, s.current_pacing.planned_ms);
    fixture_set_target_fps(0); wait_clock(TICK_HZ, 0, 0, 0); engine_frame_cap_wait(&s);
    TEST_ASSERT_EQUAL_UINT(0, wait_reads); TEST_ASSERT_EQUAL_INT(0, s.current_pacing.fps);
    TEST_ASSERT_FALSE(s.current_pacing.clock_valid);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_wait_owner_invalid_clock_or_budget_never_exempts(void)
{
    JceEnginePerfHitch s; jce_engine_perf_hitch_init(&s, false, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    fixture_set_target_fps(30); s_frame_cap.deadline = 1032000u;
    wait_clock(0, 1000000u, 1000000u, 1032000u); engine_frame_cap_wait(&s);
    TEST_ASSERT_FALSE(s.current_pacing.clock_valid); TEST_ASSERT_EQUAL_UINT(0, wait_reads);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, s.current_pacing.planned_ms);
    wait_clock(10, 1000000u, 1000000u, 1032000u); engine_frame_cap_wait(&s);
    TEST_ASSERT_FALSE(s.current_pacing.clock_valid); TEST_ASSERT_EQUAL_UINT(1, wait_reads);
    TEST_ASSERT_EQUAL_UINT64(1032000u, s_frame_cap.deadline);
    wait_clock(TICK_HZ, 1000000u, 999999u, 1032000u); engine_frame_cap_wait(&s);
    TEST_ASSERT_FALSE(s.current_pacing.clock_valid);
    TEST_ASSERT_EQUAL_UINT(3, wait_reads); TEST_ASSERT_EQUAL_UINT(1, wait_sleeps);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_wait_trace_preserves_parent_duration_and_clock_reads(void)
{
    JceEnginePerfHitch s; jce_engine_perf_hitch_init(&s, false, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    fixture_set_target_fps(30); s_frame_cap.deadline = 1032000u;
    wait_trace.enabled = true;
    wait_trace.next_id = UINT64_C(0x123456789abcdef0);
    wait_trace.parent_id = UINT64_C(0xfedcba9876543210);
    wait_clock(TICK_HZ, 1000000u, 1000000u, 1132000u);
    engine_frame_cap_wait(&s);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.begins);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, wait_trace.ends, "actual pacing wait must close its WAIT span");
    TEST_ASSERT_EQUAL_UINT64(wait_trace.next_id, wait_trace.begin_id);
    TEST_ASSERT_EQUAL_UINT64(wait_trace.begin_id, wait_trace.end_id);
    TEST_ASSERT_EQUAL_UINT64(wait_trace.parent_id, wait_trace.begin_parent);
    TEST_ASSERT_EQUAL_UINT64(wait_trace.begin_parent, wait_trace.end_parent);
    TEST_ASSERT_EQUAL_STRING("engine.frame_pacing", wait_trace.begin_name);
    TEST_ASSERT_EQUAL_STRING(wait_trace.begin_name, wait_trace.end_name);
    TEST_ASSERT_UINT64_WITHIN(1, UINT64_C(132000000), wait_trace.ns);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.enabled_reads);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.id_reads);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.parent_reads);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.begins_at_sleep);
    TEST_ASSERT_EQUAL_UINT(0, wait_trace.ends_at_sleep);
    TEST_ASSERT_EQUAL_UINT(1, wait_freq_reads);
    TEST_ASSERT_EQUAL_UINT(3, wait_reads);
    TEST_ASSERT_EQUAL_UINT(1, wait_sleeps);
    TEST_ASSERT_EQUAL_UINT32(31, wait_sleep_ms);
    TEST_ASSERT_EQUAL_UINT64(1032000u + TICK_HZ / 30u, s_frame_cap.deadline);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 32.0, s.current_pacing.planned_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 132.0, s.current_pacing.actual_ms);
    TEST_ASSERT_TRUE(s.current_pacing.clock_valid);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_wait_trace_disabled_keeps_wait_and_clock_reads(void)
{
    fixture_set_target_fps(30); s_frame_cap.deadline = 1032000u;
    wait_clock(TICK_HZ, 1000000u, 1000000u, 1032000u);
    engine_frame_cap_wait(NULL);
    TEST_ASSERT_EQUAL_UINT(1, wait_trace.enabled_reads);
    TEST_ASSERT_EQUAL_UINT(0, wait_trace.id_reads);
    TEST_ASSERT_EQUAL_UINT(0, wait_trace.parent_reads);
    TEST_ASSERT_EQUAL_UINT(0, wait_trace.begins);
    TEST_ASSERT_EQUAL_UINT(0, wait_trace.ends);
    TEST_ASSERT_EQUAL_UINT(1, wait_freq_reads);
    TEST_ASSERT_EQUAL_UINT(3, wait_reads);
    TEST_ASSERT_EQUAL_UINT(1, wait_sleeps);
    TEST_ASSERT_EQUAL_UINT32(31, wait_sleep_ms);
}

static void test_actual_wait_trace_no_wait_or_invalid_clock_publishes_no_wait(void)
{
    /* Uncapped, no clock, zero budget, first frame, overrun, deadline passed
     * between samples and a regressed clock. None admits a WAIT event. */
    const struct {
        int fps;
        uint64_t hz, deadline, decision, start, end;
        unsigned freq_reads, reads, sleeps;
    } cases[] = {
        {0, TICK_HZ, 1032000u, 1000000u, 1000000u, 1032000u, 0, 0, 0},
        {30, 0, 1032000u, 1000000u, 1000000u, 1032000u, 1, 0, 0},
        {30, 10, 1032000u, 1000000u, 1000000u, 1032000u, 1, 1, 0},
        {30, TICK_HZ, 0, 1000000u, 1000000u, 1032000u, 1, 1, 0},
        {30, TICK_HZ, 1032000u, 1200000u, 1200000u, 1200000u, 1, 1, 0},
        {30, TICK_HZ, 1032000u, 1000000u, 1032000u, 1032000u, 1, 2, 0},
        {30, TICK_HZ, 1032000u, 1000000u, 999999u, 1032000u, 1, 3, 1}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(&wait_trace, 0, sizeof wait_trace); wait_trace.enabled = true;
        fixture_set_target_fps(cases[i].fps); s_frame_cap.deadline = cases[i].deadline;
        wait_clock(cases[i].hz, cases[i].decision, cases[i].start, cases[i].end);
        engine_frame_cap_wait(NULL);
        TEST_ASSERT_EQUAL_UINT(cases[i].freq_reads, wait_freq_reads);
        TEST_ASSERT_EQUAL_UINT(cases[i].reads, wait_reads);
        TEST_ASSERT_EQUAL_UINT(cases[i].sleeps, wait_sleeps);
        TEST_ASSERT_EQUAL_UINT(0, wait_trace.enabled_reads);
        TEST_ASSERT_EQUAL_UINT(0, wait_trace.id_reads);
        TEST_ASSERT_EQUAL_UINT(0, wait_trace.parent_reads);
        TEST_ASSERT_EQUAL_UINT(0, wait_trace.begins);
        TEST_ASSERT_EQUAL_UINT(0, wait_trace.ends);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_memory_policy_resolves_each_lifecycle);
    RUN_TEST(test_nonfocus_window_policy_is_explicit_and_ignored_headless);
    RUN_TEST(test_error_dialog_policy_preserves_only_interactive_requests);
    RUN_TEST(test_the_first_capped_frame_does_not_stall);
    RUN_TEST(test_a_frame_that_finished_early_waits_to_its_deadline);
    RUN_TEST(test_the_cap_does_not_drift_over_many_frames);
    RUN_TEST(test_a_blown_budget_reanchors_instead_of_catching_up);
    RUN_TEST(test_a_slightly_late_frame_keeps_the_grid);
    RUN_TEST(test_a_zero_budget_never_waits);
    RUN_TEST(test_the_public_setter_round_trips_and_clamps);
    RUN_TEST(test_actual_wait_owner_preserves_grid_and_same_fps);
    RUN_TEST(test_actual_wait_owner_invalid_clock_or_budget_never_exempts);
    RUN_TEST(test_actual_wait_trace_preserves_parent_duration_and_clock_reads);
    RUN_TEST(test_actual_wait_trace_disabled_keeps_wait_and_clock_reads);
    RUN_TEST(test_actual_wait_trace_no_wait_or_invalid_clock_publishes_no_wait);
    return UNITY_END();
}
