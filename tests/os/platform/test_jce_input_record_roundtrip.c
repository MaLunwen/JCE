/* test_jce_input_record_roundtrip.c
 *
 * The .jirc recorder writes through the engine's own host filesystem
 * (jce_fs_host_*), not SDL_IOStream -- nothing in it is a device, and a
 * recorder that dragged SDL in just to open a file would have kept the whole
 * input subsystem chained to it after the seam split.
 *
 * That port replaced streaming writes with batched appends, so what needs
 * pinning is not "does it use the new API" but the two things the change could
 * plausibly break and nothing else covered:
 *
 *   - frames survive the batch boundary (JIRC_FLUSH_FRAMES is 64, and the
 *     obvious bug is a partial batch dropped at close);
 *   - a second record session TRUNCATES rather than appending behind the old
 *     header, which would silently produce a file whose frame count is right
 *     and whose contents are two sessions interleaved.
 *
 * State is injected through jce_input_apply, so no window, no event pump and
 * no hardware.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#define TMP_PATH "test_jce_input_record_roundtrip.tmp.jirc"

static JceInput *g_input = NULL;

void setUp(void)
{
    g_input = jce_input_create();
}

void tearDown(void)
{
    jce_input_destroy(g_input);
    g_input = NULL;
    /* remove_recursive, not remove(): two cases below deliberately turn
     * TMP_PATH into a DIRECTORY to make an append fail, and a case that aborts
     * on an assertion before its own cleanup would otherwise leave that
     * directory behind and poison every later run of this binary. */
    jce_fs_host_remove_recursive(TMP_PATH);
}

/* Push a frame whose mouse_x is `marker` so each recorded frame is
 * distinguishable from every other. */
static void inject(float marker)
{
    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version   = JCE_INPUT_FRAME_VERSION;
    f.key_count = (uint32_t)JCE_KEY_COUNT;
    f.mouse_x   = marker;
    TEST_ASSERT_TRUE(jce_input_apply(g_input, &f));
}

/* Record `count` frames marked 0..count-1 and close the recorder. */
static void record_n(int count)
{
    JceInputRecorder *r = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);
    for (int i = 0; i < count; ++i) {
        inject((float)i);
        TEST_ASSERT_TRUE(jce_input_record_tick(r, g_input));
    }
    TEST_ASSERT_EQUAL_UINT64((uint64_t)count, jce_input_record_frame_count(r));
    jce_input_record_close(r);
}

/* Replay the file and assert it holds exactly `count` frames marked 0..n-1. */
static void expect_n(int count)
{
    JceInputRecorder *r = jce_input_replay_open(TMP_PATH);
    float x = 0.0f, y = 0.0f;
    TEST_ASSERT_NOT_NULL(r);
    for (int i = 0; i < count; ++i) {
        TEST_ASSERT_TRUE(jce_input_replay_tick(r, g_input));
        jce_input_mouse_pos(g_input, &x, &y);
        TEST_ASSERT_EQUAL_FLOAT((float)i, x);
    }
    /* EOF, not a partial frame. */
    TEST_ASSERT_FALSE(jce_input_replay_tick(r, g_input));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)count, jce_input_record_frame_count(r));
    jce_input_record_close(r);
}

static void test_three_frames_round_trip_in_order(void)
{
    record_n(3);
    expect_n(3);
}

static void test_frames_survive_the_flush_boundary(void)
{
    /* 130 crosses the 64-frame batch twice and leaves a partial third, which
     * only reaches disk if close() flushes what tick() has not. */
    record_n(130);
    expect_n(130);
}

static void test_a_session_with_no_frames_leaves_a_valid_empty_file(void)
{
    JceInputRecorder *r = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);
    jce_input_record_close(r);
    expect_n(0);
}

static void test_recording_again_truncates_the_previous_session(void)
{
    record_n(70);              /* more than one batch */
    record_n(2);
    expect_n(2);               /* not 72, and not 2 frames of stale tail */
}

static void test_replay_refuses_a_file_that_is_not_a_jirc(void)
{
    FILE *f = fopen(TMP_PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("NOT-A-JIRC-HEADER-AT-ALL", 1, 24, f);
    fclose(f);
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_replay_refuses_a_missing_file(void)
{
    remove(TMP_PATH);
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_recorder_refuses_the_other_mode(void)
{
    JceInputRecorder *r = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(jce_input_replay_tick(r, g_input));
    jce_input_record_close(r);

    r = jce_input_replay_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(jce_input_record_tick(r, g_input));
    jce_input_record_close(r);
}

/* Make the destination un-appendable without touching the recorder: swap the
 * file for a DIRECTORY of the same name.  SDL_IOFromFile(dir, "ab") fails, so
 * the next flush fails, with no fault injection hook and no mock filesystem. */
static void make_path_unwritable(void)
{
    TEST_ASSERT_EQUAL_INT(0, remove(TMP_PATH));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(TMP_PATH));
}

static void test_a_failed_flush_stops_recording_rather_than_holing_the_file(void)
{
    /* The regression this exists to prevent: clearing the pending batch on a
     * failed append drops up to 64 frames, marks nothing, and keeps
     * recording -- and replay reads the result back as a shorter CONTINUOUS
     * session.  A short file is honest; a file with a hole is not. */
    JceInputRecorder *r = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);

    for (int i = 0; i < 63; ++i) {          /* below the batch, nothing on disk */
        inject((float)i);
        TEST_ASSERT_TRUE(jce_input_record_tick(r, g_input));
    }
    TEST_ASSERT_EQUAL_UINT64(63u, jce_input_record_frame_count(r));

    make_path_unwritable();

    inject(63.0f);                          /* the 64th triggers the flush */
    TEST_ASSERT_FALSE(jce_input_record_tick(r, g_input));

    /* Not 64, and not 63: none of them reached disk, and the recorder does not
     * get to report a session longer than the file it produced. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_record_frame_count(r));

    /* Sticky.  It does not resume even though nothing stops it trying. */
    inject(64.0f);
    TEST_ASSERT_FALSE(jce_input_record_tick(r, g_input));
    inject(65.0f);
    TEST_ASSERT_FALSE(jce_input_record_tick(r, g_input));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_record_frame_count(r));

    jce_input_record_close(r);
    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(TMP_PATH));
}

static void test_what_reached_disk_replays_as_a_clean_prefix(void)
{
    /* The other half of the same promise: a failed FINAL flush must leave the
     * frames that did get written readable, in order, ending in EOF -- a
     * prefix, not a truncation in the middle of a frame and not a gap. */
    JceInputRecorder *r = jce_input_record_open(TMP_PATH);
    uint64_t on_disk = 0;
    void *bytes;

    TEST_ASSERT_NOT_NULL(r);
    for (int i = 0; i < 100; ++i) {         /* 64 flushed, 36 still pending */
        inject((float)i);
        TEST_ASSERT_TRUE(jce_input_record_tick(r, g_input));
    }

    /* Snapshot the file as it stands one instant before the path breaks. */
    bytes = jce_fs_host_read_all(TMP_PATH, &on_disk);
    TEST_ASSERT_NOT_NULL(bytes);

    make_path_unwritable();
    jce_input_record_close(r);              /* final flush fails; logs, no crash */
    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(TMP_PATH));

    TEST_ASSERT_TRUE(jce_fs_host_write_all(TMP_PATH, bytes, on_disk));
    jce_fs_buffer_free(bytes);
    expect_n(64);                           /* frames 0..63, then EOF */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_three_frames_round_trip_in_order);
    RUN_TEST(test_frames_survive_the_flush_boundary);
    RUN_TEST(test_a_session_with_no_frames_leaves_a_valid_empty_file);
    RUN_TEST(test_recording_again_truncates_the_previous_session);
    RUN_TEST(test_replay_refuses_a_file_that_is_not_a_jirc);
    RUN_TEST(test_replay_refuses_a_missing_file);
    RUN_TEST(test_a_recorder_refuses_the_other_mode);
    RUN_TEST(test_a_failed_flush_stops_recording_rather_than_holing_the_file);
    RUN_TEST(test_what_reached_disk_replays_as_a_clean_prefix);
    return UNITY_END();
}
