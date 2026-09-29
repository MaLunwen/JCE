/*
 * test_jce_pak_loader.c  Unit tests for jce_pak_loader.h (L2 / resource).
 *
 * Strategy: cook a tiny synthetic JPAK v1 archive in memory via the public
 * archive writer (jce_archive_writer_*).  The tiny payloads are incompressible
 * at this size, so the writer's "keep only if it helps" guard stores them
 * uncompressed (STORED).  Path normalization + hashing is done by the writer,
 * matching what jce_pak_find() recomputes on lookup.
 *
 * Covers happy paths (open/count/get/find/decompress/verify/acquire+close)
 * plus the documented rejection paths (bad magic, wrong version, oversized
 * TOC count, NULL inputs).
 */

#include "unity.h"

#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_alloc.h>

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <jce/resource/jce_archive_writer.h>

/* ---- in-memory PAK builder ----------------------------------------- */

typedef struct {
    const char *path;
    const void *data;
    size_t      size;
} TestEntry;

/* Build a JPAK v1 archive (format_version 1) containing `n` entries using the
   public archive writer.  The tiny payloads below are incompressible at this
   size, so the writer's "keep only if it helps" guard (spec §6.3) stores them
   uncompressed (JCE_PAK_ASSET_STORED).  The returned buffer is jce_malloc'd;
   callers free it with jce_free().  *out_size receives the byte count. */
static void *build_pak(const TestEntry *e, uint32_t n, size_t *out_size)
{
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);

    for (uint32_t i = 0; i < n; ++i) {
        bool ok = jce_archive_writer_add(w, e[i].path, e[i].data, e[i].size);
        TEST_ASSERT_TRUE(ok);
    }

    void  *buf = NULL;
    size_t sz  = 0;
    bool   fin = jce_archive_writer_finish(w, &buf, &sz);
    TEST_ASSERT_TRUE(fin);
    TEST_ASSERT_NOT_NULL(buf);
    jce_archive_writer_destroy(w);

    *out_size = sz;
    return buf;
}

/* Poke a little-endian u32 into the v1 header at byte offset `off` (spec §4.2)
   so the rejection-path tests can corrupt specific fields. */
static void poke_u32(void *blob, size_t off, uint32_t v)
{
    uint8_t *p = (uint8_t *)blob + off;
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void setUp(void)    { }
void tearDown(void) { }

/* ---- happy paths ---------------------------------------------------- */

static void test_open_count_and_get(void)
{
    const TestEntry items[] = {
        { "alpha.txt", "AAA",        3 },
        { "beta.bin",  "\x01\x02\x03\x04", 4 },
        { "long/nested/path.dat", "deadbeef", 8 },
    };
    size_t sz = 0;
    void *blob = build_pak(items, 3, &sz);

    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);
    TEST_ASSERT_EQUAL_UINT32(3, jce_pak_count(pak));

    /* Every index resolves; out-of-range is NULL. */
    TEST_ASSERT_NOT_NULL(jce_pak_get(pak, 0));
    TEST_ASSERT_NOT_NULL(jce_pak_get(pak, 1));
    TEST_ASSERT_NOT_NULL(jce_pak_get(pak, 2));
    TEST_ASSERT_NULL    (jce_pak_get(pak, 3));

    jce_pak_close(pak);
    jce_free(blob);
}

static void test_find_and_decompress_stored(void)
{
    const char payload[] = "hello jpak";
    const TestEntry items[] = {
        { "msg.txt", payload, sizeof(payload) - 1 },
    };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);

    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const JcePakAsset *a = jce_pak_find(pak, "msg.txt");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT64(sizeof(payload) - 1, a->original_size);
    TEST_ASSERT_TRUE(a->flags & JCE_PAK_ASSET_STORED);

    char out[32] = { 0 };
    size_t n = jce_pak_decompress(a, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload) - 1, n);
    TEST_ASSERT_EQUAL_STRING_LEN(payload, out, sizeof(payload) - 1);

    /* content_hash matches what we wrote -> verify passes. */
    TEST_ASSERT_EQUAL_INT(1, jce_pak_verify(a, out, n));

    jce_pak_close(pak);
    jce_free(blob);
}

static void test_find_missing_returns_null(void)
{
    const TestEntry items[] = { { "only.bin", "x", 1 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);

    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NULL(jce_pak_find(pak, "absent.txt"));
    jce_pak_close(pak);
    jce_free(blob);
}

static void test_decompress_buffer_too_small(void)
{
    const TestEntry items[] = { { "f", "12345", 5 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);

    const JcePakAsset *a = jce_pak_find(pak, "f");
    char small[2];
    TEST_ASSERT_EQUAL_size_t(0, jce_pak_decompress(a, small, sizeof(small)));

    jce_pak_close(pak);
    jce_free(blob);
}

static void test_acquire_release_refcount(void)
{
    const TestEntry items[] = { { "r", "z", 1 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);

    TEST_ASSERT_EQUAL_INT(1, jce_pak_refcount(pak));
    JcePakArchive *p2 = jce_pak_acquire(pak);
    TEST_ASSERT_EQUAL_PTR(pak, p2);
    TEST_ASSERT_EQUAL_INT(2, jce_pak_refcount(pak));

    jce_pak_close(pak);                         /* drops one ref */
    TEST_ASSERT_EQUAL_INT(1, jce_pak_refcount(p2));
    jce_pak_close(p2);                          /* final ref */
    jce_free(blob);
}

static void test_open_owned_takes_buffer(void)
{
    const TestEntry items[] = { { "o", "owned", 5 } };
    size_t sz = 0;
    /* build_pak already returns a jce_malloc'd buffer, which is exactly what
       jce_pak_open_owned() takes ownership of (freed via JCE_FREE on close). */
    void *engine_blob = build_pak(items, 1, &sz);

    JcePakArchive *pak = jce_pak_open_owned(engine_blob, sz);
    TEST_ASSERT_NOT_NULL(pak);
    TEST_ASSERT_EQUAL_UINT32(1, jce_pak_count(pak));
    jce_pak_close(pak);
    /* engine_blob freed by jce_pak_close via JCE_FREE  no leak. */
}

/* ---- error / rejection paths --------------------------------------- */

static void test_open_null_returns_null(void)
{
    TEST_ASSERT_NULL(jce_pak_open(NULL, 0));
    TEST_ASSERT_NULL(jce_pak_open("xx", 2));   /* too small for header */
}

static void test_open_bad_magic(void)
{
    const TestEntry items[] = { { "x", "y", 1 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);
    ((uint8_t *)blob)[0] = 'X';                /* corrupt magic */
    TEST_ASSERT_NULL(jce_pak_open(blob, sz));
    jce_free(blob);
}

static void test_open_wrong_version(void)
{
    const TestEntry items[] = { { "x", "y", 1 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);
    poke_u32(blob, 4, 99u);                     /* format_version @ off 4 */
    TEST_ASSERT_NULL(jce_pak_open(blob, sz));
    jce_free(blob);
}

static void test_open_truncated_toc(void)
{
    const TestEntry items[] = { { "x", "y", 1 } };
    size_t sz = 0;
    void *blob = build_pak(items, 1, &sz);
    /* Claim 1000 entries — index region will exceed blob size (entry_count
       @ off 12, spec §4.2). */
    poke_u32(blob, 12, 1000u);
    TEST_ASSERT_NULL(jce_pak_open(blob, sz));
    jce_free(blob);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_count_and_get);
    RUN_TEST(test_find_and_decompress_stored);
    RUN_TEST(test_find_missing_returns_null);
    RUN_TEST(test_decompress_buffer_too_small);
    RUN_TEST(test_acquire_release_refcount);
    RUN_TEST(test_open_owned_takes_buffer);
    RUN_TEST(test_open_null_returns_null);
    RUN_TEST(test_open_bad_magic);
    RUN_TEST(test_open_wrong_version);
    RUN_TEST(test_open_truncated_toc);
    return UNITY_END();
}
