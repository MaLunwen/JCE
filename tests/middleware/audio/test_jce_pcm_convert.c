/*
 * test_jce_pcm_convert.c  PCM sizing primitive (L4 audio).
 *
 * Regression guard for the 8-bit cooked-audio heap overflow (audit
 * 2026-06-13, R2F8): jce_audio_load_pcm sized the s16 output buffer for
 * whole frames (frame_count*channels) but converted pcm_size samples, so a
 * malformed clip with pcm_size % channels != 0 overflowed the heap.  The
 * sizing primitive must report the whole-frame sample count (never pcm_size),
 * and be safe when channels == 0 (the F52 divide-by-zero).
 */

#include "unity.h"

#include "middleware/audio/jce_pcm_convert.h"

#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_whole_frames_only(void)
{
    /* 3 bytes @ 2ch = 1 whole frame = 2 samples (NOT 3 -> the overflow). */
    TEST_ASSERT_EQUAL_size_t(2, jce_pcm_u8_to_s16_samples(3, 2));
    TEST_ASSERT_EQUAL_size_t(4, jce_pcm_u8_to_s16_samples(4, 2));
    TEST_ASSERT_EQUAL_size_t(5, jce_pcm_u8_to_s16_samples(5, 1));
    TEST_ASSERT_EQUAL_size_t(6, jce_pcm_u8_to_s16_samples(7, 2)); /* 3 frames */
}

static void test_zero_channels_safe(void)
{
    /* channels == 0 must not divide by zero — report nothing to load. */
    TEST_ASSERT_EQUAL_size_t(0, jce_pcm_u8_to_s16_samples(8, 0));
}

static void test_empty_input(void)
{
    TEST_ASSERT_EQUAL_size_t(0, jce_pcm_u8_to_s16_samples(0, 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_whole_frames_only);
    RUN_TEST(test_zero_channels_safe);
    RUN_TEST(test_empty_input);
    return UNITY_END();
}
