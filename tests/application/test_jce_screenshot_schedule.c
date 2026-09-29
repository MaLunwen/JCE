#include <jce/application/jce_screenshot.h>

#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_schedule_parses_ordered_absolute_paths(void)
{
    JceScreenshotSchedule schedule;
    char error[160];

    TEST_ASSERT_TRUE(jce_screenshot_schedule_parse(
        "35|/tmp/transfer.png;70|/tmp/capture.png;100|/tmp/result.png",
        &schedule, error, sizeof(error)));
    TEST_ASSERT_EQUAL_INT(3, schedule.count);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 35.0, schedule.entries[0].seconds);
    TEST_ASSERT_EQUAL_STRING("/tmp/capture.png", schedule.entries[1].path);
}

static void test_schedule_rejects_ambiguous_specs(void)
{
    static const char *invalid[] = {
        "35|relative.png",
        "35|",
        "0|/tmp/a.png",
        "nan|/tmp/a.png",
        "35|/tmp/a.png;35|/tmp/b.png",
        "70|/tmp/a.png;35|/tmp/b.png",
        "1|/1.png;2|/2.png;3|/3.png;4|/4.png;5|/5.png;"
        "6|/6.png;7|/7.png;8|/8.png;9|/9.png",
        "35/tmp/a.png",
    };
    size_t i;

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        JceScreenshotSchedule schedule;
        char error[160];
        TEST_ASSERT_FALSE(jce_screenshot_schedule_parse(
            invalid[i], &schedule, error, sizeof(error)));
        TEST_ASSERT_NOT_EQUAL(0, error[0]);
    }
}

/* The schedule clamps every PLAYING step to 100 ms (the deterministic-clock
 * stall clamp in jce_screenshot_schedule_tick) — advance the way the app
 * does, in frame-sized steps, so the intended total actually accrues. */
static void tick_playing(JceScreenshotSchedule *s, double total)
{
    while (total > 1e-9) {
        double step = total > 0.05 ? 0.05 : total;
        jce_screenshot_schedule_tick(s, JCE_SCREENSHOT_CLOCK_PLAYING, step);
        total -= step;
    }
}

static void test_schedule_clock_and_async_serialization(void)
{
    JceScreenshotSchedule schedule;
    const JceScreenshotScheduleEntry *due;
    char error[160];

    TEST_ASSERT_TRUE(jce_screenshot_schedule_parse(
        "1|/tmp/a.png;2|/tmp/b.png", &schedule, error, sizeof(error)));

    jce_screenshot_schedule_tick(
        &schedule, JCE_SCREENSHOT_CLOCK_STOPPED, 20.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, schedule.elapsed_seconds);
    tick_playing(&schedule, 0.75);
    TEST_ASSERT_NULL(jce_screenshot_schedule_due(&schedule));
    jce_screenshot_schedule_tick(
        &schedule, JCE_SCREENSHOT_CLOCK_PAUSED, 5.0);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.75, schedule.elapsed_seconds);
    tick_playing(&schedule, 0.25);

    due = jce_screenshot_schedule_due(&schedule);
    TEST_ASSERT_NOT_NULL(due);
    TEST_ASSERT_EQUAL_STRING("/tmp/a.png", due->path);
    TEST_ASSERT_TRUE(jce_screenshot_schedule_mark_submitted(&schedule));
    TEST_ASSERT_NULL(jce_screenshot_schedule_due(&schedule));

    tick_playing(&schedule, 2.0);
    jce_screenshot_schedule_mark_complete(&schedule, true);
    due = jce_screenshot_schedule_due(&schedule);
    TEST_ASSERT_NOT_NULL(due);
    TEST_ASSERT_EQUAL_STRING("/tmp/b.png", due->path);
    TEST_ASSERT_TRUE(jce_screenshot_schedule_mark_submitted(&schedule));
    jce_screenshot_schedule_mark_complete(&schedule, true);
    TEST_ASSERT_TRUE(jce_screenshot_schedule_finished(&schedule));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_schedule_parses_ordered_absolute_paths);
    RUN_TEST(test_schedule_rejects_ambiguous_specs);
    RUN_TEST(test_schedule_clock_and_async_serialization);
    return UNITY_END();
}
