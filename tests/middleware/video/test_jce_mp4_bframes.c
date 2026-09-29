/* Synthetic B-frame regression: output PTS, complete drain and seek fencing. */
#include "unity.h"
#include <jce/api_video.h>
#include <jce/api_core.h>
#include <math.h>

static JceVideo video;
void setUp(void) { video = JCE_VIDEO_INVALID; }
void tearDown(void) { jce_video_unload(video); }

static void verify_clip(const char *path)
{
    JceVideoInfo info = {0};
    JceVideoPerfStats stats = {0};
    double pts = -1.0;
    uint64_t start = 0u;
    video = jce_video_load_file(path);
    TEST_ASSERT_NOT_EQUAL(JCE_VIDEO_INVALID, video);
    TEST_ASSERT_TRUE(jce_video_get_info(video, &info));
#if defined(JCE_ENABLE_PATENTED_CODECS) && JCE_ENABLE_PATENTED_CODECS
    TEST_ASSERT_FALSE(info.metadata_only);
    start = jce_time_ticks_ms();
    while (!jce_video_is_ready_to_play(video) && jce_time_ticks_ms() - start < 10000u) {
        jce_video_advance(video, 0.0);
        jce_thread_sleep_ms(1u);
    }
    TEST_ASSERT_TRUE(jce_video_is_ready_to_play(video));
    TEST_ASSERT_NOT_NULL(jce_video_get_frame_rgba(video, NULL, NULL, &pts));
    TEST_ASSERT_DOUBLE_WITHIN(.000001, 0.0, pts);
    start = jce_time_ticks_ms();
    while (!jce_video_has_ended(video) && jce_time_ticks_ms() - start < 10000u) {
        /* Wait for decode, rather than turning CPU contention into late drops. */
        jce_video_get_perf_stats(video, &stats);
        if (stats.q_count || stats.worker_eof) jce_video_advance(video, 1.0 / 30.0);
        jce_thread_sleep_ms(1u);
    }
    TEST_ASSERT_TRUE(jce_video_has_ended(video));
    TEST_ASSERT_TRUE(jce_video_get_perf_stats(video, &stats));
    TEST_ASSERT_EQUAL_UINT64(90u, stats.frames_decoded);
    TEST_ASSERT_EQUAL_UINT64(90u, stats.frames_displayed + stats.frames_dropped);
    jce_video_get_frame_rgba(video, NULL, NULL, &pts);
    TEST_ASSERT_DOUBLE_WITHIN(.000001, 89.0 / 30.0, pts);

    jce_video_seek(video, 1.6, true);
    start = jce_time_ticks_ms();
    while (!jce_video_is_ready_to_play(video) && jce_time_ticks_ms() - start < 10000u) {
        jce_video_advance(video, .016);
        TEST_ASSERT_DOUBLE_WITHIN(.000001, 1.6, jce_video_get_time(video));
        jce_thread_sleep_ms(1u);
    }
    TEST_ASSERT_TRUE(jce_video_is_ready_to_play(video));
    jce_video_get_frame_rgba(video, NULL, NULL, &pts);
    TEST_ASSERT_DOUBLE_WITHIN(.000001, 1.6, pts);
    jce_video_rewind(video);
    start = jce_time_ticks_ms();
    while (!jce_video_is_ready_to_play(video) && jce_time_ticks_ms() - start < 10000u) {
        jce_video_advance(video, 0.0);
        jce_thread_sleep_ms(1u);
    }
    TEST_ASSERT_TRUE(jce_video_is_ready_to_play(video));
    jce_video_get_frame_rgba(video, NULL, NULL, &pts);
    TEST_ASSERT_DOUBLE_WITHIN(.000001, 0.0, pts);
#else
    (void)pts; (void)start; (void)stats;
    TEST_ASSERT_TRUE(info.metadata_only);
#endif
}

static void test_progressive(void) { verify_clip(JCE_TEST_BFRAME_DIR "/bframes.mp4"); }
static void test_fragmented(void) { verify_clip(JCE_TEST_BFRAME_DIR "/bframes-fragmented.mp4"); }
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_progressive);
    RUN_TEST(test_fragmented);
    return UNITY_END();
}
