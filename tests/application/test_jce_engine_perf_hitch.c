/* Real production owner and real core snapshots; no renderer or timing sleep. */
#ifndef JCE_TEST_PERF_HITCH_OWNER
#define JCE_TEST_PERF_HITCH_OWNER "application/jce_engine_perf_hitch.c"
#endif
#include JCE_TEST_PERF_HITCH_OWNER
#include <stddef.h>
#include <jce/os/core/jce_perf_phase.h>
#include "unity.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>

/* Typed ports around the actual sole wait owner, not an OS timing assertion. */
static uint64_t pacing_frequency, pacing_counters[3];
static unsigned pacing_reads, pacing_sleeps;
static uint32_t pacing_sleep_ms;
static uint64_t pacing_freq(void) { return pacing_frequency; }
static uint64_t pacing_counter(void)
{
    unsigned slot = pacing_reads++;
    return pacing_counters[slot < 3u ? slot : 2u];
}
static void pacing_sleep(uint32_t ms) { ++pacing_sleeps; pacing_sleep_ms = ms; }
#define jce_time_perf_freq pacing_freq
#define jce_time_perf_counter pacing_counter
#define jce_thread_sleep_ms pacing_sleep
#ifndef JCE_TEST_FRAME_CAP_OWNER
#define JCE_TEST_FRAME_CAP_OWNER "application/jce_engine_frame_cap.inc.h"
#endif
#include JCE_TEST_FRAME_CAP_OWNER
#undef jce_time_perf_freq
#undef jce_time_perf_counter
#undef jce_thread_sleep_ms

static void pacing_clock(uint64_t decision, uint64_t start, uint64_t end)
{
    pacing_frequency = 1000000u;
    pacing_counters[0] = decision; pacing_counters[1] = start; pacing_counters[2] = end;
    pacing_reads = pacing_sleeps = 0; pacing_sleep_ms = 0;
}

void setUp(void) { jce_perf_phase_set_enabled(1); }
void tearDown(void) {}
static void step(JceEnginePerfHitch *state, double ms, JceEnginePerfHitchReport *hitch,
                 JceEnginePerfWindow *window, bool full)
{
    jce_engine_perf_hitch_begin_interval(state);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(state, ms, true, hitch, window));
    jce_engine_perf_hitch_end_interval(state, full);
}
static void warm(JceEnginePerfHitch *state)
{
    JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    jce_engine_perf_hitch_init(state, true, 4.0);
    for (unsigned i = 0; i < 152; ++i) step(state, 1.0, &h, &w, true);
    TEST_ASSERT_EQUAL_UINT(32, state->count);
}
static void test_initial_unavailable_is_not_zero(void)
{
    TEST_ASSERT_FALSE(jce_perf_phase_frame_has_data());
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char line[1024]; warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_TRUE(h.ready);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_UNAVAILABLE, h.phase_status);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NOT_NULL(strstr(line, "NO PHASE SNAPSHOT"));
}
static void test_sampled_all_zero_is_available(void)
{
    jce_perf_phase_add("available_zero", 0.0); jce_perf_phase_frame_tick();
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char line[1024]; warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_TRUE(jce_perf_phase_frame_has_data());
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_ZERO, h.phase_status);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NOT_NULL(strstr(line, "sampled snapshot; no positive duration"));
    TEST_ASSERT_NULL(strstr(line, "outside every"));
}
static void test_A_wall_captures_A_then_B_cannot_change_log(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char before[1024], after[1024]; warm(&s);
    jce_perf_phase_add("end_frame", 90.0); jce_perf_phase_frame_tick();
    step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_DOUBLE_WITHIN(0.0001, 100.0, h.wall_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 132.0 / 33.0, h.mean_ms);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, before, sizeof before));
    TEST_ASSERT_NOT_NULL(strstr(before, "end_frame=90.00"));
    jce_perf_phase_add("app_update", 0.8); jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, after, sizeof after));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_NULL(strstr(after, "app_update=0.80"));
    step(&s, 1.0, &h, &w, true); TEST_ASSERT_FALSE(h.ready);
}
static void test_skipped_interval_does_not_reuse_everavailable_snapshot(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char line[1024]; warm(&s);
    jce_perf_phase_add("old_render", 90.0); jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    jce_engine_perf_hitch_end_interval(&s, false); /* minimized/fallback/early return */
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_FALSE(s.interval_eligible);
    jce_perf_phase_frame_tick(); /* no new sample: core preserves prior snapshot */
    TEST_ASSERT_TRUE(jce_perf_phase_frame_has_data());
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_INELIGIBLE, h.phase_status);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NULL(strstr(line, "old_render=90.00"));
    TEST_ASSERT_NOT_NULL(strstr(line, "discontinuous interval"));
    jce_engine_perf_hitch_end_interval(&s, true);
    jce_perf_phase_add("fresh_render", 91.0); jce_perf_phase_frame_tick();
    step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_POSITIVE, h.phase_status);
    TEST_ASSERT_NOT_NULL(strstr(h.phases, "fresh_render=91.00"));
}
static void test_ordinal_advances_without_autostop_and_duplicate_observe_is_rejected(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    warm(&s); uint64_t ordinal = s.interval;
    jce_engine_perf_hitch_begin_interval(&s);
    TEST_ASSERT_EQUAL_UINT64(ordinal + 1, s.interval);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_TRUE(h.ready);
    JceEnginePerfHitchReport captured = h;
    JceEnginePerfWindow captured_window = w;
    JceEnginePerfHitch before = s;
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_observe_completed(&s, 1.0, true, &h, &w));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof s);
    TEST_ASSERT_EQUAL_MEMORY(&captured, &h, sizeof h);
    TEST_ASSERT_EQUAL_MEMORY(&captured_window, &w, sizeof w);
    jce_engine_perf_hitch_end_interval(&s, true);
}
static void test_nested_entry_cannot_change_outer_state(void)
{
    JceEnginePerfHitch s; warm(&s);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    JceEnginePerfHitch before = s;
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof s);
    TEST_ASSERT_TRUE(s.iterating);
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_FALSE(s.iterating);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_TRUE(s.interval_eligible);
    TEST_ASSERT_EQUAL_UINT64(before.interval + 1, s.interval);
    jce_engine_perf_hitch_end_interval(&s, true);
}
static void test_late_slot_and_top_eight_use_full_public_capacity(void)
{
    static char names[40][32];
    for (unsigned i = 0; i < 40; ++i) {
        snprintf(names[i], sizeof names[i], "late_phase_%u", i);
        jce_perf_phase_add(names[i], (double)i + 1.0);
    }
    jce_perf_phase_frame_tick();
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_EQUAL_UINT(8, h.top_count);
    TEST_ASSERT_NOT_NULL(strstr(h.phases, "late_phase_39=40.00"));
    TEST_ASSERT_NOT_NULL(strstr(h.phases, "late_phase_32=33.00"));
    TEST_ASSERT_NULL(strstr(h.phases, "late_phase_31="));
}
static void test_names_are_ascii_escaped_and_long_entry_is_explicitly_truncated(void)
{
    jce_perf_phase_add("line\n=break|\x01", 95.0); jce_perf_phase_frame_tick();
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_NOT_NULL(strstr(h.phases, "line\\x0a\\x3dbreak\\x7c\\x01=95.00"));
    TEST_ASSERT_NULL(strchr(h.phases, '\n'));
    static char long_name[1024]; memset(long_name, 'a', sizeof long_name - 1);
    jce_perf_phase_add(long_name, 96.0); jce_perf_phase_frame_tick();
    warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_TRUE(h.truncated); TEST_ASSERT_EQUAL_STRING("", h.phases);
}
static void test_invalid_input_and_small_output_fail_without_history_damage(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    warm(&s); jce_engine_perf_hitch_begin_interval(&s); JceEnginePerfHitch before = s;
    const double values[] = {NAN, INFINITY, -1.0};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        TEST_ASSERT_FALSE(jce_engine_perf_hitch_observe_completed(&s, values[i], true, &h, &w));
        TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof s);
    }
    jce_perf_phase_add("valid_phase", 90.0); jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    struct { char data[8]; char guard[8]; } bytes;
    memset(&bytes, '#', sizeof bytes);
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_format(&h, bytes.data, sizeof bytes.data));
    TEST_ASSERT_EQUAL_STRING("", bytes.data);
    TEST_ASSERT_EQUAL_MEMORY("########", bytes.guard, sizeof bytes.guard);
    jce_engine_perf_hitch_init(&s, true, NAN); TEST_ASSERT_DOUBLE_WITHIN(0.0001, 4.0, s.factor);
}
static void test_invalid_snapshot_does_not_forge_duration(void)
{
    jce_perf_phase_add("invalid_sample", NAN); jce_perf_phase_frame_tick();
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char line[1024]; warm(&s); step(&s, 100.0, &h, &w, true);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_INVALID, h.phase_status);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NOT_NULL(strstr(line, "invalid phase samples"));
    TEST_ASSERT_NULL(strstr(line, "invalid_sample=0"));
}
static void test_window_policy_and_lifecycle_reset(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    jce_engine_perf_hitch_init(&s, false, 4.0);
    jce_engine_perf_hitch_begin_interval(&s);
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_observe_completed(&s, 1.0, true, &h, &w));
    TEST_ASSERT_EQUAL_UINT(0, s.window_count);
    jce_engine_perf_hitch_init(&s, true, 4.0);
    for (unsigned i = 0; i < 120; ++i) step(&s, 1.0, &h, &w, true);
    TEST_ASSERT_TRUE(w.ready); TEST_ASSERT_EQUAL_UINT(120, w.count);
    TEST_ASSERT_EQUAL_UINT(0, w.ring_count); TEST_ASSERT_DOUBLE_WITHIN(0.0001, 1.0, w.average_ms);
    for (unsigned i = 1; i <= 120; ++i) step(&s, (double)i, &h, &w, true);
    TEST_ASSERT_TRUE(w.ready); TEST_ASSERT_EQUAL_UINT(120, w.ring_count);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 60.5, w.average_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 120.0, w.worst_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 60.0, w.p50_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 114.0, w.p95_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 119.0, w.p99_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 120.0, w.p999_ms);
    for (unsigned i = 0; i < 2100; ++i) step(&s, 1.0, &h, &w, true);
    TEST_ASSERT_EQUAL_UINT(JCE_ENGINE_PERF_RING, s.count);
    jce_engine_perf_hitch_init(&s, false, 4.0);
    TEST_ASSERT_FALSE(s.enabled); TEST_ASSERT_FALSE(s.started); TEST_ASSERT_FALSE(s.previous_full);
    TEST_ASSERT_EQUAL_UINT(0, s.window_count); TEST_ASSERT_EQUAL_UINT(0, s.logged);
    TEST_ASSERT_EQUAL_UINT(0, s.count); TEST_ASSERT_EQUAL_UINT64(0, s.interval);
}
static void test_phase_reads_and_window_report_do_not_consume_snapshot(void)
{
    jce_perf_phase_add("stable_sample", 90.0); jce_perf_phase_frame_tick();
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    char window[512]; warm(&s); step(&s, 100.0, &h, &w, true);
    JceEnginePerfHitchReport captured = h;
    jce_perf_phase_report_average(window, sizeof window, 120);
    jce_perf_phase_frame_tick();
    TEST_ASSERT_EQUAL_MEMORY(&captured, &h, sizeof h);
    bool found = false;
    for (int i = 0; i < jce_perf_phase_count(); ++i) {
        const char *name = NULL; double ms = 0;
        jce_perf_phase_peek_frame(i, &name, &ms);
        if (!strcmp(name, "stable_sample")) { found = true; TEST_ASSERT_DOUBLE_WITHIN(0.0001, 90.0, ms); }
    }
    TEST_ASSERT_TRUE(found);
}

static void test_zero_wall_advances_history_without_forging_a_hitch(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    jce_engine_perf_hitch_init(&s, true, 4.0);
    for (unsigned i = 0; i < 120; ++i) {
        step(&s, 0.0, &h, &w, true);
        TEST_ASSERT_FALSE(h.ready);
    }
    TEST_ASSERT_EQUAL_UINT(120, s.warm);
    TEST_ASSERT_TRUE(w.ready); TEST_ASSERT_EQUAL_UINT(120, w.count);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, w.average_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, w.worst_ms);
    for (unsigned i = 0; i < 32; ++i) step(&s, 0.0, &h, &w, true);
    TEST_ASSERT_EQUAL_UINT(32, s.count); TEST_ASSERT_EQUAL_UINT(32, s.window_count);
    TEST_ASSERT_DOUBLE_WITHIN(0.0, 0.0, s.window_sum_ms);
    step(&s, 1.0, &h, &w, true); TEST_ASSERT_FALSE(h.ready); /* floor8ms */
    step(&s, 100.0, &h, &w, true); TEST_ASSERT_TRUE(h.ready);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 101.0 / 34.0, h.mean_ms);
}
static void test_public_phase_disable_reenable_does_not_reuse_old_snapshot(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    warm(&s);
    jce_perf_phase_add("before_disable", 90.0); jce_perf_phase_frame_tick();
    /* Starting armed but closing disabled cannot certify a measured interval. */
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    jce_perf_phase_set_enabled(0);
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_FALSE(s.previous_phase_armed);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_FALSE(s.interval_eligible);
    jce_perf_phase_add("disabled_ignored", 91.0);
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_FALSE(s.previous_phase_armed);
    jce_perf_phase_set_enabled(1);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    jce_perf_phase_frame_tick(); /* No pending B samples: the old Z remains available. */
    TEST_ASSERT_TRUE(jce_perf_phase_frame_has_data());
    TEST_ASSERT_FALSE(s.interval_eligible);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_INELIGIBLE, h.phase_status);
    TEST_ASSERT_NULL(strstr(h.phases, "before_disable=90.00"));
    jce_perf_phase_add("after_reenable_zero", 0.0);
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(s.interval_eligible);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_ZERO, h.phase_status);
    jce_engine_perf_hitch_end_interval(&s, true);
}
static void test_exact_phase_text_boundaries_preserve_padding_and_guard(void)
{
    static char boundary_names[3][379];
    const unsigned lengths[] = {376, 377, 378};
    for (unsigned c = 0; c < 3; ++c) {
        memset(boundary_names[c], 'a', lengths[c]);
        boundary_names[c][lengths[c]] = 0;
        jce_perf_phase_add(boundary_names[c], 95.0);
        jce_perf_phase_add("", 94.0); /* Valid public API phase name. */
        jce_perf_phase_frame_tick();
        struct { JceEnginePerfHitchReport report; unsigned char guard[8]; } bytes;
        memset(&bytes, 0xa5, sizeof bytes);
        bytes.report.interval = 0; bytes.report.wall_ms = 0; bytes.report.mean_ms = 0;
        bytes.report.phase_status = JCE_ENGINE_PERF_PHASE_UNAVAILABLE;
        bytes.report.top_count = 0; bytes.report.ready = false; bytes.report.truncated = false;
        bool exact_name_present = false;
        for (int slot = 0; slot < jce_perf_phase_count(); ++slot) {
            const char *name = NULL;
            TEST_ASSERT_TRUE(jce_perf_phase_peek_frame(slot, &name, NULL));
            if (name == boundary_names[c]) {
                exact_name_present = true; TEST_ASSERT_EQUAL_UINT(lengths[c], strlen(name));
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(exact_name_present, "public phase owner must preserve the exact name");
        capture_phases(&bytes.report, true); /* Exact production static routine. */
        TEST_ASSERT_EQUAL_INT(JCE_ENGINE_PERF_PHASE_POSITIVE, bytes.report.phase_status);
        TEST_ASSERT_TRUE(bytes.report.truncated);
        TEST_ASSERT_EQUAL_UINT(c == 2 ? 0 : 1, bytes.report.top_count);
        TEST_ASSERT_EQUAL_UINT(c == 2 ? 0 : lengths[c] + 6, strlen(bytes.report.phases));
        const unsigned char *raw = (const unsigned char *)&bytes;
        const size_t end = offsetof(JceEnginePerfHitchReport, phases) +
                           sizeof bytes.report.phases;
        for (size_t i = end; i < sizeof bytes; ++i)
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xa5, raw[i], "phase formatter wrote padding/guard");
    }
}
static void test_unterminated_private_payload_fails_bounded_format(void)
{
    JceEnginePerfHitchReport h = {0};
    h.ready = true; h.wall_ms = 100; h.mean_ms = 4;
    h.phase_status = JCE_ENGINE_PERF_PHASE_POSITIVE;
    memset(h.phases, '#', sizeof h.phases);
    struct { char text[8]; char guard[8]; } bytes;
    memset(&bytes, '#', sizeof bytes);
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_format(&h, bytes.text, sizeof bytes.text));
    TEST_ASSERT_EQUAL_STRING("", bytes.text);
    TEST_ASSERT_EQUAL_MEMORY("########", bytes.guard, sizeof bytes.guard);
}

static void test_initial_entry_and_skips_have_no_completed_render(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    TEST_ASSERT_FALSE(state.interval_completed);
    jce_engine_perf_hitch_end_interval(&state, false);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    TEST_ASSERT_FALSE(state.interval_completed);
    jce_engine_perf_hitch_end_interval(&state, true);
    jce_perf_phase_set_enabled(0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    TEST_ASSERT_TRUE(state.interval_completed);
    TEST_ASSERT_FALSE(state.interval_eligible);
    jce_engine_perf_hitch_end_interval(&state, false);
}

static void test_terminal_completion_rotates_current_snapshot_once_under_gate(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick(); /* Ordinary entry boundary. */
    jce_perf_phase_add("terminal_current", 191.75);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_TRUE(state.iterating);
    TEST_ASSERT_TRUE(state.interval_completed);
    TEST_ASSERT_TRUE(state.interval_eligible);
    TEST_ASSERT_EQUAL_UINT64(1, state.interval);
    bool found = false;
    for (int i = 0; i < jce_perf_phase_count(); ++i) {
        const char *name = NULL; double ms = 0.0;
        jce_perf_phase_peek_frame(i, &name, &ms);
        if (name && strcmp(name, "terminal_current") == 0) {
            found = true;
            TEST_ASSERT_DOUBLE_WITHIN(0.000001, 191.75, ms);
        }
    }
    TEST_ASSERT_TRUE(found);
    JceEnginePerfHitch before = state;
    jce_perf_phase_add("after_terminal_boundary", 777.0);
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof state);
    jce_engine_perf_hitch_end_interval(&state, true);
    TEST_ASSERT_FALSE(state.iterating);
    TEST_ASSERT_FALSE(state.previous_full);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    TEST_ASSERT_FALSE(state.interval_completed); /* Do not export twice. */
    jce_engine_perf_hitch_end_interval(&state, false);
}

static void test_terminal_disabled_phases_are_completed_but_not_attributed(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, true, 4.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    jce_perf_phase_add("terminal_partial_disabled", 90.0);
    jce_perf_phase_set_enabled(0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_TRUE(state.interval_completed);
    TEST_ASSERT_FALSE(state.interval_eligible);
    jce_engine_perf_hitch_end_interval(&state, true);
    jce_perf_phase_set_enabled(1);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick(); /* Clear the disabled interval's old pending data. */
    TEST_ASSERT_FALSE(state.interval_completed);
    jce_perf_phase_add("terminal_fresh_zero", 0.0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_TRUE(state.interval_eligible);
    TEST_ASSERT_TRUE(jce_perf_phase_frame_has_data());
    jce_engine_perf_hitch_end_interval(&state, true);
}

static void test_terminal_completion_without_entry_changes_nothing(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, true, 4.0);
    JceEnginePerfHitch before = state;
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_complete_terminal_interval(NULL));
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof state);
}

static void wait_then_observe(JceEnginePerfHitch *s, double wall_ms,
    JceEnginePerfHitchReport *h, JceEnginePerfWindow *w)
{
    if (!s->iterating) TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(s));
    engine_frame_cap_wait(s); /* Exact production wait-to-reporter wire. */
    jce_engine_perf_hitch_end_interval(s, true);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(s));
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(s, wall_ms, true, h, w));
}

static void test_actual_owner_normal_wait_keeps_raw_wall_and_selected_rate(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
    jce_engine_set_target_fps(0); jce_engine_set_target_fps(30);
    pacing_clock(1000000u, 1000000u, 1000000u);
    wait_then_observe(&s, 1.0, &h, &w); /* First anchor is no exemption. */
    TEST_ASSERT_EQUAL_UINT(1, pacing_reads); TEST_ASSERT_EQUAL_UINT(0, pacing_sleeps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, s.completed_pacing.planned_ms);
    s_frame_cap.deadline = 2032000u;
    pacing_clock(2000000u, 2000000u, 2032000u);
    engine_frame_cap_wait(&s);
    TEST_ASSERT_EQUAL_UINT(3, pacing_reads); TEST_ASSERT_EQUAL_UINT(1, pacing_sleeps);
    TEST_ASSERT_EQUAL_UINT(31, pacing_sleep_ms);
    jce_engine_set_target_fps(0); /* A later mode cannot rewrite the selected FPS. */
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 33.0, true, &h, &w));
    TEST_ASSERT_FALSE(h.ready); TEST_ASSERT_EQUAL_INT(30, s.completed_pacing.fps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 32.0, s.completed_pacing.actual_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 33.0, s.ring[s.head - 1u].wall_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 1.0, s.ring[s.head - 1u].comparison_ms);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_owner_oversleep_remains_a_hitch(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
    jce_engine_set_target_fps(30); s_frame_cap.deadline = 1032000u;
    pacing_clock(1000000u, 1000000u, 1132000u);
    wait_then_observe(&s, 133.0, &h, &w);
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_EQUAL_INT(30, h.pacing.fps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 133.0, h.wall_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 101.0, h.comparison_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 32.0, h.pacing.planned_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 132.0, h.pacing.actual_ms);
    char line[1024]; TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NOT_NULL(strstr(line, "raw wall 133.00 ms"));
    TEST_ASSERT_NOT_NULL(strstr(line, "comparison 101.00 ms"));
    TEST_ASSERT_NOT_NULL(strstr(line, "planned 32.00 ms actual 132.00 ms fps 30"));
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_owner_work_stall_and_overrun_are_not_exempted(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
    jce_perf_phase_add("app_update", 27.0); jce_perf_phase_frame_tick();
    jce_engine_set_target_fps(30); s_frame_cap.deadline = 1000000u;
    pacing_clock(1100000u, 1100000u, 1100000u);
    wait_then_observe(&s, 27.0, &h, &w);
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_EQUAL_UINT(0, pacing_sleeps);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 27.0, h.comparison_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, h.pacing.planned_ms);
    TEST_ASSERT_NOT_NULL(strstr(h.phases, "app_update=27.00"));
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_actual_owner_uncapped_capped_uncapped_does_not_mix_wall_baseline(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
    const uint32_t prior_count = s.count; const uint64_t prior_interval = s.interval;
    jce_engine_set_target_fps(30);
    bool raw_window = false;
    for (unsigned i = 0; i < 120; ++i) {
        s_frame_cap.deadline = 1032000u;
        pacing_clock(1000000u, 1000000u, 1032000u);
        wait_then_observe(&s, 33.0, &h, &w); TEST_ASSERT_FALSE(h.ready);
        if (w.ready) {
            raw_window = true;
            TEST_ASSERT_DOUBLE_WITHIN(0.00001, (32.0 + 88.0 * 33.0) / 120.0, w.average_ms);
            TEST_ASSERT_DOUBLE_WITHIN(0.00001, 33.0, w.p50_ms);
            TEST_ASSERT_DOUBLE_WITHIN(0.00001, 33.0, w.worst_ms);
        }
    }
    TEST_ASSERT_TRUE(raw_window); TEST_ASSERT_EQUAL_UINT(prior_count + 120u, s.count);
    TEST_ASSERT_EQUAL_UINT64(prior_interval + 121u, s.interval);
    jce_engine_set_target_fps(0); pacing_clock(0, 0, 0);
    wait_then_observe(&s, 27.0, &h, &w);
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_EQUAL_UINT(0, pacing_reads);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 27.0, h.comparison_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, (152.0 + 27.0) / 153.0, h.mean_ms);
    TEST_ASSERT_EQUAL_INT(0, h.pacing.fps);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_skip_and_unwaited_terminal_cannot_reuse_previous_wait(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
    jce_engine_set_target_fps(30); s_frame_cap.deadline = 1032000u;
    pacing_clock(1000000u, 1000000u, 1032000u); wait_then_observe(&s, 33.0, &h, &w);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&s));
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_DOUBLE_WITHIN(0.00001, 100.0, h.comparison_ms);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 0.0, h.pacing.planned_ms);
    jce_engine_perf_hitch_end_interval(&s, true);
    TEST_ASSERT_FALSE(s.previous_full);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    s_frame_cap.deadline = 2032000u; pacing_clock(2000000u, 2000000u, 2032000u);
    engine_frame_cap_wait(&s); jce_engine_perf_hitch_end_interval(&s, false);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 100.0, true, &h, &w));
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_FALSE(s.interval_completed);
    TEST_ASSERT_DOUBLE_WITHIN(0.00001, 100.0, h.comparison_ms);
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void test_invalid_pacing_never_rejects_raw_wall_or_forges_exemption(void)
{
    const JceEnginePerfPacing invalid[] = {
        {30, NAN, 32, true}, {30, 32, INFINITY, true}, {30, 32, 31, true},
        {30, 32, 34, true}, {-1, 32, 32, true}, {0, 32, 32, true}, {30, 32, 32, false}
    };
    for (unsigned i = 0; i <= sizeof invalid / sizeof invalid[0]; ++i) {
        JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w; warm(&s);
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
        JceEnginePerfPacing valid = {30, 32, 32, true};
        jce_engine_perf_hitch_record_pacing(&s, i ? &invalid[i - 1u] : &valid);
        jce_engine_perf_hitch_end_interval(&s, true);
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_observe_completed(&s, 33.0, i != 0, &h, &w));
        TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_FALSE(h.pacing.clock_valid);
        TEST_ASSERT_DOUBLE_WITHIN(0.00001, 33.0, h.comparison_ms);
        TEST_ASSERT_EQUAL_UINT(33, s.count);
        jce_engine_perf_hitch_end_interval(&s, false);
    }
}

static void test_zero_comparison_baseline_formats_without_division(void)
{
    JceEnginePerfHitch s; JceEnginePerfHitchReport h; JceEnginePerfWindow w;
    jce_engine_perf_hitch_init(&s, true, 4.0); jce_engine_set_target_fps(30);
    for (unsigned i = 0; i < 152; ++i) {
        s_frame_cap.deadline = 1032000u; pacing_clock(1000000u, 1000000u, 1032000u);
        wait_then_observe(&s, 32.0, &h, &w); TEST_ASSERT_FALSE(h.ready);
    }
    jce_engine_set_target_fps(0); wait_then_observe(&s, 8.0, &h, &w);
    TEST_ASSERT_TRUE(h.ready); TEST_ASSERT_DOUBLE_WITHIN(0.00001, 8.0 / 33.0, h.mean_ms);
    h.comparison_ms = h.mean_ms = 0.0;
    char line[1024]; TEST_ASSERT_TRUE(jce_engine_perf_hitch_format(&h, line, sizeof line));
    TEST_ASSERT_NOT_NULL(strstr(line, "comparison 0.00 ms (mean 0.00 ms)"));
    jce_engine_perf_hitch_end_interval(&s, false);
}

static void disposition_entry(JceEnginePerfHitch *s, uint64_t boundary, uint64_t row)
{
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(s));
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_boundary(s,1000,boundary,row));
}
static void disposition_exit(JceEnginePerfHitch *s, uint32_t kind, bool full)
{
    jce_engine_perf_disposition_reason(s,kind);
    jce_engine_perf_disposition_release(s,full);
    jce_engine_perf_hitch_end_interval(s,full);
}
static void test_disposition_raw_boundaries_partition_reasons_and_terminal_once(void)
{
    JceEnginePerfHitch s;
    jce_engine_perf_hitch_init(&s,false,4.0);
    jce_engine_perf_disposition_init(&s,true,1000);
    disposition_entry(&s,0,0);
    JceEnginePerfDisposition before=s.disposition;
    TEST_ASSERT_FALSE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_EQUAL_MEMORY(&before,&s.disposition,sizeof before);
    disposition_exit(&s,JCE_ENGINE_DISPOSITION_HEADLESS,false);
    disposition_entry(&s,3,0);
    disposition_exit(&s,JCE_ENGINE_DISPOSITION_DRAWABLE+3u,false);
    disposition_entry(&s,8,0);
    disposition_exit(&s,JCE_ENGINE_DISPOSITION_CLOSING,true);
    disposition_entry(&s,13,1);
    jce_engine_perf_disposition_reason(&s,JCE_ENGINE_DISPOSITION_GUARD);
    jce_engine_perf_disposition_release(&s,false);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&s,1000,17));
    TEST_ASSERT_EQUAL_UINT64(4,s.disposition.intervals);
    TEST_ASSERT_EQUAL_UINT64(1,s.disposition.full);TEST_ASSERT_EQUAL_UINT64(3,s.disposition.nonfull);
    TEST_ASSERT_EQUAL_UINT64(0,s.disposition.begin);TEST_ASSERT_EQUAL_UINT64(17,s.disposition.end);
    TEST_ASSERT_EQUAL_UINT64(3,s.disposition.buckets[JCE_ENGINE_DISPOSITION_HEADLESS].ticks);
    TEST_ASSERT_EQUAL_UINT64(5,s.disposition.buckets[JCE_ENGINE_DISPOSITION_DRAWABLE+3u].ticks);
    TEST_ASSERT_EQUAL_UINT64(5,s.disposition.buckets[JCE_ENGINE_DISPOSITION_FULL].ticks);
    const JceEnginePerfDispositionBucket *guard=&s.disposition.buckets[JCE_ENGINE_DISPOSITION_GUARD];
    TEST_ASSERT_EQUAL_UINT64(1,guard->count);TEST_ASSERT_EQUAL_UINT64(4,guard->ticks);
    TEST_ASSERT_EQUAL_UINT64(3,guard->first.interval);TEST_ASSERT_EQUAL_UINT64(1,guard->first.row);
    TEST_ASSERT_EQUAL_UINT64(13,guard->first.begin);TEST_ASSERT_EQUAL_UINT64(17,guard->last.end);
    before=s.disposition;
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&s,9,0));
    TEST_ASSERT_FALSE(jce_engine_perf_disposition_boundary(&s,1000,20,2));
    jce_engine_perf_disposition_reason(&s,JCE_ENGINE_DISPOSITION_HEADLESS);
    jce_engine_perf_disposition_release(&s,true);
    TEST_ASSERT_EQUAL_MEMORY(&before,&s.disposition,sizeof before);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_report(&s));
    TEST_ASSERT_FALSE(jce_engine_perf_disposition_report(&s));
    jce_engine_perf_hitch_end_interval(&s,false);
}
static void test_disposition_preboundary_refusal_cannot_relabel_prior_interval(void)
{
    JceEnginePerfHitch s;
    jce_engine_perf_hitch_init(&s,false,4.0);
    jce_engine_perf_disposition_init(&s,true,1000);
    disposition_entry(&s,7,3);
    disposition_exit(&s,JCE_ENGINE_DISPOSITION_HEADLESS,false);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    jce_engine_perf_disposition_reason(&s,JCE_ENGINE_DISPOSITION_DEVICE_FAILED);
    jce_engine_perf_disposition_release(&s,false);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&s,1000,11));
    TEST_ASSERT_EQUAL_UINT64(1,s.disposition.buckets[JCE_ENGINE_DISPOSITION_HEADLESS].count);
    TEST_ASSERT_EQUAL_UINT64(0,s.disposition.buckets[JCE_ENGINE_DISPOSITION_DEVICE_FAILED].count);
    TEST_ASSERT_EQUAL_UINT64(4,s.disposition.buckets[JCE_ENGINE_DISPOSITION_HEADLESS].ticks);
    jce_engine_perf_hitch_end_interval(&s,false);
}
static void test_disposition_zero_duration_and_full_terminal_rotation_do_not_double_count(void)
{
    JceEnginePerfHitch s;
    jce_engine_perf_hitch_init(&s,false,4.0);
    jce_engine_perf_disposition_init(&s,true,1000);
    disposition_entry(&s,0,0);
    jce_engine_perf_disposition_release(&s,true);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&s,1000,0));
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&s));
    jce_engine_perf_hitch_end_interval(&s,true);
    TEST_ASSERT_TRUE(jce_engine_perf_disposition_finish(&s,1000,9000));
    TEST_ASSERT_EQUAL_UINT64(1,s.disposition.full);TEST_ASSERT_EQUAL_UINT64(1,s.disposition.intervals);
    TEST_ASSERT_EQUAL_UINT64(0,s.disposition.buckets[JCE_ENGINE_DISPOSITION_FULL].ticks);
    TEST_ASSERT_FALSE(s.previous_full);
}
static void test_disposition_invalid_boundary_frequency_release_and_identity_fail_closed(void)
{
    for(unsigned which=0;which<7;++which) {
        JceEnginePerfHitch s;
        jce_engine_perf_hitch_init(&s,false,4.0);
        jce_engine_perf_disposition_init(&s,true,1000);
        disposition_entry(&s,8,3);
        if(which!=0)jce_engine_perf_disposition_release(&s,false);
        uint32_t expected=JCE_ENGINE_DISPOSITION_UNFINISHED;
        if(which==1)expected=JCE_ENGINE_DISPOSITION_FREQUENCY;
        if(which==2)expected=JCE_ENGINE_DISPOSITION_COUNTER;
        if(which==3) {
            expected=JCE_ENGINE_DISPOSITION_SEQUENCE;
            TEST_ASSERT_FALSE(jce_engine_perf_disposition_boundary(&s,1000,9,3));
        }
        if(which==4) {
            expected=JCE_ENGINE_DISPOSITION_SEQUENCE;
            jce_engine_perf_hitch_end_interval(&s,false);
            TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
            TEST_ASSERT_FALSE(jce_engine_perf_disposition_boundary(&s,1000,9,2));
        }
        if(which==5) {
            expected=JCE_ENGINE_DISPOSITION_SEQUENCE;
            jce_engine_perf_disposition_release(&s,false);
        }
        if(which==6) {
            expected=JCE_ENGINE_DISPOSITION_SEQUENCE;
            s.disposition.kind=JCE_ENGINE_DISPOSITION_COUNT;
        }
        TEST_ASSERT_FALSE(jce_engine_perf_disposition_finish(&s,which==1?999u:1000u,which==2?7u:10u));
        TEST_ASSERT_TRUE(s.disposition.errors&expected);
        TEST_ASSERT_EQUAL_UINT64(0,s.disposition.intervals);
    }
}
static void test_disposition_overflow_and_disabled_capture_never_wrap_or_publish(void)
{
    for(unsigned which=0;which<4;++which) {
        JceEnginePerfHitch s;
        jce_engine_perf_hitch_init(&s,false,4.0);
        jce_engine_perf_disposition_init(&s,true,1000);
        disposition_entry(&s,UINT64_MAX-2u,0);
        jce_engine_perf_disposition_reason(&s,JCE_ENGINE_DISPOSITION_HEADLESS);
        jce_engine_perf_disposition_release(&s,false);
        JceEnginePerfDispositionBucket *b=&s.disposition.buckets[JCE_ENGINE_DISPOSITION_HEADLESS];
        if(which==0)b->ticks=UINT64_MAX;
        if(which==1)b->count=UINT64_MAX;
        if(which==2)s.disposition.intervals=UINT64_MAX;
        if(which==3)s.disposition.nonfull=UINT64_MAX;
        uint64_t count=b->count,ticks=b->ticks,intervals=s.disposition.intervals;
        TEST_ASSERT_FALSE(jce_engine_perf_disposition_finish(&s,1000,UINT64_MAX));
        TEST_ASSERT_TRUE(s.disposition.errors&JCE_ENGINE_DISPOSITION_OVERFLOW);
        TEST_ASSERT_EQUAL_UINT64(count,b->count);TEST_ASSERT_EQUAL_UINT64(ticks,b->ticks);
        TEST_ASSERT_EQUAL_UINT64(intervals,s.disposition.intervals);
    }
    JceEnginePerfHitch s;
    jce_engine_perf_hitch_init(&s,false,4.0);
    jce_engine_perf_disposition_init(&s,false,0);
    JceEnginePerfDisposition before=s.disposition;
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&s));
    TEST_ASSERT_FALSE(jce_engine_perf_disposition_boundary(&s,0,0,0));
    jce_engine_perf_disposition_reason(&s,JCE_ENGINE_DISPOSITION_GUARD);
    jce_engine_perf_disposition_release(&s,false);
    TEST_ASSERT_FALSE(jce_engine_perf_disposition_finish(&s,0,0));
    TEST_ASSERT_FALSE(jce_engine_perf_disposition_report(&s));
    TEST_ASSERT_EQUAL_MEMORY(&before,&s.disposition,sizeof before);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_initial_unavailable_is_not_zero);
    RUN_TEST(test_sampled_all_zero_is_available);
    RUN_TEST(test_A_wall_captures_A_then_B_cannot_change_log);
    RUN_TEST(test_skipped_interval_does_not_reuse_everavailable_snapshot);
    RUN_TEST(test_ordinal_advances_without_autostop_and_duplicate_observe_is_rejected);
    RUN_TEST(test_nested_entry_cannot_change_outer_state);
    RUN_TEST(test_late_slot_and_top_eight_use_full_public_capacity);
    RUN_TEST(test_names_are_ascii_escaped_and_long_entry_is_explicitly_truncated);
    RUN_TEST(test_invalid_input_and_small_output_fail_without_history_damage);
    RUN_TEST(test_zero_wall_advances_history_without_forging_a_hitch);
    RUN_TEST(test_public_phase_disable_reenable_does_not_reuse_old_snapshot);
    RUN_TEST(test_exact_phase_text_boundaries_preserve_padding_and_guard);
    RUN_TEST(test_unterminated_private_payload_fails_bounded_format);
    RUN_TEST(test_invalid_snapshot_does_not_forge_duration);
    RUN_TEST(test_window_policy_and_lifecycle_reset);
    RUN_TEST(test_phase_reads_and_window_report_do_not_consume_snapshot);
    RUN_TEST(test_initial_entry_and_skips_have_no_completed_render);
    RUN_TEST(test_terminal_completion_rotates_current_snapshot_once_under_gate);
    RUN_TEST(test_terminal_disabled_phases_are_completed_but_not_attributed);
    RUN_TEST(test_terminal_completion_without_entry_changes_nothing);
    RUN_TEST(test_actual_owner_normal_wait_keeps_raw_wall_and_selected_rate);
    RUN_TEST(test_actual_owner_oversleep_remains_a_hitch);
    RUN_TEST(test_actual_owner_work_stall_and_overrun_are_not_exempted);
    RUN_TEST(test_actual_owner_uncapped_capped_uncapped_does_not_mix_wall_baseline);
    RUN_TEST(test_skip_and_unwaited_terminal_cannot_reuse_previous_wait);
    RUN_TEST(test_invalid_pacing_never_rejects_raw_wall_or_forges_exemption);
    RUN_TEST(test_zero_comparison_baseline_formats_without_division);
    RUN_TEST(test_disposition_raw_boundaries_partition_reasons_and_terminal_once);
    RUN_TEST(test_disposition_preboundary_refusal_cannot_relabel_prior_interval);
    RUN_TEST(test_disposition_zero_duration_and_full_terminal_rotation_do_not_double_count);
    RUN_TEST(test_disposition_invalid_boundary_frequency_release_and_identity_fail_closed);
    RUN_TEST(test_disposition_overflow_and_disabled_capture_never_wrap_or_publish);
    return UNITY_END();
}
