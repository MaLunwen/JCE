/*
 * test_jce_image_gray16.c — regression guard for the 16-bit grayscale PNG
 * heap overrun (dependency audit P1 img-runtime-gray16-heap-overrun).
 *
 * A PNG with IHDR bit-depth 16 / colour-type 0 (greyscale) heap-overruns the
 * bundled SDL3_image libpng path — STATUS_HEAP_CORRUPTION, originally hit
 * while cooking a DCC-exported height map. The cooker bypassed it, but the
 * RUNTIME decode did not, so the same asset that cooked fine corrupted the
 * heap when a game loaded it raw from a PAK. jce_image is now the single
 * decode dispatcher and sniffs this class, routing it to stb_image instead.
 *
 * This test decodes such a PNG through the public entry point. Before the fix
 * it corrupts the heap (crash / ASAN report); after it, it must return
 * correctly-sized RGBA8 with the greyscale replicated across R, G and B.
 */

#include "unity.h"

#include <jce/renderer/jce_image.h>

#include <stdint.h>

/* 4x2, bit depth 16, colour type 0 (greyscale) — generated, valid PNG. */
static const unsigned char k_gray16_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x53, 0xFE, 0xFC, 0x00, 0x00, 0x00,
    0x1A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x60, 0x90, 0x77,
    0xB0, 0x6B, 0x88, 0x3D, 0xC0, 0xE0, 0xA7, 0x90, 0x9B, 0xD0, 0xB3, 0x60,
    0xF5, 0x03, 0x00, 0x28, 0x0C, 0x06, 0x2D, 0x9B, 0x15, 0x46, 0xF8, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

void setUp(void) {}
void tearDown(void) {}

void test_gray16_png_decodes_without_heap_overrun(void)
{
    int w = 0, h = 0;
    uint8_t *rgba = jce_image_load_rgba8_from_memory(
        k_gray16_png, (uint64_t)sizeof(k_gray16_png), &w, &h);

    TEST_ASSERT_NOT_NULL_MESSAGE(rgba, "16-bit gray PNG failed to decode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, w, "wrong width");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, h, "wrong height");

    /* Greyscale expands to RGBA8 with R==G==B and an opaque alpha. */
    for (int i = 0; i < w * h; ++i) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(p[0], p[1], "grey not replicated to G");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(p[0], p[2], "grey not replicated to B");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, p[3], "alpha not opaque");
    }

    jce_image_free_rgba8(rgba);
}

/* The 16-bit gray sniff must not steal ordinary 8-bit PNGs from SDL_image;
 * a truncated/!PNG buffer must fail cleanly rather than crash. */
void test_non_png_input_fails_cleanly(void)
{
    const unsigned char junk[32] = { 0 };
    int w = -1, h = -1;
    uint8_t *rgba = jce_image_load_rgba8_from_memory(junk, sizeof(junk), &w, &h);
    TEST_ASSERT_NULL_MESSAGE(rgba, "garbage input decoded as an image");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gray16_png_decodes_without_heap_overrun);
    RUN_TEST(test_non_png_input_fails_cleanly);
    return UNITY_END();
}
