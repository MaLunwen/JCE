/*
 * test_jce_archive.c  Unit tests for the JCE Archive format (L2 / resource).
 *
 * Exercises the Phase-1 writer + reader through the public API only:
 *   - round-trip for stored (NONE) and zstd entries
 *   - path normalization battery (equivalent spellings hash identically)
 *   - the "keep only if it helps" guard
 *   - deterministic output (same inputs/config -> identical bytes)
 *   - layered integrity checks detect data/index corruption
 *   - duplicate / hash-collision rejection on add
 *   - optional debug path table round-trip
 */

#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>

#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_archive_loader.h>
#include <jce/os/core/jce_alloc.h>

#include "resource/jce_archive_crypto.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

void setUp(void)    {}
void tearDown(void)
{
    /* Tests that install a process-wide decryption key must not leak it
     * into later tests — encrypted-open auto-apply would mask failures. */
    jce_archive_set_process_key(NULL);
    /* Likewise the opt-in verify-on-open gate must not leak across tests. */
    jce_archive_set_verify_on_open(false);
}

/* Fill a buffer with deterministic high-entropy bytes (LCG). */
static void fill_random(uint8_t *p, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = (uint8_t)(s >> 24);
    }
}

/* ── crypto known-answer tests (RFC 4231 / RFC 8439) ──────────────── */

static void test_crypto_hmac_sha256_rfc4231(void) {
    static const uint8_t key[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
    };
    static const uint8_t expected[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7,
    };
    static const uint8_t message[] = "Hi There";
    uint8_t digest[32];

    jce_archive_hmac_sha256(key, sizeof(key), message,
                            sizeof(message) - 1u, digest);
    TEST_ASSERT_EQUAL_MEMORY(expected, digest, sizeof(digest));
}

static void test_crypto_chacha20_rfc8439(void) {
    static const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    };
    static const uint8_t nonce[12] = {
        0x00,0x00,0x00,0x09,0x00,0x00,0x00,0x4a,
        0x00,0x00,0x00,0x00,
    };
    static const uint8_t expected[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,
        0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03,
        0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09,
        0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9,
        0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e,
    };
    uint8_t input[64] = {0};
    uint8_t output[64];

    jce_archive_chacha20_xor(key, nonce, 1, input, output, sizeof(output));
    TEST_ASSERT_EQUAL_MEMORY(expected, output, sizeof(output));
}

/* ── round-trip: stored + zstd ───────────────────────────────────────── */

static void test_roundtrip_stored_and_zstd(void) {
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);

    uint8_t rnd[2048];
    fill_random(rnd, sizeof(rnd), 0xC0FFEE);
    uint8_t comp[4096];
    memset(comp, 'A', sizeof(comp));

    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "data/random.bin", rnd, sizeof(rnd)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "text/aaa.txt", comp, sizeof(comp)));

    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    TEST_ASSERT_NOT_NULL(buf);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_count(ar));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));

    const JceArchiveEntry *er = jce_archive_find(ar, "data/random.bin");
    const JceArchiveEntry *ec = jce_archive_find(ar, "text/aaa.txt");
    TEST_ASSERT_NOT_NULL(er);
    TEST_ASSERT_NOT_NULL(ec);

    /* Random data must fall back to stored; 'A'*4096 must compress. */
    TEST_ASSERT_EQUAL_UINT8(0 /*NONE*/, er->compression);
    TEST_ASSERT_EQUAL_UINT8(1 /*ZSTD*/, ec->compression);
    TEST_ASSERT_TRUE(ec->stored_size < ec->original_size);

    uint8_t *out = (uint8_t *)jce_malloc(4096);
    TEST_ASSERT_EQUAL_size_t(sizeof(rnd), jce_archive_read(ar, er, out, 4096));
    TEST_ASSERT_EQUAL_MEMORY(rnd, out, sizeof(rnd));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_entry(er, out, sizeof(rnd)));

    TEST_ASSERT_EQUAL_size_t(sizeof(comp), jce_archive_read(ar, ec, out, 4096));
    TEST_ASSERT_EQUAL_MEMORY(comp, out, sizeof(comp));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_entry(ec, out, sizeof(comp)));

    jce_free(out);
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── path normalization battery ──────────────────────────────────────── */

static void test_normalization(void) {
    const char *equiv[] = {
        "textures/player.png",
        "Textures/Player.PNG",
        "textures\\player.png",
        "/textures/player.png",
        "C:\\textures\\player.png",
        "textures//player.png",
        "./textures/player.png",
        "textures/./player.png",
        "foo/../textures/player.png",
    };
    uint64_t h0 = jce_archive_hash_path(equiv[0]);
    TEST_ASSERT_NOT_EQUAL_UINT64(0, h0);
    for (size_t i = 1; i < sizeof(equiv) / sizeof(equiv[0]); i++) {
        TEST_ASSERT_EQUAL_UINT64(h0, jce_archive_hash_path(equiv[i]));
    }
    /* A genuinely different path hashes differently. */
    TEST_ASSERT_NOT_EQUAL_UINT64(h0, jce_archive_hash_path("textures/enemy.png"));

    char out[64];
    size_t n = jce_archive_normalize_path("C:\\Foo\\\\Bar/../baz.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(strlen("foo/baz.txt"), n);
    TEST_ASSERT_EQUAL_STRING("foo/baz.txt", out);

    /* find() resolves every equivalent spelling. */
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    uint8_t d[8] = {1,2,3,4,5,6,7,8};
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "textures/player.png", d, sizeof(d)));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    for (size_t i = 0; i < sizeof(equiv) / sizeof(equiv[0]); i++) {
        TEST_ASSERT_NOT_NULL(jce_archive_find(ar, equiv[i]));
    }
    TEST_ASSERT_NULL(jce_archive_find(ar, "textures/enemy.png"));
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── determinism: order-independent, byte-identical output ───────────── */

static void test_determinism(void) {
    uint8_t a[100], b[200], c[37];
    fill_random(a, sizeof(a), 11);
    fill_random(b, sizeof(b), 22);
    fill_random(c, sizeof(c), 33);

    void *buf1 = NULL, *buf2 = NULL; size_t s1 = 0, s2 = 0;

    JceArchiveWriter *w1 = jce_archive_writer_create(NULL);
    jce_archive_writer_add(w1, "a.bin", a, sizeof(a));
    jce_archive_writer_add(w1, "b.bin", b, sizeof(b));
    jce_archive_writer_add(w1, "c.bin", c, sizeof(c));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w1, &buf1, &s1));
    jce_archive_writer_destroy(w1);

    /* Different insertion order, same content + config. */
    JceArchiveWriter *w2 = jce_archive_writer_create(NULL);
    jce_archive_writer_add(w2, "c.bin", c, sizeof(c));
    jce_archive_writer_add(w2, "a.bin", a, sizeof(a));
    jce_archive_writer_add(w2, "b.bin", b, sizeof(b));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w2, &buf2, &s2));
    jce_archive_writer_destroy(w2);

    TEST_ASSERT_EQUAL_size_t(s1, s2);
    TEST_ASSERT_EQUAL_MEMORY(buf1, buf2, s1);

    jce_free(buf1);
    jce_free(buf2);
}

/* ── corruption detection ────────────────────────────────────────────── */

static void test_corruption_detected(void) {
    uint8_t comp[1024];
    memset(comp, 'Z', sizeof(comp));

    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    jce_archive_writer_add(w, "x.dat", comp, sizeof(comp));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    uint8_t *bytes = (uint8_t *)buf;

    /* Pristine archive verifies. */
    JceArchive *ar = jce_archive_open(bytes, sz);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));
    jce_archive_close(ar);

    /* Corrupt a data-region byte (just past the 64-byte header). */
    bytes[64] ^= 0xFF;
    ar = jce_archive_open(bytes, sz);
    TEST_ASSERT_NOT_NULL(ar); /* open still succeeds; only hashes mismatch */
    TEST_ASSERT_EQUAL_INT(0, jce_archive_verify_header(ar));
    jce_archive_close(ar);
    bytes[64] ^= 0xFF; /* restore */

    /* Corrupt the last byte (inside the index region). */
    bytes[sz - 1] ^= 0xFF;
    ar = jce_archive_open(bytes, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_INT(0, jce_archive_verify_header(ar));
    jce_archive_close(ar);

    jce_free(buf);
}

/* ── duplicate / collision rejection ─────────────────────────────────── */

static void test_duplicate_rejected(void) {
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    uint8_t d[4] = {9, 9, 9, 9};
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "dup/path.txt", d, sizeof(d)));
    /* Same logical path (different spelling) must be rejected. */
    TEST_ASSERT_FALSE(jce_archive_writer_add(w, "DUP\\Path.txt", d, sizeof(d)));
    jce_archive_writer_destroy(w);
}

/* ── debug path table ────────────────────────────────────────────────── */

static void test_debug_paths(void) {
    JceArchiveWriterConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.zstd_level = 9;
    cfg.alignment_log2 = 4;
    cfg.emit_debug_paths = true;

    JceArchiveWriter *w = jce_archive_writer_create(&cfg);
    uint8_t d[2] = {7, 7};
    jce_archive_writer_add(w, "Assets/One.txt", d, sizeof(d));
    jce_archive_writer_add(w, "assets/two.txt", d, sizeof(d));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_count(ar));

    /* Each entry index maps to its normalized debug path string. */
    int seen_one = 0, seen_two = 0;
    for (uint32_t i = 0; i < jce_archive_count(ar); i++) {
        const char *p = jce_archive_debug_path(ar, i);
        TEST_ASSERT_NOT_NULL(p);
        if (strcmp(p, "assets/one.txt") == 0) seen_one = 1;
        if (strcmp(p, "assets/two.txt") == 0) seen_two = 1;
    }
    TEST_ASSERT_TRUE(seen_one && seen_two);

    jce_archive_close(ar);
    jce_free(buf);
}

/* ── dictionary compression (spec §7) ────────────────────────────────── */

static void test_dictionary_roundtrip(void) {
    enum { NSAMP = 2000 };
    char        **samples = (char **)jce_malloc(sizeof(char *) * NSAMP);
    const void  **ptrs    = (const void **)jce_malloc(sizeof(void *) * NSAMP);
    size_t       *sizes   = (size_t *)jce_malloc(sizeof(size_t) * NSAMP);
    TEST_ASSERT_NOT_NULL(samples);
    TEST_ASSERT_NOT_NULL(ptrs);
    TEST_ASSERT_NOT_NULL(sizes);

    /* A corpus of small, structurally similar JSON records — exactly the case
     * dictionary compression targets (spec §7.1). */
    for (int i = 0; i < NSAMP; i++) {
        char tmp[160];
        int n = snprintf(tmp, sizeof(tmp),
            "{\"name\":\"item_%d\",\"type\":\"widget\",\"enabled\":true,"
            "\"value\":%d,\"group\":\"common\"}", i, i * 7);
        char *s = (char *)jce_malloc((size_t)n + 1);
        TEST_ASSERT_NOT_NULL(s);
        memcpy(s, tmp, (size_t)n + 1);
        samples[i] = s; ptrs[i] = s; sizes[i] = (size_t)n;
    }

    uint8_t dict[16 * 1024];
    size_t dsz = jce_archive_train_dictionary(ptrs, sizes, NSAMP, 4096,
                                              dict, sizeof(dict));
    TEST_ASSERT_TRUE(dsz > 0);

    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    int id = jce_archive_writer_add_dictionary(w, JCE_ARCHIVE_TAG('J','S','O','N'),
                                               dict, dsz);
    TEST_ASSERT_EQUAL_INT(0, id);

    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "cfg/item_%d.json", i);
        TEST_ASSERT_TRUE(jce_archive_writer_add_with_dict(w, path, samples[i],
                                                          sizes[i], id));
    }

    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));
    TEST_ASSERT_EQUAL_UINT16(1, jce_archive_dict_count(ar));
    TEST_ASSERT_EQUAL_UINT32(JCE_ARCHIVE_TAG('J','S','O','N'),
                             jce_archive_dict_tag(ar, 0));

    int saw_dict = 0;
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "cfg/item_%d.json", i);
        const JceArchiveEntry *e = jce_archive_find(ar, path);
        TEST_ASSERT_NOT_NULL(e);
        if (e->compression == 2 /*ZSTD_DICT*/) {
            saw_dict = 1;
            TEST_ASSERT_EQUAL_UINT16(0, e->dict_id);
        }
        uint8_t out[256];
        size_t got = jce_archive_read(ar, e, out, sizeof(out));
        TEST_ASSERT_EQUAL_size_t(sizes[i], got);
        TEST_ASSERT_EQUAL_MEMORY(samples[i], out, sizes[i]);
        TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_entry(e, out, got));
    }
    /* The dictionary path must have been exercised and kept for at least one
     * small record (it compresses well against the shared model). */
    TEST_ASSERT_TRUE(saw_dict);

    jce_archive_close(ar);
    jce_free(buf);
    for (int i = 0; i < NSAMP; i++) jce_free(samples[i]);
    jce_free(samples); jce_free(ptrs); jce_free(sizes);
}

/* ── rejection paths ─────────────────────────────────────────────────── */

static void test_open_rejects_garbage(void) {
    uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    TEST_ASSERT_NULL(jce_archive_open(junk, sizeof(junk)));   /* bad magic */
    TEST_ASSERT_NULL(jce_archive_open(NULL, 0));
    TEST_ASSERT_NULL(jce_archive_open(junk, 8));               /* too small */
}

/* ── mmap / zero-copy (spec §8) ───────────────────────────────────────── */

static void test_mmap_zero_copy(void) {
    JceArchiveWriterConfig cfg = {0};
    cfg.zstd_level = 9;
    cfg.alignment_log2 = 4;
    cfg.mmap_friendly = true;

    JceArchiveWriter *w = jce_archive_writer_create(&cfg);
    TEST_ASSERT_NOT_NULL(w);

    /* High-entropy payload -> stored NONE (eligible for zero-copy). */
    uint8_t rnd[3000];
    fill_random(rnd, sizeof(rnd), 0x5EED);
    /* Compressible payload -> zstd (NOT eligible for zero-copy). */
    uint8_t comp[8192];
    memset(comp, 'Z', sizeof(comp));

    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "blob/raw.bin", rnd, sizeof(rnd)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "text/big.txt", comp, sizeof(comp)));

    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    TEST_ASSERT_NOT_NULL(buf);

    const char *tmp = "test_jce_archive_mmap.tmp";
    FILE *f = fopen(tmp, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(sz, fwrite(buf, 1, sz, f));
    fclose(f);
    jce_free(buf);

    JceArchive *ar = jce_archive_open_file(tmp);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_count(ar));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));

    /* Uncompressed entry: zero-copy pointer matches original bytes. */
    const JceArchiveEntry *raw = jce_archive_find(ar, "blob/raw.bin");
    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT8(JCE_ARCHIVE_COMP_NONE, raw->compression);
    const void *ptr = NULL; size_t mapped = 0;
    TEST_ASSERT_EQUAL_INT(1, jce_archive_map_entry(ar, raw, &ptr, &mapped));
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL_size_t(sizeof(rnd), mapped);
    TEST_ASSERT_EQUAL_MEMORY(rnd, ptr, sizeof(rnd));
    TEST_ASSERT_TRUE((raw->entry_flags & JCE_ARCHIVE_ENTRY_PAGE_ALIGNED) != 0);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(raw->data_offset % 4096));

    /* Compressed entry: not zero-copyable, must use jce_archive_read. */
    const JceArchiveEntry *big = jce_archive_find(ar, "text/big.txt");
    TEST_ASSERT_NOT_NULL(big);
    const void *p2 = NULL; size_t s2 = 0;
    TEST_ASSERT_EQUAL_INT(0, jce_archive_map_entry(ar, big, &p2, &s2));
    uint8_t *dec = (uint8_t *)jce_malloc(big->original_size);
    TEST_ASSERT_NOT_NULL(dec);
    TEST_ASSERT_EQUAL_size_t(sizeof(comp),
                             jce_archive_read(ar, big, dec, big->original_size));
    TEST_ASSERT_EQUAL_MEMORY(comp, dec, sizeof(comp));
    jce_free(dec);

    jce_archive_close(ar);
    remove(tmp);
}

/* ── encryption (spec §9.2) ──────────────────────────────────────────── */

static void test_encryption_roundtrip(void) {
    static const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    };

    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    jce_archive_writer_set_encryption_key(w, key);

    /* Compressible secret -> compress then encrypt; the stored bytes must end
     * up neither plaintext nor matching a plain-zstd frame. */
    uint8_t secret[4096];
    memset(secret, 'S', sizeof(secret));
    TEST_ASSERT_TRUE(jce_archive_writer_add_encrypted(w, "story/secret.txt",
                                                      secret, sizeof(secret), -1));

    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    TEST_ASSERT_NOT_NULL(buf);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));

    const JceArchiveEntry *es = jce_archive_get(ar, 0);
    TEST_ASSERT_NOT_NULL(es);
    TEST_ASSERT_TRUE((es->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) != 0);

    uint8_t *out = (uint8_t *)jce_malloc(sizeof(secret));
    TEST_ASSERT_NOT_NULL(out);

    /* Reading an encrypted entry without a key must fail. */
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_read(ar, es, out, sizeof(secret)));

    /* A wrong key is rejected by HMAC before decryption. */
    uint8_t wrong[32];
    memcpy(wrong, key, sizeof(wrong));
    wrong[0] ^= 0xFF;
    jce_archive_set_decryption_key(ar, wrong);
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_INVALID,
                          jce_archive_auth_status(ar));
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_read(ar, es, out, sizeof(secret)));

    /* The correct key round-trips exactly. */
    jce_archive_set_decryption_key(ar, key);
    es = jce_archive_find(ar, "story/secret.txt");
    TEST_ASSERT_NOT_NULL(es);
    TEST_ASSERT_EQUAL_size_t(sizeof(secret),
                             jce_archive_read(ar, es, out, sizeof(secret)));
    TEST_ASSERT_EQUAL_MEMORY(secret, out, sizeof(secret));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_entry(es, out, sizeof(secret)));

    jce_free(out);
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── encryption: per-archive nonce salt (header nonce_salt32) ────────── */

static const uint8_t k_test_key[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
};

static void build_secure_cooked(const JceCookInput *inputs, size_t count,
                                void **out_buf, size_t *out_size) {
    JceCookConfig cfg = {0};
    cfg.encrypt        = true;
    cfg.encryption_key = k_test_key;
    cfg.encrypt_label  = "secure_test";
    cfg.compress_index = true;
    cfg.dedup_content  = true;
    TEST_ASSERT_TRUE(jce_archive_cook(inputs, count, &cfg,
                                      out_buf, out_size, NULL));
    TEST_ASSERT_NOT_NULL(*out_buf);
}

static int buffer_contains(const void *haystack, size_t haystack_size,
                           const void *needle, size_t needle_size) {
    const uint8_t *h = (const uint8_t *)haystack;
    if (!h || !needle || needle_size == 0 || needle_size > haystack_size)
        return 0;
    for (size_t i = 0; i <= haystack_size - needle_size; ++i) {
        if (memcmp(h + i, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static void test_secure_cook_has_no_plaintext_dictionary(void) {
    enum { SAMPLE_COUNT = 16, SAMPLE_SIZE = 4096 };
    static const char marker[] =
        "scenes/confidential_launch_site.scene.json";
    char payloads[SAMPLE_COUNT][SAMPLE_SIZE];
    char paths[SAMPLE_COUNT][64];
    JceCookInput inputs[SAMPLE_COUNT];

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        size_t used = 0;
        for (int field = 0; field < 32; ++field) {
            int n = snprintf(payloads[i] + used, SAMPLE_SIZE - used,
                             "{\"startup_scene\":\"%s\","
                             "\"sample\":%d,\"field\":%d}\n",
                             marker, i, field);
            TEST_ASSERT_TRUE(n > 0);
            TEST_ASSERT_TRUE((size_t)n < SAMPLE_SIZE - used);
            used += (size_t)n;
        }
        snprintf(paths[i], sizeof(paths[i]), "secure/sample_%02d.json", i);
        inputs[i].vpath = paths[i];
        inputs[i].data = payloads[i];
        inputs[i].size = used;
    }

    JceCookConfig cfg = {0};
    cfg.use_dict       = true;
    cfg.compress_index = true;
    cfg.encrypt        = true;
    cfg.encryption_key = k_test_key;
    cfg.encrypt_label  = "no_plaintext_dictionary";

    void *blob = NULL;
    size_t blob_size = 0;
    uint16_t dict_count = UINT16_MAX;
    TEST_ASSERT_TRUE(jce_archive_cook(inputs, SAMPLE_COUNT, &cfg,
                                      &blob, &blob_size, &dict_count));
    TEST_ASSERT_EQUAL_UINT16(0, dict_count);
    TEST_ASSERT_FALSE(buffer_contains(blob, blob_size, marker,
                                      sizeof(marker) - 1u));

    JceArchive *ar = jce_archive_open(blob, blob_size);
    TEST_ASSERT_NOT_NULL(ar);
    jce_archive_set_decryption_key(ar, k_test_key);
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_VALID,
                          jce_archive_auth_status(ar));
    TEST_ASSERT_EQUAL_UINT16(0, jce_archive_dict_count(ar));
    const JceArchiveEntry *entry = jce_archive_find(ar, paths[0]);
    TEST_ASSERT_NOT_NULL(entry);
    char decoded[SAMPLE_SIZE];
    TEST_ASSERT_EQUAL_size_t(inputs[0].size,
                             jce_archive_read(ar, entry, decoded,
                                              sizeof(decoded)));
    TEST_ASSERT_EQUAL_MEMORY(payloads[0], decoded, inputs[0].size);
    jce_archive_close(ar);
    jce_free(blob);
}

static void test_secure_archive_roundtrip_and_keyed_index(void) {
    uint8_t repeated[2048];
    fill_random(repeated, sizeof(repeated), 0x51C0);
    const JceCookInput inputs[] = {
        { "secret/alpha.payload", repeated, sizeof(repeated) },
        { "secret/beta.payload",  repeated, sizeof(repeated) },
    };
    void *blob = NULL;
    size_t blob_size = 0;
    build_secure_cooked(inputs, 2, &blob, &blob_size);

    JceArchive *ar = jce_archive_open(blob, blob_size);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_TRUE(jce_archive_is_secure(ar));
    TEST_ASSERT_TRUE(jce_archive_is_authenticated(ar));
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_UNAVAILABLE,
                          jce_archive_auth_status(ar));
    TEST_ASSERT_NULL(jce_archive_find(ar, inputs[0].vpath));

    uint8_t wrong[32];
    memcpy(wrong, k_test_key, sizeof(wrong));
    wrong[7] ^= 0x40;
    jce_archive_set_decryption_key(ar, wrong);
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_INVALID,
                          jce_archive_auth_status(ar));
    TEST_ASSERT_NULL(jce_archive_find(ar, inputs[0].vpath));

    jce_archive_set_decryption_key(ar, k_test_key);
    TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_VALID,
                          jce_archive_auth_status(ar));
    const JceArchiveEntry *alpha = jce_archive_find(ar, inputs[0].vpath);
    const JceArchiveEntry *beta  = jce_archive_find(ar, inputs[1].vpath);
    TEST_ASSERT_NOT_NULL(alpha);
    TEST_ASSERT_NOT_NULL(beta);
    TEST_ASSERT_NOT_EQUAL_UINT64(jce_archive_hash_path(inputs[0].vpath),
                                 alpha->path_hash);
    TEST_ASSERT_TRUE(alpha->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED);
    TEST_ASSERT_TRUE(alpha->entry_flags & JCE_ARCHIVE_ENTRY_AUTHENTICATED);
    TEST_ASSERT_NULL(jce_archive_debug_path(ar, 0));
    TEST_ASSERT_EQUAL_UINT64(alpha->data_offset, beta->data_offset);

    uint8_t decoded[2048];
    TEST_ASSERT_EQUAL_size_t(sizeof(repeated),
                             jce_archive_read(ar, alpha, decoded,
                                              sizeof(decoded)));
    TEST_ASSERT_EQUAL_MEMORY(repeated, decoded, sizeof(repeated));
    jce_archive_close(ar);
    jce_free(blob);
}

static void assert_secure_tamper_rejected(size_t flip_offset) {
    uint8_t payload[1536];
    fill_random(payload, sizeof(payload), 0xA117);
    const JceCookInput input = {
        "secure/tamper.payload", payload, sizeof(payload)
    };
    void *blob = NULL;
    size_t blob_size = 0;
    build_secure_cooked(&input, 1, &blob, &blob_size);
    TEST_ASSERT_TRUE(flip_offset < blob_size);
    ((uint8_t *)blob)[flip_offset] ^= 0x80;

    JceArchive *ar = jce_archive_open(blob, blob_size);
    if (ar) {
        jce_archive_set_decryption_key(ar, k_test_key);
        TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_INVALID,
                              jce_archive_auth_status(ar));
        const JceArchiveEntry *entry = jce_archive_get(ar, 0);
        uint8_t decoded[1536];
        TEST_ASSERT_EQUAL_size_t(0, jce_archive_read(ar, entry, decoded,
                                                     sizeof(decoded)));
        jce_archive_close(ar);
    }
    jce_free(blob);
}

static void test_secure_archive_rejects_data_and_tag_tamper(void) {
    uint8_t payload[1536];
    fill_random(payload, sizeof(payload), 0xA117);
    const JceCookInput input = {
        "secure/tamper.payload", payload, sizeof(payload)
    };
    void *blob = NULL;
    size_t blob_size = 0;
    build_secure_cooked(&input, 1, &blob, &blob_size);

    JceArchive *ar = jce_archive_open(blob, blob_size);
    TEST_ASSERT_NOT_NULL(ar);
    jce_archive_set_decryption_key(ar, k_test_key);
    const JceArchiveEntry *entry = jce_archive_find(ar, input.vpath);
    TEST_ASSERT_NOT_NULL(entry);
    const size_t data_offset = (size_t)entry->data_offset;
    jce_archive_close(ar);
    jce_free(blob);

    assert_secure_tamper_rejected(data_offset + 12u);

    assert_secure_tamper_rejected(blob_size - 1u);
}

static void test_secure_archive_rejects_debug_paths_and_plain_entries(void) {
    uint8_t payload[64];
    fill_random(payload, sizeof(payload), 0xD06);
    JceArchiveWriterConfig cfg = {0};
    cfg.emit_debug_paths = true;
    JceArchiveWriter *w = jce_archive_writer_create(&cfg);
    TEST_ASSERT_NOT_NULL(w);
    jce_archive_writer_set_encryption_key(w, k_test_key);
    TEST_ASSERT_TRUE(jce_archive_writer_add_encrypted(
        w, "secret/one.bin", payload, sizeof(payload), -1));
    void *blob = NULL;
    size_t blob_size = 0;
    TEST_ASSERT_FALSE(jce_archive_writer_finish(w, &blob, &blob_size));
    jce_archive_writer_destroy(w);

    memset(&cfg, 0, sizeof(cfg));
    w = jce_archive_writer_create(&cfg);
    TEST_ASSERT_NOT_NULL(w);
    jce_archive_writer_set_encryption_key(w, k_test_key);
    TEST_ASSERT_TRUE(jce_archive_writer_add_encrypted(
        w, "secret/one.bin", payload, sizeof(payload), -1));
    TEST_ASSERT_TRUE(jce_archive_writer_add(
        w, "open/two.bin", payload, sizeof(payload)));
    TEST_ASSERT_FALSE(jce_archive_writer_finish(w, &blob, &blob_size));
    jce_archive_writer_destroy(w);
}

/* Build a one-entry encrypted archive with the given writer salt. */
static void build_encrypted_salted(uint32_t salt, const char *path,
                                   const void *data, size_t size,
                                   void **out_buf, size_t *out_sz) {
    JceArchiveWriterConfig cfg = {0};
    cfg.encryption_salt = salt;
    JceArchiveWriter *w = jce_archive_writer_create(&cfg);
    TEST_ASSERT_NOT_NULL(w);
    jce_archive_writer_set_encryption_key(w, k_test_key);
    TEST_ASSERT_TRUE(jce_archive_writer_add_encrypted(w, path, data, size, -1));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, out_buf, out_sz));
    jce_archive_writer_destroy(w);
    TEST_ASSERT_NOT_NULL(*out_buf);
}

static void test_encryption_salt_roundtrip(void) {
    uint8_t secret[2048];
    fill_random(secret, sizeof(secret), 0x5A17);

    void *b1 = NULL, *b2 = NULL; size_t s1 = 0, s2 = 0;
    build_encrypted_salted(0xDEADBEEFu, "data/secret.bin",
                           secret, sizeof(secret), &b1, &s1);
    build_encrypted_salted(0x12345678u, "data/secret.bin",
                           secret, sizeof(secret), &b2, &s2);

    /* The salt is persisted in the header's ex-RESERVED u32 at offset 60
     * (little-endian), so the reader can re-derive every nonce. */
    const uint8_t *h1 = (const uint8_t *)b1;
    uint32_t hdr_salt = (uint32_t)h1[60] | ((uint32_t)h1[61] << 8) |
                        ((uint32_t)h1[62] << 16) | ((uint32_t)h1[63] << 24);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, hdr_salt);

    /* Same key + path + bytes but different salts must yield different
     * ciphertext (the whole point: no cross-archive keystream reuse). */
    JceArchive *a1 = jce_archive_open(b1, s1);
    JceArchive *a2 = jce_archive_open(b2, s2);
    TEST_ASSERT_NOT_NULL(a1);
    TEST_ASSERT_NOT_NULL(a2);
    jce_archive_set_decryption_key(a1, k_test_key);
    jce_archive_set_decryption_key(a2, k_test_key);
    const JceArchiveEntry *e1 = jce_archive_find(a1, "data/secret.bin");
    const JceArchiveEntry *e2 = jce_archive_find(a2, "data/secret.bin");
    TEST_ASSERT_NOT_NULL(e1);
    TEST_ASSERT_NOT_NULL(e2);
    TEST_ASSERT_EQUAL_UINT32(e1->stored_size, e2->stored_size);
    TEST_ASSERT_TRUE(memcmp((const uint8_t *)b1 + e1->data_offset,
                            (const uint8_t *)b2 + e2->data_offset,
                            e1->stored_size) != 0);

    /* Both round-trip with the same key — the reader picks each archive's
     * salt out of its own header. */
    uint8_t out[2048];
    TEST_ASSERT_EQUAL_size_t(sizeof(secret),
                             jce_archive_read(a1, e1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(secret, out, sizeof(secret));
    TEST_ASSERT_EQUAL_size_t(sizeof(secret),
                             jce_archive_read(a2, e2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(secret, out, sizeof(secret));

    jce_archive_close(a1);
    jce_archive_close(a2);
    jce_free(b1);
    jce_free(b2);
}

/* ── encryption via the cook: distinct encrypt_label ⇒ distinct salt ──── */

static void test_cook_encrypt_labels_distinct_ciphertext(void) {
    uint8_t payload[1024];
    fill_random(payload, sizeof(payload), 0xC00C);

    JceCookInput in = { "shared/asset.bin", payload, sizeof(payload) };

    JceCookConfig cfg = {0};
    cfg.encrypt        = true;
    cfg.encryption_key = k_test_key;

    void *ba = NULL, *bb = NULL; size_t sa = 0, sb = 0;
    cfg.encrypt_label = "bundle_a";
    TEST_ASSERT_TRUE(jce_archive_cook(&in, 1, &cfg, &ba, &sa, NULL));
    cfg.encrypt_label = "bundle_b";
    TEST_ASSERT_TRUE(jce_archive_cook(&in, 1, &cfg, &bb, &sb, NULL));

    JceArchive *aa = jce_archive_open(ba, sa);
    JceArchive *ab = jce_archive_open(bb, sb);
    TEST_ASSERT_NOT_NULL(aa);
    TEST_ASSERT_NOT_NULL(ab);
    jce_archive_set_decryption_key(aa, k_test_key);
    jce_archive_set_decryption_key(ab, k_test_key);
    const JceArchiveEntry *ea = jce_archive_find(aa, "shared/asset.bin");
    const JceArchiveEntry *eb = jce_archive_find(ab, "shared/asset.bin");
    TEST_ASSERT_NOT_NULL(ea);
    TEST_ASSERT_NOT_NULL(eb);
    TEST_ASSERT_TRUE((ea->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) != 0);
    TEST_ASSERT_TRUE((eb->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) != 0);

    /* Same path + bytes + key, different labels ⇒ different ciphertext. */
    TEST_ASSERT_EQUAL_UINT32(ea->stored_size, eb->stored_size);
    TEST_ASSERT_TRUE(memcmp((const uint8_t *)ba + ea->data_offset,
                            (const uint8_t *)bb + eb->data_offset,
                            ea->stored_size) != 0);

    /* And both decrypt back to the same plaintext. */
    uint8_t out[1024];
    TEST_ASSERT_EQUAL_size_t(sizeof(payload),
                             jce_archive_read(aa, ea, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));
    TEST_ASSERT_EQUAL_size_t(sizeof(payload),
                             jce_archive_read(ab, eb, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));

    jce_archive_close(aa);
    jce_archive_close(ab);
    jce_free(ba);
    jce_free(bb);
}

/* ── process-wide key: auto-applied to encrypted archives on open ─────── */

static void test_process_key_auto_apply(void) {
    uint8_t secret[512];
    fill_random(secret, sizeof(secret), 0x9909);

    void *buf = NULL; size_t sz = 0;
    build_encrypted_salted(jce_archive_salt_from_label("project_assets"),
                           "cfg/locked.bin", secret, sizeof(secret),
                           &buf, &sz);

    uint8_t out[512];

    /* Install the process key BEFORE open: the archive inherits it and
     * reads decrypt without any per-archive set_decryption_key call. */
    jce_archive_set_process_key(k_test_key);
    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    const JceArchiveEntry *e = jce_archive_find(ar, "cfg/locked.bin");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_size_t(sizeof(secret),
                             jce_archive_read(ar, e, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(secret, out, sizeof(secret));
    jce_archive_close(ar);

    /* A wrong process key rejects the archive before its keyed index is used. */
    uint8_t wrong[32];
    memcpy(wrong, k_test_key, sizeof(wrong));
    wrong[31] ^= 0x80;
    jce_archive_set_process_key(wrong);
    ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NULL(ar);

    /* Without a process key metadata may open, but lookup/read stay closed
     * until a valid per-archive key authenticates the bytes. */
    jce_archive_set_process_key(NULL);
    ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    e = jce_archive_find(ar, "cfg/locked.bin");
    TEST_ASSERT_NULL(e);
    e = jce_archive_get(ar, 0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_read(ar, e, out, sizeof(out)));
    jce_archive_set_decryption_key(ar, k_test_key);
    e = jce_archive_find(ar, "cfg/locked.bin");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_size_t(sizeof(secret),
                             jce_archive_read(ar, e, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(secret, out, sizeof(secret));
    jce_archive_close(ar);

    jce_free(buf);
}

/* ── layered patch archives (spec §11.2) ─────────────────────────────── */

/* Build a small archive holding the given (path,bytes) pairs; returns the
 * malloc'd blob via *out_buf/*out_sz (caller frees) and the open archive. */
static JceArchive *build_simple(const char *p0, const char *v0,
                                const char *p1, const char *v1,
                                void **out_buf, size_t *out_sz) {
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, p0, v0, strlen(v0)));
    if (p1) TEST_ASSERT_TRUE(jce_archive_writer_add(w, p1, v1, strlen(v1)));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, out_buf, out_sz));
    jce_archive_writer_destroy(w);
    JceArchive *ar = jce_archive_open(*out_buf, *out_sz);
    TEST_ASSERT_NOT_NULL(ar);
    return ar;
}

static void test_patch_overlay(void) {
    /* Base ships two resources; the patch overrides one and adds a third. */
    void *bbuf = NULL, *pbuf = NULL; size_t bsz = 0, psz = 0;
    JceArchive *base  = build_simple("cfg/a.txt", "BASE-A",
                                     "cfg/b.txt", "BASE-B", &bbuf, &bsz);
    JceArchive *patch = build_simple("cfg/b.txt", "PATCH-B-OVERRIDE",
                                     "cfg/c.txt", "PATCH-C-NEW", &pbuf, &psz);

    JceArchiveMount *m = jce_archive_mount_create();
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_INT(1, jce_archive_mount_add(m, base));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_mount_add(m, patch));   /* patch wins */
    TEST_ASSERT_EQUAL_size_t(2, jce_archive_mount_layer_count(m));
    /* Re-adding is a no-op. */
    TEST_ASSERT_EQUAL_INT(1, jce_archive_mount_add(m, patch));
    TEST_ASSERT_EQUAL_size_t(2, jce_archive_mount_layer_count(m));

    char out[64];

    /* a.txt: only in base -> base value. */
    size_t na = jce_archive_mount_read(m, "cfg/a.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(6, na);
    TEST_ASSERT_EQUAL_MEMORY("BASE-A", out, 6);

    /* b.txt: in both -> patch overrides base. */
    JceArchive *owner = NULL;
    const JceArchiveEntry *eb = jce_archive_mount_find(m, "cfg/b.txt", &owner);
    TEST_ASSERT_NOT_NULL(eb);
    TEST_ASSERT_EQUAL_PTR(patch, owner);
    size_t nb = jce_archive_mount_read(m, "cfg/b.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(16, nb);
    TEST_ASSERT_EQUAL_MEMORY("PATCH-B-OVERRIDE", out, 16);

    /* c.txt: only in patch. */
    size_t nc = jce_archive_mount_read(m, "cfg/c.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(11, nc);
    TEST_ASSERT_EQUAL_MEMORY("PATCH-C-NEW", out, 11);

    /* Missing path -> 0 and NULL find. */
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_mount_read(m, "cfg/missing", out, sizeof(out)));
    TEST_ASSERT_NULL(jce_archive_mount_find(m, "cfg/missing", &owner));
    TEST_ASSERT_NULL(owner);

    /* Removing the patch falls back to the base copy of b.txt. */
    jce_archive_mount_remove(m, patch);
    TEST_ASSERT_EQUAL_size_t(1, jce_archive_mount_layer_count(m));
    size_t nb2 = jce_archive_mount_read(m, "cfg/b.txt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(6, nb2);
    TEST_ASSERT_EQUAL_MEMORY("BASE-B", out, 6);
    /* c.txt now gone (was patch-only). */
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_mount_read(m, "cfg/c.txt", out, sizeof(out)));

    jce_archive_mount_destroy(m);
    jce_archive_close(base);
    jce_archive_close(patch);
    jce_free(bbuf);
    jce_free(pbuf);
}

/* ── binary delta patches (spec §11.3) ───────────────────────────────── */

static void test_delta_roundtrip(void) {
    /* Build an "old" archive and a "new" archive that shares most content. */
    JceArchiveWriter *w0 = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w0);
    uint8_t big[16384];
    memset(big, 'Q', sizeof(big));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w0, "world/level1.dat", big, sizeof(big)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w0, "cfg/keep.txt", "unchanged", 9));
    void *oldb = NULL; size_t olds = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w0, &oldb, &olds));
    jce_archive_writer_destroy(w0);

    JceArchiveWriter *w1 = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w1);
    big[0] = 'X'; big[10000] = 'Y';   /* tiny change to the large resource */
    TEST_ASSERT_TRUE(jce_archive_writer_add(w1, "world/level1.dat", big, sizeof(big)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w1, "cfg/keep.txt", "unchanged", 9));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w1, "cfg/added.txt", "brand new", 9));
    void *newb = NULL; size_t news = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w1, &newb, &news));
    jce_archive_writer_destroy(w1);

    /* Create the delta and reconstruct the new archive from old + delta. */
    void *delta = NULL; size_t dsz = 0;
    TEST_ASSERT_TRUE(jce_archive_delta_create(oldb, olds, newb, news, 19, &delta, &dsz) > 0);
    TEST_ASSERT_NOT_NULL(delta);

    void *recon = NULL; size_t rsz = 0;
    TEST_ASSERT_EQUAL_size_t(news,
        jce_archive_delta_apply(oldb, olds, delta, dsz, &recon, &rsz));
    TEST_ASSERT_EQUAL_size_t(news, rsz);
    TEST_ASSERT_EQUAL_MEMORY(newb, recon, news);

    /* The reconstructed bytes open and resolve the added resource. */
    JceArchive *ar = jce_archive_open(recon, rsz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_NOT_NULL(jce_archive_find(ar, "cfg/added.txt"));
    jce_archive_close(ar);

    /* Applying against the wrong base must be rejected (identity guard). */
    uint8_t *wrong = (uint8_t *)jce_malloc(olds);
    memcpy(wrong, oldb, olds);
    wrong[olds / 2] ^= 0xFF;
    void *bad = NULL; size_t badsz = 0;
    TEST_ASSERT_EQUAL_size_t(0,
        jce_archive_delta_apply(wrong, olds, delta, dsz, &bad, &badsz));
    jce_free(wrong);

    jce_free(recon);
    jce_free(delta);
    jce_free(oldb);
    jce_free(newb);
}

/* ── §13/§14 runtime loader: refcounted cache + sync acquire ──────────── */
static void test_loader_acquire_cache(void) {
    void *buf = NULL; size_t sz = 0;
    /* A compressible payload so it is stored zstd (exercises the decode path). */
    char big[4096];
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = (char)('a' + (i % 7));

    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "cfg/a.txt", "HELLO-A", 7));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "data/big.bin", big, sizeof(big)));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);

    JceArchiveLoaderConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.verify_crc = 1;                 /* exercise CRC verification */
    JceArchiveLoader *ld = jce_archive_loader_create(ar, &cfg);
    TEST_ASSERT_NOT_NULL(ld);

    /* acquire content matches the original bytes */
    const JceArchiveResource *ra = jce_archive_loader_acquire(ld, "cfg/a.txt");
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_EQUAL_size_t(7, ra->size);
    TEST_ASSERT_EQUAL_MEMORY("HELLO-A", ra->data, 7);

    /* second acquire returns the SAME cached pointer (load once) */
    const JceArchiveResource *ra2 = jce_archive_loader_acquire(ld, "cfg/a.txt");
    TEST_ASSERT_EQUAL_PTR(ra, ra2);
    TEST_ASSERT_EQUAL_UINT32(1, jce_archive_loader_cache_count(ld));

    const JceArchiveResource *rb = jce_archive_loader_acquire(ld, "data/big.bin");
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL_size_t(sizeof(big), rb->size);
    TEST_ASSERT_EQUAL_MEMORY(big, rb->data, sizeof(big));
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_loader_cache_count(ld));

    /* missing resource -> NULL (absence is not an error) */
    TEST_ASSERT_NULL(jce_archive_loader_acquire(ld, "nope/missing.dat"));

    jce_archive_loader_release(ld, ra);
    jce_archive_loader_release(ld, ra2);
    jce_archive_loader_release(ld, rb);

    jce_archive_loader_destroy(ld);
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── §14.1 inline async path: request -> tick -> poll, plus LRU eviction ── */
static void test_loader_async_and_eviction(void) {
    void *buf = NULL; size_t sz = 0;
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    /* Three ~1 KB stored-incompressible payloads, distinct content. */
    char p[3][1024];
    const char *paths[3] = { "r/0.bin", "r/1.bin", "r/2.bin" };
    for (int k = 0; k < 3; ++k) {
        for (size_t i = 0; i < sizeof(p[k]); ++i) p[k][i] = (char)((i * 31 + k * 7) & 0xff);
        TEST_ASSERT_TRUE(jce_archive_writer_add(w, paths[k], p[k], sizeof(p[k])));
    }
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);

    JceArchiveLoaderConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = 0;                 /* inline, frame-budgeted path */
    cfg.cache_budget_bytes = 2048;        /* room for ~2 of the 1 KB buffers */
    cfg.frame_budget_ms = 1000.0;         /* don't defer in the test */
    JceArchiveLoader *ld = jce_archive_loader_create(ar, &cfg);
    TEST_ASSERT_NOT_NULL(ld);

    JceArchiveRequestId id = jce_archive_loader_request(ld, "r/0.bin");
    TEST_ASSERT_NOT_EQUAL(0, id);

    /* before tick: pending, poll returns 0 */
    const JceArchiveResource *r0 = NULL;
    TEST_ASSERT_EQUAL_INT(0, jce_archive_loader_poll(ld, id, &r0));
    TEST_ASSERT_EQUAL_UINT32(1, jce_archive_loader_pending_count(ld));

    jce_archive_loader_tick(ld);          /* services the queued load */

    TEST_ASSERT_EQUAL_INT(1, jce_archive_loader_poll(ld, id, &r0));
    TEST_ASSERT_NOT_NULL(r0);
    TEST_ASSERT_EQUAL_size_t(1024, r0->size);
    TEST_ASSERT_EQUAL_MEMORY(p[0], r0->data, 1024);

    /* request for an absent resource -> id 0 */
    TEST_ASSERT_EQUAL_UINT64(0, jce_archive_loader_request(ld, "r/none.bin"));

    /* Release the held reference so it becomes evictable, then load two more
     * to push past the 2 KB budget and trigger LRU eviction. */
    jce_archive_loader_release(ld, r0);

    const JceArchiveResource *a1 = jce_archive_loader_acquire(ld, "r/1.bin");
    const JceArchiveResource *a2 = jce_archive_loader_acquire(ld, "r/2.bin");
    TEST_ASSERT_NOT_NULL(a1);
    TEST_ASSERT_NOT_NULL(a2);
    /* r/0 was unreferenced and least-recently-used -> evicted under budget. */
    TEST_ASSERT_TRUE(jce_archive_loader_cache_bytes(ld) <= 2048);
    TEST_ASSERT_EQUAL_UINT32(2, jce_archive_loader_cache_count(ld));

    jce_archive_loader_release(ld, a1);
    jce_archive_loader_release(ld, a2);
    jce_archive_loader_destroy(ld);
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── §14.1 worker-thread path: concurrent loads via the job pool ────────── */
static void test_loader_worker_threads(void) {
    enum { N = 16 };
    void *buf = NULL; size_t sz = 0;
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    char payload[N][512];
    char path[N][32];
    for (int k = 0; k < N; ++k) {
        for (size_t i = 0; i < sizeof(payload[k]); ++i)
            payload[k][i] = (char)((i * 13 + k * 101) & 0xff);
        snprintf(path[k], sizeof(path[k]), "blob/%02d.bin", k);
        TEST_ASSERT_TRUE(jce_archive_writer_add(w, path[k], payload[k], sizeof(payload[k])));
    }
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);

    JceArchiveLoaderConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = 3;        /* threaded async path */
    cfg.verify_crc = 1;
    JceArchiveLoader *ld = jce_archive_loader_create(ar, &cfg);
    TEST_ASSERT_NOT_NULL(ld);

    JceArchiveRequestId ids[N];
    for (int k = 0; k < N; ++k) {
        ids[k] = jce_archive_loader_request(ld, path[k]);
        TEST_ASSERT_NOT_EQUAL(0, ids[k]);
    }

    /* Drive ticks until every request resolves (workers do the I/O). */
    /* BOUNDED BY TIME AND YIELDING, not by a spin count.
     *
     * This waited 100000 spins with no yield, on a thread that pins a core
     * while the three WORKER threads it is waiting for compete for the rest.
     * Alone that is fine; under `ctest -j 8` -- the invocation this project's
     * own closing sequence prescribes -- eight such processes starve each
     * other and the budget expires with requests still in flight.  Measured:
     * this test failed in roughly a third of parallel runs and never once in
     * isolation, and it was not alone (14 different tests named across 20
     * parallel runs).  A spin count measures the CPU share this process
     * happened to get; what the test means to assert is that the loader
     * resolves every request. */
    const JceArchiveResource *res[N] = { 0 };
    int remaining = N;
    const uint64_t wait_t0 = jce_time_perf_counter();
    while (remaining > 0 &&
           jce_time_perf_to_ms(wait_t0, jce_time_perf_counter()) < 30000.0) {
        jce_archive_loader_tick(ld);
        jce_thread_sleep_ms(0);   /* let the workers run */
        for (int k = 0; k < N; ++k) {
            if (res[k]) continue;
            int rc = jce_archive_loader_poll(ld, ids[k], &res[k]);
            if (rc == 1) { TEST_ASSERT_NOT_NULL(res[k]); remaining--; }
            else if (rc < 0) { TEST_FAIL_MESSAGE("worker load failed"); }
        }
    }
    TEST_ASSERT_EQUAL_INT(0, remaining);

    for (int k = 0; k < N; ++k) {
        TEST_ASSERT_EQUAL_size_t(512, res[k]->size);
        TEST_ASSERT_EQUAL_MEMORY(payload[k], res[k]->data, 512);
        jce_archive_loader_release(ld, res[k]);
    }

    jce_archive_loader_destroy(ld);   /* must drain any in-flight loads */
    jce_archive_close(ar);
    jce_free(buf);
}

/* ── §4.2 / §10.2 / §16 explicit little-endian on-disk layout ──────────── *
 * Verifies fields land at the exact documented byte offsets in little-endian,
 * independent of host byte order (the reader assembles integers byte-by-byte,
 * so this also validates the cross-endian read contract of §10.1). */
static void test_explicit_offset_layout(void) {
    void *buf = NULL; size_t sz = 0;
    JceArchive *ar = build_simple("a/one.txt", "ONE",
                                  "b/two.txt", "TWO", &buf, &sz);
    jce_archive_close(ar);

    const uint8_t *b = (const uint8_t *)buf;
    TEST_ASSERT_TRUE(sz >= 64);

    /* magic "JPAK" at offset 0 */
    TEST_ASSERT_EQUAL_UINT8('J', b[0]);
    TEST_ASSERT_EQUAL_UINT8('P', b[1]);
    TEST_ASSERT_EQUAL_UINT8('A', b[2]);
    TEST_ASSERT_EQUAL_UINT8('K', b[3]);

    /* format_version (u32 LE) == 1 at offset 4 */
    uint32_t ver = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                   ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    TEST_ASSERT_EQUAL_UINT32(1u, ver);

    /* entry_count (u32 LE) == 2 at offset 12 */
    uint32_t ec = (uint32_t)b[12] | ((uint32_t)b[13] << 8) |
                  ((uint32_t)b[14] << 16) | ((uint32_t)b[15] << 24);
    TEST_ASSERT_EQUAL_UINT32(2u, ec);

    /* index_offset (u64 LE) at offset 16 must be within the file */
    uint64_t ioff = 0;
    for (int i = 0; i < 8; ++i) ioff |= (uint64_t)b[16 + i] << (8 * i);
    TEST_ASSERT_TRUE(ioff >= 64 && ioff <= sz);

    /* dict_count (u16 LE) == 0 at offset 56 (no dictionaries here) */
    uint16_t dc = (uint16_t)(b[56] | (b[57] << 8));
    TEST_ASSERT_EQUAL_UINT16(0u, dc);

    /* reserved (u32) at offset 60 must be zero (spec §4.2) */
    TEST_ASSERT_EQUAL_UINT8(0, b[60]);
    TEST_ASSERT_EQUAL_UINT8(0, b[61]);
    TEST_ASSERT_EQUAL_UINT8(0, b[62]);
    TEST_ASSERT_EQUAL_UINT8(0, b[63]);

    jce_free(buf);
}

/* ── §15 / §16 large-archive scale: binary-search lookup at 20k entries ─── */
static void test_large_archive(void) {
    enum { N = 20000 };
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);

    char path[40], val[24];
    for (int i = 0; i < N; ++i) {
        snprintf(path, sizeof(path), "assets/group%03d/item%05d.dat", i % 256, i);
        int vn = snprintf(val, sizeof(val), "payload-%d", i);
        TEST_ASSERT_TRUE(jce_archive_writer_add(w, path, val, (size_t)vn));
    }

    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)N, jce_archive_count(ar));

    /* Spot-check lookups across the range resolve to the right bytes. */
    for (int s = 0; s < N; s += 1237) {
        snprintf(path, sizeof(path), "assets/group%03d/item%05d.dat", s % 256, s);
        int vn = snprintf(val, sizeof(val), "payload-%d", s);
        const JceArchiveEntry *e = jce_archive_find(ar, path);
        TEST_ASSERT_NOT_NULL(e);
        char out[24];
        size_t n = jce_archive_read(ar, e, out, sizeof(out));
        TEST_ASSERT_EQUAL_size_t((size_t)vn, n);
        TEST_ASSERT_EQUAL_MEMORY(val, out, (size_t)vn);
    }

    /* An absent path returns NULL (clean miss, not an error). */
    TEST_ASSERT_NULL(jce_archive_find(ar, "assets/group000/itemZZZZZ.dat"));

    jce_archive_close(ar);
    jce_free(buf);
}

/* ── §11.1 / §16 incremental build: precompressed reuse is byte-identical ─ *
 * Builds an archive normally, then rebuilds it by reusing each resource's
 * compressed blob via compress_resource()+add_precompressed() (the build-cache
 * reuse path), and asserts the two archives are byte-for-byte identical — the
 * determinism property incremental builds depend upon (§10.5). */
static void test_incremental_reuse(void) {
    const char *paths[3] = { "cfg/world.json", "text/dialogue.txt", "bin/mesh.dat" };
    /* Compressible payloads so the reuse path actually carries zstd blobs. */
    char pj[800], pt[600], pb[1000];
    for (size_t i = 0; i < sizeof(pj); ++i) pj[i] = (char)('{' + (i % 5));
    for (size_t i = 0; i < sizeof(pt); ++i) pt[i] = (char)('A' + (i % 9));
    for (size_t i = 0; i < sizeof(pb); ++i) pb[i] = (char)(i % 17);
    const void *data[3] = { pj, pt, pb };
    size_t lens[3] = { sizeof(pj), sizeof(pt), sizeof(pb) };

    /* Reference build. */
    void *refbuf = NULL; size_t refsz = 0;
    JceArchiveWriter *w0 = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w0);
    for (int i = 0; i < 3; ++i)
        TEST_ASSERT_TRUE(jce_archive_writer_add(w0, paths[i], data[i], lens[i]));
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w0, &refbuf, &refsz));
    jce_archive_writer_destroy(w0);

    /* Reuse build: compress each resource separately, then add precompressed. */
    void *rbuf = NULL; size_t rsz = 0;
    JceArchiveWriter *w1 = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w1);
    for (int i = 0; i < 3; ++i) {
        JceArchiveBlob blob;
        memset(&blob, 0, sizeof(blob));
        TEST_ASSERT_EQUAL_INT(1,
            jce_archive_compress_resource(w1, data[i], lens[i], -1, &blob));
        TEST_ASSERT_TRUE(jce_archive_writer_add_precompressed(w1, paths[i], &blob));
        jce_free(blob.bytes);
    }
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w1, &rbuf, &rsz));
    jce_archive_writer_destroy(w1);

    /* Byte-identical: incremental reuse must not change the output. */
    TEST_ASSERT_EQUAL_size_t(refsz, rsz);
    TEST_ASSERT_EQUAL_MEMORY(refbuf, rbuf, refsz);

    jce_free(refbuf);
    jce_free(rbuf);
}

/* ── content dedup: byte-identical payloads share one on-disk copy ────── */
static void test_content_dedup(void) {
    JceArchiveWriterConfig cfg = {0};
    cfg.dedup_content = true;

    /* Two distinct paths with identical, compressible content; one unique. */
    uint8_t same[4096];
    memset(same, 'Z', sizeof(same));
    uint8_t uniq[2048];
    fill_random(uniq, sizeof(uniq), 0xBADF00D);

    JceArchiveWriter *w = jce_archive_writer_create(&cfg);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "a/copy1.bin", same, sizeof(same)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "b/copy2.bin", same, sizeof(same)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "c/uniq.bin", uniq, sizeof(uniq)));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);

    JceArchive *ar = jce_archive_open(buf, sz);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT32(3, jce_archive_count(ar));
    TEST_ASSERT_EQUAL_INT(1, jce_archive_verify_header(ar));

    const JceArchiveEntry *e1 = jce_archive_find(ar, "a/copy1.bin");
    const JceArchiveEntry *e2 = jce_archive_find(ar, "b/copy2.bin");
    const JceArchiveEntry *e3 = jce_archive_find(ar, "c/uniq.bin");
    TEST_ASSERT_NOT_NULL(e1);
    TEST_ASSERT_NOT_NULL(e2);
    TEST_ASSERT_NOT_NULL(e3);

    /* The two identical entries must share one data_offset; the unique one
     * must not collide with them. */
    TEST_ASSERT_EQUAL_UINT64(e1->data_offset, e2->data_offset);
    TEST_ASSERT_TRUE(e3->data_offset != e1->data_offset);

    /* All three still read back their original content correctly. */
    uint8_t *out = (uint8_t *)jce_malloc(4096);
    TEST_ASSERT_EQUAL_size_t(sizeof(same), jce_archive_read(ar, e1, out, 4096));
    TEST_ASSERT_EQUAL_MEMORY(same, out, sizeof(same));
    TEST_ASSERT_EQUAL_size_t(sizeof(same), jce_archive_read(ar, e2, out, 4096));
    TEST_ASSERT_EQUAL_MEMORY(same, out, sizeof(same));
    TEST_ASSERT_EQUAL_size_t(sizeof(uniq), jce_archive_read(ar, e3, out, 4096));
    TEST_ASSERT_EQUAL_MEMORY(uniq, out, sizeof(uniq));
    jce_free(out);
    jce_archive_close(ar);

    /* Dedup must shrink the archive vs. the same inputs without dedup. */
    JceArchiveWriter *wn = jce_archive_writer_create(NULL); /* dedup off */
    TEST_ASSERT_NOT_NULL(wn);
    TEST_ASSERT_TRUE(jce_archive_writer_add(wn, "a/copy1.bin", same, sizeof(same)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(wn, "b/copy2.bin", same, sizeof(same)));
    TEST_ASSERT_TRUE(jce_archive_writer_add(wn, "c/uniq.bin", uniq, sizeof(uniq)));
    void *nbuf = NULL; size_t nsz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(wn, &nbuf, &nsz));
    jce_archive_writer_destroy(wn);
    TEST_ASSERT_TRUE(sz < nsz);

    jce_free(buf);
    jce_free(nbuf);
}

/* ── opt-in verify-on-open (default off) ─────────────────────────────── */

static void test_verify_on_open(void) {
    uint8_t comp[1024];
    memset(comp, 'V', sizeof(comp));
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    jce_archive_writer_add(w, "x.dat", comp, sizeof(comp));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    uint8_t *bytes = (uint8_t *)buf;

    /* Corrupt a data-region byte (just past the 64-byte header). */
    bytes[64] ^= 0xFF;

    /* Default (off): a corrupted archive still opens — no integrity gate,
     * zero behaviour change vs. before this feature. */
    jce_archive_set_verify_on_open(false);
    JceArchive *ar = jce_archive_open(bytes, sz);
    TEST_ASSERT_NOT_NULL(ar);
    jce_archive_close(ar);

    /* Opt-in: open must reject the tampered/corrupted archive. */
    jce_archive_set_verify_on_open(true);
    TEST_ASSERT_NULL(jce_archive_open(bytes, sz));

    /* A pristine archive still opens with verify on. */
    bytes[64] ^= 0xFF; /* restore */
    ar = jce_archive_open(bytes, sz);
    TEST_ASSERT_NOT_NULL(ar);
    jce_archive_close(ar);

    jce_archive_set_verify_on_open(false);
    jce_free(buf);
}

/* ── script sources share the TEXT dictionary (every language) ───────── *
 *
 * The compression class is chosen by extension in jce_archive_cook.c, and
 * ".lua" used to be spelled into that list by hand — so .py and .java were
 * compressed as opaque binary while .lua shared the text dictionary.  The
 * class now comes from jce_asset_script_form_from_ext(), which is also the
 * only place that distinguishes SOURCE from BYTECODE, so both halves are
 * asserted here: a corpus of Python sources trains a TEXT dictionary, and a
 * corpus of Java .class blobs trains none.
 */
static void cook_corpus(const char *ext, void **out_buf, size_t *out_size,
                        uint16_t *out_dicts) {
    /* 4000 x ~65 B ~= 260 KB, comfortably past ZDICT's 10x-the-dictionary
     * advisory for the 16 KiB floor jce_archive_cook clamps to. */
    enum { NSAMP = 4000 };
    JceCookInput *in = (JceCookInput *)jce_malloc(sizeof(JceCookInput) * NSAMP);
    char (*paths)[64] = (char (*)[64])jce_malloc(64 * NSAMP);
    char (*bodies)[160] = (char (*)[160])jce_malloc(160 * NSAMP);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(paths);
    TEST_ASSERT_NOT_NULL(bodies);

    for (int i = 0; i < NSAMP; ++i) {
        snprintf(paths[i], 64, "scripts/turret_%04d.%s", i, ext);
        int n = snprintf(bodies[i], 160,
            "def on_update(entity, dt):\n"
            "    aim(entity, %d)\n"
            "    fire(entity, %d)\n", i, i * 7);
        in[i].vpath = paths[i];
        in[i].data  = bodies[i];
        in[i].size  = (size_t)n;
    }

    JceCookConfig cfg = {0};
    cfg.zstd_level = 1;
    cfg.use_dict   = true;
    TEST_ASSERT_TRUE(jce_archive_cook(in, NSAMP, &cfg, out_buf, out_size,
                                      out_dicts));
    jce_free(bodies);
    jce_free(paths);
    jce_free(in);
}

static void test_script_sources_share_the_text_dictionary(void) {
    void    *buf   = NULL;
    size_t   size  = 0;
    uint16_t dicts = 0;

    /* Python source: SOURCE form -> the shared TEXT class. */
    cook_corpus("py", &buf, &size, &dicts);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1, dicts,
        "a .py corpus must train exactly one class dictionary");
    JceArchive *ar = jce_archive_open(buf, size);
    TEST_ASSERT_NOT_NULL(ar);
    TEST_ASSERT_EQUAL_UINT16(1, jce_archive_dict_count(ar));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(JCE_ARCHIVE_TAG('T','E','X','T'),
        jce_archive_dict_tag(ar, 0),
        "script SOURCE belongs to the TEXT class, not a class of its own");
    jce_archive_close(ar);
    jce_free(buf);

    /* Java bytecode: BYTECODE form -> no text dictionary.  This is the half
     * that makes `form` an enum instead of a bool. */
    buf = NULL; size = 0; dicts = 0;
    cook_corpus("class", &buf, &size, &dicts);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, dicts,
        "compiled bytecode must not be fed to the text dictionary trainer");
    jce_free(buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crypto_hmac_sha256_rfc4231);
    RUN_TEST(test_crypto_chacha20_rfc8439);
    RUN_TEST(test_roundtrip_stored_and_zstd);
    RUN_TEST(test_normalization);
    RUN_TEST(test_determinism);
    RUN_TEST(test_corruption_detected);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_debug_paths);
    RUN_TEST(test_dictionary_roundtrip);
    RUN_TEST(test_script_sources_share_the_text_dictionary);
    RUN_TEST(test_mmap_zero_copy);
    RUN_TEST(test_encryption_roundtrip);
    RUN_TEST(test_secure_cook_has_no_plaintext_dictionary);
    RUN_TEST(test_secure_archive_roundtrip_and_keyed_index);
    RUN_TEST(test_secure_archive_rejects_data_and_tag_tamper);
    RUN_TEST(test_secure_archive_rejects_debug_paths_and_plain_entries);
    RUN_TEST(test_encryption_salt_roundtrip);
    RUN_TEST(test_cook_encrypt_labels_distinct_ciphertext);
    RUN_TEST(test_process_key_auto_apply);
    RUN_TEST(test_patch_overlay);
    RUN_TEST(test_delta_roundtrip);
    RUN_TEST(test_loader_acquire_cache);
    RUN_TEST(test_loader_async_and_eviction);
    RUN_TEST(test_loader_worker_threads);
    RUN_TEST(test_explicit_offset_layout);
    RUN_TEST(test_large_archive);
    RUN_TEST(test_incremental_reuse);
    RUN_TEST(test_content_dedup);
    RUN_TEST(test_open_rejects_garbage);
    RUN_TEST(test_verify_on_open);
    return UNITY_END();
}
