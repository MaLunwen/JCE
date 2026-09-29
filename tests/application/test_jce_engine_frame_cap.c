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
 * WHAT IS TESTED IS THE DECISION, NOT THE SLEEP.  Asserting that a capped loop
 * takes 16.6 ms per frame measures this machine's scheduler, and would be a
 * flaky test of an OS.  The half that can actually be WRONG is where the next
 * deadline goes -- and getting that wrong is not subtle: repaying a hitch by
 * advancing the deadline one budget at a time gives a burst of zero-length
 * frames, which the player sees as a speed-up.  So the policy is a pure
 * function and this drives it with a fake clock.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_engine_frame_cap.h"

#include "unity.h"

#include <jce/application/jce_engine.h>

#include <stddef.h>
#include <stdint.h>

/* Linking jce_engine.c drags the embedded-PAK symbols a shipped exe
 * supplies; an empty pak is what the other engine-level tests provide
 * (test_jce_headless_boot.c, test_jce_engine_input_backend.c). */
const unsigned char assets_pak_data[1] = { 0 };
const size_t        assets_pak_data_size = 0;

/* 1 MHz fake clock, 60 fps: a budget of 16666 ticks.  Round numbers would hide
 * a truncation bug in the caller's freq/fps. */
#define TICK_HZ  1000000u
#define BUDGET   (TICK_HZ / 60u)

void setUp(void)    {}
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_capped_frame_does_not_stall);
    RUN_TEST(test_a_frame_that_finished_early_waits_to_its_deadline);
    RUN_TEST(test_the_cap_does_not_drift_over_many_frames);
    RUN_TEST(test_a_blown_budget_reanchors_instead_of_catching_up);
    RUN_TEST(test_a_slightly_late_frame_keeps_the_grid);
    RUN_TEST(test_a_zero_budget_never_waits);
    RUN_TEST(test_the_public_setter_round_trips_and_clamps);
    return UNITY_END();
}
