/* test_jce_terrain_heightmap_io.c
 *
 * Unit tests for terrain heightmap image import / export
 * (jce_terrain_import_heightmap_r16 / jce_terrain_export_heightmap_r16):
 *   - a synthesized 16-bit gradient imports so that jce_terrain_sample_height
 *     at known vertex centers == value/65535 * max_height (the sample oracle)
 *   - export round-trips the grid back to a 16-bit buffer within +/-1 LSB
 *   - a smaller source resamples (bilinear) into a larger grid without crashing
 *     and reproduces the corner samples exactly
 *   - bounds / NULL guards behave (no import, no overflow)
 *
 * Exercises the REAL terrain grid + bilinear sample path; no editor/GPU.
 */

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_filesystem.h>

#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* World position of grid vertex (i,j) for a terrain of size W x H. */
static float vx(int i, int W, float world_size_x)
{
    return (float)i / (float)(W - 1) * world_size_x;
}
static float vz(int j, int H, float world_size_z)
{
    return (float)j / (float)(H - 1) * world_size_z;
}

/* A WxW 16-bit gradient: value increases with (i + j). */
static uint16_t gradient_value(int i, int j, int W)
{
    /* Spread 0..65535 across the diagonal so neighbours differ noticeably. */
    int   max_step = 2 * (W - 1);
    float t        = (float)(i + j) / (float)(max_step);
    long  v        = (long)(t * 65535.0f + 0.5f);
    if (v < 0) v = 0; if (v > 65535) v = 65535;
    return (uint16_t)v;
}

static void test_import_r16_samples_match_oracle(void)
{
    const int   W = 33, H = 33;           /* 32 cells, integer chunk tiling */
    const float WSX = 64.0f, WSZ = 64.0f;
    const float MAXH = 20.0f;
    JceTerrain *t = jce_terrain_create(W, H, WSX, WSZ, MAXH, 32);
    TEST_ASSERT_NOT_NULL(t);

    uint16_t *src = (uint16_t *)malloc((size_t)W * H * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(src);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i)
            src[(size_t)j * W + i] = gradient_value(i, j, W);

    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_r16(t, src, W, H));

    /* Sample at a spread of exact vertex centers; bilinear at a vertex returns
     * that exact grid value, so expected = value/65535 * max_height. */
    const int probes_i[] = { 0, 8, 16, 24, 32 };
    const int probes_j[] = { 0, 4, 16, 28, 32 };
    for (int a = 0; a < 5; ++a) {
        for (int b = 0; b < 5; ++b) {
            int   i  = probes_i[a], j = probes_j[b];
            float wx = vx(i, W, WSX), wz = vz(j, H, WSZ);
            float got = jce_terrain_sample_height(t, wx, wz);
            float want = (float)gradient_value(i, j, W) / 65535.0f * MAXH;
            /* float32 quantization of /65535 -> tight epsilon. */
            TEST_ASSERT_FLOAT_WITHIN(2.0e-3f, want, got);
        }
    }

    free(src);
    jce_terrain_free(t);
}

static void test_export_round_trips(void)
{
    const int W = 17, H = 17;
    JceTerrain *t = jce_terrain_create(W, H, 32.0f, 32.0f, 50.0f, 16);
    TEST_ASSERT_NOT_NULL(t);

    size_t    n   = (size_t)W * H;
    uint16_t *src = (uint16_t *)malloc(n * sizeof(uint16_t));
    uint16_t *out = (uint16_t *)malloc(n * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(out);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i)
            src[(size_t)j * W + i] = gradient_value(i, j, W);

    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_r16(t, src, W, H));

    /* Too-small capacity is rejected. */
    TEST_ASSERT_FALSE(jce_terrain_export_heightmap_r16(t, out, n - 1));

    TEST_ASSERT_TRUE(jce_terrain_export_heightmap_r16(t, out, n));
    for (size_t i = 0; i < n; ++i) {
        int diff = (int)out[i] - (int)src[i];
        if (diff < 0) diff = -diff;
        /* import normalizes to float32 then export re-quantizes:
         * round-trip is stable to within 1 LSB. */
        TEST_ASSERT_TRUE(diff <= 1);
    }

    free(src);
    free(out);
    jce_terrain_free(t);
}

static void test_import_resamples_smaller_source(void)
{
    /* 3x3 source bilinearly upsampled into a 9x9 grid.  The four corners are
     * exact source samples, so they must reproduce exactly; interior samples
     * must stay within the source value range (monotone gradient). */
    const int   SW = 3, SH = 3;
    const int   W = 9, H = 9;
    const float MAXH = 10.0f;
    JceTerrain *t = jce_terrain_create(W, H, 16.0f, 16.0f, MAXH, 8);
    TEST_ASSERT_NOT_NULL(t);

    uint16_t src[9];
    /* Linear ramp 0 .. 65535 across both axes (corner (0,0)=0, (2,2)=max). */
    for (int j = 0; j < SH; ++j)
        for (int i = 0; i < SW; ++i)
            src[j * SW + i] = (uint16_t)(((i + j) * 65535) / (2 * (SW - 1)));

    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_r16(t, src, SW, SH));

    /* Corner (0,0) -> 0 height; corner (W-1,H-1) -> max source value. */
    float c00 = jce_terrain_sample_height(t, vx(0, W, 16.0f), vz(0, H, 16.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 0.0f, c00);

    float cMM = jce_terrain_sample_height(t, vx(W - 1, W, 16.0f),
                                          vz(H - 1, H, 16.0f));
    float wantMM = (float)src[SW * SH - 1] / 65535.0f * MAXH;
    TEST_ASSERT_FLOAT_WITHIN(2.0e-3f, wantMM, cMM);

    /* Center vertex of the grid maps to source center (1,1) of the ramp. */
    float cmid = jce_terrain_sample_height(t, vx(W / 2, W, 16.0f),
                                           vz(H / 2, H, 16.0f));
    float wantmid = (float)src[1 * SW + 1] / 65535.0f * MAXH;
    TEST_ASSERT_FLOAT_WITHIN(2.0e-3f, wantmid, cmid);

    jce_terrain_free(t);
}

/* Build a unique temp host path under TEMP (or cwd) with the given leaf. */
static void temp_path(char *out, size_t cap, const char *leaf)
{
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(out, cap, "%s/%s", tmp, leaf);
}

/* jce_terrain_import_heightmap_file: a headerless 16-bit RAW matching the
 * terrain grid is byte-assembled little-endian and imported, so a sample at a
 * vertex center reproduces the encoded value (this exercises the file read +
 * RAW decode + jce_fs_buffer_free branch the in-memory r16 test never hits). */
static void test_import_file_raw16_matches_oracle(void)
{
    const int   W = 5, H = 5;
    const float WSX = 8.0f, WSZ = 8.0f, MAXH = 16.0f;
    JceTerrain *t = jce_terrain_create(W, H, WSX, WSZ, MAXH, 4);
    TEST_ASSERT_NOT_NULL(t);

    /* W*H 16-bit little-endian raw: a per-cell ramp so neighbours differ. */
    const size_t   grid = (size_t)W * (size_t)H;
    uint16_t      *vals = (uint16_t *)malloc(grid * sizeof(uint16_t));
    uint8_t       *raw  = (uint8_t  *)malloc(grid * 2u);
    TEST_ASSERT_NOT_NULL(vals);
    TEST_ASSERT_NOT_NULL(raw);
    for (size_t i = 0; i < grid; ++i) {
        uint16_t v = (uint16_t)((i * 65535u) / (grid - 1));   /* 0..65535 */
        vals[i]      = v;
        raw[i * 2u]      = (uint8_t)(v & 0xFFu);              /* little-endian */
        raw[i * 2u + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    }

    char path[512];
    temp_path(path, sizeof path, "jce_test_hm_raw16.r16");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, raw, (uint64_t)(grid * 2u)));

    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_file(t, path));

    /* Every vertex sample must equal its encoded value (no resample: src==grid). */
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            float wx   = vx(i, W, WSX), wz = vz(j, H, WSZ);
            float got  = jce_terrain_sample_height(t, wx, wz);
            float want = (float)vals[(size_t)j * W + i] / 65535.0f * MAXH;
            TEST_ASSERT_FLOAT_WITHIN(2.0e-3f, want, got);
        }
    }

    remove(path);
    free(vals);
    free(raw);
    jce_terrain_free(t);
}

/* An 8-bit RAW matching the grid imports via the /255 branch. */
static void test_import_file_raw8_matches_oracle(void)
{
    const int   W = 4, H = 4;
    const float WSX = 6.0f, WSZ = 6.0f, MAXH = 10.0f;
    JceTerrain *t = jce_terrain_create(W, H, WSX, WSZ, MAXH, 4);
    TEST_ASSERT_NOT_NULL(t);

    const size_t grid = (size_t)W * (size_t)H;
    uint8_t     *raw  = (uint8_t *)malloc(grid);
    TEST_ASSERT_NOT_NULL(raw);
    for (size_t i = 0; i < grid; ++i)
        raw[i] = (uint8_t)((i * 255u) / (grid - 1));          /* 0..255 ramp */

    char path[512];
    temp_path(path, sizeof path, "jce_test_hm_raw8.r8");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, raw, (uint64_t)grid));

    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_file(t, path));

    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            float wx   = vx(i, W, WSX), wz = vz(j, H, WSZ);
            float got  = jce_terrain_sample_height(t, wx, wz);
            float want = (float)raw[(size_t)j * W + i] / 255.0f * MAXH;
            TEST_ASSERT_FLOAT_WITHIN(2.0e-3f, want, got);
        }
    }

    remove(path);
    free(raw);
    jce_terrain_free(t);
}

/* A RAW whose byte count matches neither grid nor grid*2 (and is not a valid
 * image) is rejected: import returns false and the heightmap stays zeroed. */
static void test_import_file_wrong_size_rejected(void)
{
    const int W = 5, H = 5;
    JceTerrain *t = jce_terrain_create(W, H, 8.0f, 8.0f, 16.0f, 4);
    TEST_ASSERT_NOT_NULL(t);

    /* grid = 25; choose a byte count that is neither 25 nor 50, and not a
     * decodable image header. */
    uint8_t junk[37];
    memset(junk, 0xA5, sizeof junk);

    char path[512];
    temp_path(path, sizeof path, "jce_test_hm_badsize.bin");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, junk, (uint64_t)sizeof junk));

    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_file(t, path));

    /* Rejected import leaves the grid at its created (zero) state. */
    float h = jce_terrain_sample_height(t, 4.0f, 4.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 0.0f, h);

    remove(path);
    jce_terrain_free(t);
}

/* A missing file path is rejected without crashing. */
static void test_import_file_missing_rejected(void)
{
    JceTerrain *t = jce_terrain_create(5, 5, 8.0f, 8.0f, 16.0f, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_file(
        t, "jce_test_hm_does_not_exist_zzz.r16"));
    jce_terrain_free(t);
}

static void test_guards(void)
{
    JceTerrain *t = jce_terrain_create(5, 5, 10.0f, 10.0f, 5.0f, 4);
    TEST_ASSERT_NOT_NULL(t);
    uint16_t buf[25];
    memset(buf, 0, sizeof buf);

    /* NULL / degenerate arguments are rejected, never crash. */
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_r16(NULL, buf, 5, 5));
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_r16(t, NULL, 5, 5));
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_r16(t, buf, 0, 5));
    TEST_ASSERT_FALSE(jce_terrain_export_heightmap_r16(NULL, buf, 25));
    TEST_ASSERT_FALSE(jce_terrain_export_heightmap_r16(t, NULL, 25));
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_file(t, NULL));
    TEST_ASSERT_FALSE(jce_terrain_import_heightmap_file(t, ""));

    jce_terrain_free(t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_import_r16_samples_match_oracle);
    RUN_TEST(test_export_round_trips);
    RUN_TEST(test_import_resamples_smaller_source);
    RUN_TEST(test_import_file_raw16_matches_oracle);
    RUN_TEST(test_import_file_raw8_matches_oracle);
    RUN_TEST(test_import_file_wrong_size_rejected);
    RUN_TEST(test_import_file_missing_rejected);
    RUN_TEST(test_guards);
    return UNITY_END();
}
