/*
 * test_jce_terrain_format.c
 *
 * Cooked terrain v3 codec.
 *
 * v1/v2 wrote native float and uint32_t arrays straight to disk while calling
 * the format little-endian, with no checksums, no random-access directory and
 * no independently stored tiles.  v3 exists so terrain can be paged, verified
 * and cook-hashed.  These tests hold the codec to that: a round trip must be
 * exact, and every corruption the format claims to detect must actually be
 * detected rather than producing plausible garbage.
 *
 * The corruption cases matter more than the happy path.  A codec that decodes
 * valid files is easy; one that REJECTS invalid ones is what makes streaming
 * and byte-identical double-cook safe.
 */

#include "jce_terrain_format.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SX 17u
#define SZ 13u
#define BASE   (-64.0f)
#define RANGE  256.0f

static float g_src[SX * SZ];
static uint8_t *g_buf;
static size_t   g_size;

static void build(void)
{
    for (uint32_t z = 0; z < SZ; z++)
        for (uint32_t x = 0; x < SX; x++)
            g_src[z * SX + x] =
                BASE + RANGE * ((float)(x * 7u + z * 3u) / (float)(SX * 7u + SZ * 3u));

    g_size = jce_terrain_write_size_height_only(SX, SZ);
    g_buf  = (uint8_t *)malloc(g_size);
    TEST_ASSERT_NOT_NULL(g_buf);

    size_t written = 0;
    JceTerrainStatus s = jce_terrain_write_height_only(
        g_buf, g_size, g_src, SX, SZ, 512.0f, 512.0f, BASE, RANGE,
        JCE_TERRAIN_HEIGHT_R16_UNORM, JCE_TERRAIN_DIAG_DIAMOND, &written);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK, s);
    TEST_ASSERT_EQUAL_UINT64(g_size, written);
}

static void teardown_buf(void) { free(g_buf); g_buf = NULL; }

/* ── 1. Round trip is exact to one quantisation unit ───────────────── */

static void test_round_trip(void)
{
    build();

    JceTerrainHeader h;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
                          jce_terrain_read_header(g_buf, g_size, &h));
    TEST_ASSERT_EQUAL_UINT32(SX, h.width_samples);
    TEST_ASSERT_EQUAL_UINT32(SZ, h.height_samples);
    TEST_ASSERT_EQUAL_UINT32(1u, h.tile_count);
    /* The two fields added beyond the original draft must survive. */
    TEST_ASSERT_EQUAL_UINT8(JCE_TERRAIN_DIAG_DIAMOND, h.diagonal_rule);
    TEST_ASSERT_EQUAL_UINT8(1u, h.weight_texture_count);

    JceTerrainDirEntry e;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
                          jce_terrain_read_directory(g_buf, g_size, &h, &e));

    float *out = (float *)malloc(sizeof(float) * SX * SZ);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_tile_heights(g_buf, g_size, &h, &e, out));

    /* R16 over a 256-unit range: one unit is 256/65535 ~= 0.0039. */
    const float tol = RANGE / 65535.0f * 1.5f;
    for (uint32_t i = 0; i < SX * SZ; i++)
        TEST_ASSERT_FLOAT_WITHIN(tol, g_src[i], out[i]);

    free(out);
    teardown_buf();
}

/* ── 2. Decoded min/max match the data, not the declared range ─────── */

static void test_directory_minmax_is_measured(void)
{
    build();
    JceTerrainHeader h;
    jce_terrain_read_header(g_buf, g_size, &h);
    JceTerrainDirEntry e;
    jce_terrain_read_directory(g_buf, g_size, &h, &e);

    float lo = g_src[0], hi = g_src[0];
    for (uint32_t i = 1; i < SX * SZ; i++) {
        if (g_src[i] < lo) lo = g_src[i];
        if (g_src[i] > hi) hi = g_src[i];
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, lo, e.min_height);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, hi, e.max_height);
    /* Must be tighter than the declared interval, or it is not measured. */
    TEST_ASSERT_TRUE(e.min_height > BASE - 0.01f);
    teardown_buf();
}

/* ── 3. Truncation is detected, not read past ──────────────────────── */

static void test_truncation_detected(void)
{
    build();
    JceTerrainHeader h;
    /* Shorter than a header. */
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_TRUNCATED,
                          jce_terrain_read_header(g_buf, 64u, &h));
    /* Header intact but the file is cut short: the declared payload no longer
     * matches the buffer, which must fail rather than decode partial tiles. */
    TEST_ASSERT_NOT_EQUAL_INT(JCE_TERRAIN_OK,
                              jce_terrain_read_header(g_buf, g_size - 16u, &h));
    teardown_buf();
}

/* ── 4. Bad magic and bad version are rejected ─────────────────────── */

static void test_magic_and_version(void)
{
    build();
    JceTerrainHeader h;

    g_buf[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_MAGIC,
                          jce_terrain_read_header(g_buf, g_size, &h));
    g_buf[0] ^= 0xFFu;

    g_buf[4] = 99u;   /* version */
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_VERSION,
                          jce_terrain_read_header(g_buf, g_size, &h));
    teardown_buf();
}

/* ── 5. THE POINT: silent corruption is caught by the hashes ───────── */

static void test_header_corruption_caught(void)
{
    build();
    JceTerrainHeader h;
    /* Flip a dimension.  Without the header self-hash this would decode as a
     * perfectly plausible but wrong terrain. */
    g_buf[16] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_HEADER_HASH,
                          jce_terrain_read_header(g_buf, g_size, &h));
    teardown_buf();
}

static void test_directory_corruption_caught(void)
{
    build();
    JceTerrainHeader h;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
                          jce_terrain_read_header(g_buf, g_size, &h));

    JceTerrainDirEntry e;
    g_buf[JCE_TERRAIN_HEADER_BYTES + 32] ^= 0x10u;   /* min_height */
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_DIR_HASH,
                          jce_terrain_read_directory(g_buf, g_size, &h, &e));
    teardown_buf();
}

static void test_tile_corruption_caught(void)
{
    build();
    JceTerrainHeader h;
    JceTerrainDirEntry e;
    jce_terrain_read_header(g_buf, g_size, &h);
    jce_terrain_read_directory(g_buf, g_size, &h, &e);

    /* Flip one height sample.  Per-tile hashing is what makes corruption
     * isolate to a tile instead of poisoning the whole terrain silently. */
    g_buf[e.stored_offset + JCE_TERRAIN_TILEHDR_BYTES + 4] ^= 0x20u;

    float *out = (float *)malloc(sizeof(float) * SX * SZ);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_TILE_HASH,
        jce_terrain_read_tile_heights(g_buf, g_size, &h, &e, out));
    free(out);
    teardown_buf();
}

/* ── 6. Reserved bytes must be zero ────────────────────────────────── */

static void test_reserved_must_be_zero(void)
{
    build();
    JceTerrainHeader h;
    g_buf[120] = 1u;                       /* reserved tail */
    /* The hash covers 0..111, so a reserved byte at 120 passes the hash and
     * must be caught by the explicit reserved check -- exactly the case a
     * hash alone would miss. */
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_RESERVED,
                          jce_terrain_read_header(g_buf, g_size, &h));
    teardown_buf();
}

/* ── 7. Writer rejects out-of-range data instead of clamping ───────── */

static void test_writer_rejects_out_of_range(void)
{
    float bad[SX * SZ];
    for (uint32_t i = 0; i < SX * SZ; i++) bad[i] = BASE;
    bad[5] = BASE + RANGE * 2.0f;          /* above the declared interval */

    size_t need = jce_terrain_write_size_height_only(SX, SZ);
    uint8_t *b = (uint8_t *)malloc(need);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_RANGE,
        jce_terrain_write_height_only(b, need, bad, SX, SZ, 1.0f, 1.0f,
                                      BASE, RANGE,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_FIXED, NULL));

    bad[5] = (float)NAN;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_RANGE,
        jce_terrain_write_height_only(b, need, bad, SX, SZ, 1.0f, 1.0f,
                                      BASE, RANGE,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_FIXED, NULL));
    free(b);
}

/* ── 8. A short destination is refused, never overrun ──────────────── */

static void test_writer_refuses_short_buffer(void)
{
    float ok[SX * SZ];
    for (uint32_t i = 0; i < SX * SZ; i++) ok[i] = BASE;
    size_t need = jce_terrain_write_size_height_only(SX, SZ);
    uint8_t *b = (uint8_t *)malloc(need);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_TRUNCATED,
        jce_terrain_write_height_only(b, need - 1u, ok, SX, SZ, 1.0f, 1.0f,
                                      BASE, RANGE,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_FIXED, NULL));
    free(b);
}

/* ── 9. Byte-identical double write (cook determinism gate) ────────── */

static void test_double_write_is_byte_identical(void)
{
    build();
    size_t need = jce_terrain_write_size_height_only(SX, SZ);
    uint8_t *b2 = (uint8_t *)malloc(need);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_height_only(b2, need, g_src, SX, SZ, 512.0f, 512.0f,
                                      BASE, RANGE,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_DIAMOND, NULL));
    /* Two cooks of the same input must be identical byte for byte -- this is
     * the property the release gate depends on. */
    TEST_ASSERT_EQUAL_MEMORY(g_buf, b2, need);
    free(b2);
    teardown_buf();
}

/* ── Ridge channel ─────────────────────────────────────────────────────
 *
 * The erosion pass produces a ridge/crease mask that drives splat weights and
 * foliage density.  It is COOKED rather than recomputed at load, because
 * recomputing would mean shipping the erosion filter and its exact parameters
 * into the runtime -- and any drift between the two would silently move every
 * boundary that reads the mask. */

#define RW 11u
#define RH 9u

static float g_ridge_src[RW * RH];
static float g_rheights[RW * RH];

static void ridge_fill(void)
{
    for (uint32_t z = 0; z < RH; z++)
        for (uint32_t x = 0; x < RW; x++) {
            g_rheights[z * RW + x] = -10.0f + 40.0f * ((float)x / (float)RW);
            /* Spans the full [-1,1] so the encoding is exercised at both ends
             * and in the middle, not just near zero. */
            g_ridge_src[z * RW + x] =
                -1.0f + 2.0f * ((float)(x + z) / (float)(RW + RH));
        }
}

/* Adding the channel must not disturb the height-only output. */
static void test_null_ridge_is_byte_identical(void)
{
    ridge_fill();
    const size_t n = jce_terrain_write_size_height_only(RW, RH);
    uint8_t *a = (uint8_t *)malloc(n);
    uint8_t *b = (uint8_t *)malloc(n);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_height_only(a, n, g_rheights, RW, RH, 64.0f, 64.0f,
                                      -10.0f, 40.0f,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_FIXED, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_height_ridge(b, n, g_rheights, NULL, RW, RH,
                                       64.0f, 64.0f, -10.0f, 40.0f,
                                       JCE_TERRAIN_HEIGHT_R16_UNORM,
                                       JCE_TERRAIN_DIAG_FIXED, NULL));
    TEST_ASSERT_EQUAL_MEMORY(a, b, n);
    free(a); free(b);
}

static void test_ridge_round_trips(void)
{
    ridge_fill();
    const size_t n = jce_terrain_write_size_height_ridge(RW, RH);
    uint8_t *buf = (uint8_t *)malloc(n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_height_ridge(buf, n, g_rheights, g_ridge_src, RW, RH,
                                       64.0f, 64.0f, -10.0f, 40.0f,
                                       JCE_TERRAIN_HEIGHT_R16_UNORM,
                                       JCE_TERRAIN_DIAG_FIXED, NULL));

    JceTerrainHeader hdr;
    JceTerrainDirEntry e;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK, jce_terrain_read_header(buf, n, &hdr));
    TEST_ASSERT_TRUE((hdr.channel_mask & JCE_TERRAIN_CH_RIDGE) != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_directory(buf, n, &hdr, &e));

    /* Heights must survive unchanged -- the ridge section must not have
     * overwritten or shifted them. */
    float got_h[RW * RH];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_tile_heights(buf, n, &hdr, &e, got_h));
    for (uint32_t i = 0; i < RW * RH; i++)
        TEST_ASSERT_FLOAT_WITHIN(40.0f / 65535.0f * 1.5f, g_rheights[i], got_h[i]);

    float got_r[RW * RH];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_tile_ridge(buf, n, &hdr, &e, got_r));
    for (uint32_t i = 0; i < RW * RH; i++)
        TEST_ASSERT_FLOAT_WITHIN(2.0f / 65535.0f * 1.5f, g_ridge_src[i], got_r[i]);

    free(buf);
}

/* A tile with no ridge channel reports that distinctly, not as corruption. */
static void test_absent_ridge_is_unsupported_not_corrupt(void)
{
    ridge_fill();
    const size_t n = jce_terrain_write_size_height_only(RW, RH);
    uint8_t *buf = (uint8_t *)malloc(n);
    jce_terrain_write_height_only(buf, n, g_rheights, RW, RH, 64.0f, 64.0f,
                                  -10.0f, 40.0f, JCE_TERRAIN_HEIGHT_R16_UNORM,
                                  JCE_TERRAIN_DIAG_FIXED, NULL);
    JceTerrainHeader hdr;
    JceTerrainDirEntry e;
    jce_terrain_read_header(buf, n, &hdr);
    jce_terrain_read_directory(buf, n, &hdr, &e);

    float got_r[RW * RH];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_UNSUPPORTED,
        jce_terrain_read_tile_ridge(buf, n, &hdr, &e, got_r));
    free(buf);
}

/* Corruption anywhere in the payload must still be caught -- the ridge decode
 * verifies the tile hash before it reads a single sample. */
static void test_ridge_corruption_is_caught(void)
{
    ridge_fill();
    const size_t n = jce_terrain_write_size_height_ridge(RW, RH);
    uint8_t *buf = (uint8_t *)malloc(n);
    jce_terrain_write_height_ridge(buf, n, g_rheights, g_ridge_src, RW, RH,
                                   64.0f, 64.0f, -10.0f, 40.0f,
                                   JCE_TERRAIN_HEIGHT_R16_UNORM,
                                   JCE_TERRAIN_DIAG_FIXED, NULL);
    JceTerrainHeader hdr;
    JceTerrainDirEntry e;
    jce_terrain_read_header(buf, n, &hdr);
    jce_terrain_read_directory(buf, n, &hdr, &e);

    /* Flip a bit inside the RIDGE section specifically. */
    buf[e.stored_offset + JCE_TERRAIN_TILEHDR_BYTES + RW * RH * 2u + 2u] ^= 0x08u;

    float got_r[RW * RH];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_TILE_HASH,
        jce_terrain_read_tile_ridge(buf, n, &hdr, &e, got_r));
    free(buf);
}

/* A ridge outside [-1,1] means the producer and the format disagree about what
 * the channel is; that is refused, not clamped. */
static void test_out_of_range_ridge_refused(void)
{
    ridge_fill();
    g_ridge_src[3] = 2.5f;
    const size_t n = jce_terrain_write_size_height_ridge(RW, RH);
    uint8_t *buf = (uint8_t *)malloc(n);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_RANGE,
        jce_terrain_write_height_ridge(buf, n, g_rheights, g_ridge_src, RW, RH,
                                       64.0f, 64.0f, -10.0f, 40.0f,
                                       JCE_TERRAIN_HEIGHT_R16_UNORM,
                                       JCE_TERRAIN_DIAG_FIXED, NULL));
    free(buf);
    ridge_fill();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_directory_minmax_is_measured);
    RUN_TEST(test_truncation_detected);
    RUN_TEST(test_magic_and_version);
    RUN_TEST(test_header_corruption_caught);
    RUN_TEST(test_directory_corruption_caught);
    RUN_TEST(test_tile_corruption_caught);
    RUN_TEST(test_writer_rejects_out_of_range);
    RUN_TEST(test_writer_refuses_short_buffer);
    RUN_TEST(test_double_write_is_byte_identical);
    RUN_TEST(test_null_ridge_is_byte_identical);
    RUN_TEST(test_ridge_round_trips);
    RUN_TEST(test_absent_ridge_is_unsupported_not_corrupt);
    RUN_TEST(test_ridge_corruption_is_caught);
    RUN_TEST(test_out_of_range_ridge_refused);
    return UNITY_END();
}
