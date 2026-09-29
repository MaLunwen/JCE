/*
 * test_jce_cook_incremental.c  Per-asset incremental cook cache (Feature 0.4).
 *
 * Exercises the REAL catalog code path from tools/jce_cook_catalog.c (the
 * functions the --batch cooker calls to skip unchanged assets) — not a mock.
 *
 *   - jce_cook_hash_buffer  : content fingerprint of an in-memory buffer
 *   - jce_cook_hash_file    : fingerprint of a source file (+ import sidecar)
 *   - jce_cook_catalog_*    : record / lookup / up-to-date predicate
 *   - load / save round-trip
 *
 * The contract under test: a brand-new path is NEVER up-to-date; after a
 * record it IS; mutating one byte changes the hash and makes it stale again.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "unity.h"

#include "jce_cook_catalog.h"

#include <jce/os/core/jce_filesystem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char g_root[1024];

void setUp(void)
{
    static int counter;
    char base[1024];

    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_root, sizeof(g_root), "%s/_ut_cook_incr_%d_%d", base,
             (int)(uintptr_t)setUp & 0xFFFF, ++counter);
    (void)jce_fs_host_remove_recursive(g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
}

void tearDown(void)
{
    (void)jce_fs_host_remove_recursive(g_root);
}

static void join_path(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

/* ── jce_cook_hash_buffer: deterministic, content-sensitive ─────────────── */

static void test_hash_buffer_is_deterministic_and_sensitive(void)
{
    const char a[] = "the quick brown fox";
    const char b[] = "the quick brown box"; /* one byte differs */

    uint64_t ha1 = jce_cook_hash_buffer(a, sizeof(a) - 1);
    uint64_t ha2 = jce_cook_hash_buffer(a, sizeof(a) - 1);
    uint64_t hb  = jce_cook_hash_buffer(b, sizeof(b) - 1);

    TEST_ASSERT_EQUAL_UINT64(ha1, ha2);          /* deterministic */
    TEST_ASSERT_NOT_EQUAL_UINT64(ha1, hb);       /* content-sensitive */

    /* Empty buffer is well-defined and stable (no NULL-deref UB). */
    uint64_t he1 = jce_cook_hash_buffer(NULL, 0);
    uint64_t he2 = jce_cook_hash_buffer("", 0);
    TEST_ASSERT_EQUAL_UINT64(he1, he2);
}

/* ── core predicate: new entry FALSE, recorded TRUE, mutated FALSE ──────── */

static void test_predicate_lifecycle(void)
{
    JceCookCatalog cat;
    jce_cook_catalog_init(&cat);

    const char fixture[] = "fixture-bytes-v1";
    uint64_t   h1 = jce_cook_hash_buffer(fixture, sizeof(fixture) - 1);

    /* A path the catalog has never seen is NOT up to date — it must cook. */
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&cat, "tex/hero.png", h1));
    TEST_ASSERT_NULL(jce_cook_catalog_find(&cat, "tex/hero.png"));

    /* After recording, the same hash IS up to date — skip the cook. */
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "tex/hero.png", h1, 16, 123));
    TEST_ASSERT_TRUE(jce_cook_entry_is_up_to_date(&cat, "tex/hero.png", h1));

    const JceCookCatalogEntry *e = jce_cook_catalog_find(&cat, "tex/hero.png");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT64(h1, e->hash);
    TEST_ASSERT_EQUAL_UINT64(16, e->size);
    TEST_ASSERT_EQUAL_INT64(123, e->mtime);

    /* Mutate one byte: new hash differs, so the recorded entry is now stale. */
    char mutated[sizeof(fixture)];
    memcpy(mutated, fixture, sizeof(fixture));
    mutated[0] = (char)(mutated[0] ^ 0x01);
    uint64_t h2 = jce_cook_hash_buffer(mutated, sizeof(mutated) - 1);

    TEST_ASSERT_NOT_EQUAL_UINT64(h1, h2);
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&cat, "tex/hero.png", h2));

    /* Re-record with the new hash: up to date again (overwrite, not insert). */
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "tex/hero.png", h2, 16, 124));
    TEST_ASSERT_TRUE(jce_cook_entry_is_up_to_date(&cat, "tex/hero.png", h2));
    TEST_ASSERT_EQUAL_size_t(1, cat.count); /* overwrote, did not duplicate */

    jce_cook_catalog_free(&cat);
}

/* ── jce_cook_hash_file: real file + import sidecar fold ─────────────────── */

static void test_hash_file_folds_source_and_sidecar(void)
{
    char src[1024], sidecar[1024];
    join_path(src, sizeof(src), "asset.bin");
    join_path(sidecar, sizeof(sidecar), "asset.bin.import.json");

    const char v1[] = "AAAA-source-content";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(src, v1, sizeof(v1) - 1));

    bool ok = false;
    uint64_t fsize = 0; int64_t fmt = 0;
    uint64_t h_src = jce_cook_hash_file(src, 0u, &fsize, &fmt, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(sizeof(v1) - 1), fsize);

    /* Adding an import sidecar must change the fingerprint even though the
       source bytes are identical (preset edits invalidate the cache). */
    const char preset[] = "{\"target_format\":\"bc7\"}";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sidecar, preset, sizeof(preset) - 1));
    uint64_t h_with_sidecar = jce_cook_hash_file(src, 0u, NULL, NULL, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_EQUAL_UINT64(h_src, h_with_sidecar);

    /* Changing only the salt (cook options) also busts the cache. */
    uint64_t h_salted = jce_cook_hash_file(src, 0xDEADBEEFu, NULL, NULL, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_EQUAL_UINT64(h_with_sidecar, h_salted);

    /* A missing source reports failure (ok=false) — never a false cache hit. */
    char missing[1024];
    join_path(missing, sizeof(missing), "does_not_exist.bin");
    (void)jce_cook_hash_file(missing, 0u, NULL, NULL, &ok);
    TEST_ASSERT_FALSE(ok);
}

/* ── end-to-end skip decision via the real source-file hash ─────────────── */

static void test_file_change_flips_up_to_date(void)
{
    char src[1024];
    join_path(src, sizeof(src), "tex.raw");

    const char v1[] = "pixels-version-1";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(src, v1, sizeof(v1) - 1));

    JceCookCatalog cat;
    jce_cook_catalog_init(&cat);

    bool ok = false;
    uint64_t size = 0; int64_t mt = 0;
    uint64_t h1 = jce_cook_hash_file(src, 7u, &size, &mt, &ok);
    TEST_ASSERT_TRUE(ok);

    /* First cook: not cached -> must process, then record. */
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&cat, "tex.raw", h1));
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "tex.raw", h1, size, mt));
    TEST_ASSERT_TRUE(jce_cook_entry_is_up_to_date(&cat, "tex.raw", h1));

    /* Overwrite the source with new bytes: re-hash -> stale. */
    const char v2[] = "pixels-version-2!!";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(src, v2, sizeof(v2) - 1));
    uint64_t h2 = jce_cook_hash_file(src, 7u, NULL, NULL, &ok);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_EQUAL_UINT64(h1, h2);
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&cat, "tex.raw", h2));

    jce_cook_catalog_free(&cat);
}

/* ── persistence round-trip ─────────────────────────────────────────────── */

static void test_catalog_save_load_round_trip(void)
{
    char cat_path[1024];
    join_path(cat_path, sizeof(cat_path), "cook_catalog");

    JceCookCatalog out;
    jce_cook_catalog_init(&out);
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&out, "a/one.png",   0x0123456789ABCDEFull, 11, 1000));
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&out, "b/two.wav",   0xFEDCBA9876543210ull, 22, 2000));
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&out, "c/three.bin", 0u,                    0,  0));
    TEST_ASSERT_TRUE(jce_cook_catalog_save(&out, cat_path));
    jce_cook_catalog_free(&out);

    JceCookCatalog in;
    TEST_ASSERT_TRUE(jce_cook_catalog_load(&in, cat_path));
    TEST_ASSERT_EQUAL_size_t(3, in.count);

    const JceCookCatalogEntry *e1 = jce_cook_catalog_find(&in, "a/one.png");
    const JceCookCatalogEntry *e2 = jce_cook_catalog_find(&in, "b/two.wav");
    const JceCookCatalogEntry *e3 = jce_cook_catalog_find(&in, "c/three.bin");
    TEST_ASSERT_NOT_NULL(e1);
    TEST_ASSERT_NOT_NULL(e2);
    TEST_ASSERT_NOT_NULL(e3);
    TEST_ASSERT_EQUAL_HEX64(0x0123456789ABCDEFull, e1->hash);
    TEST_ASSERT_EQUAL_UINT64(11, e1->size);
    TEST_ASSERT_EQUAL_INT64(1000, e1->mtime);
    TEST_ASSERT_EQUAL_HEX64(0xFEDCBA9876543210ull, e2->hash);
    TEST_ASSERT_EQUAL_HEX64(0u, e3->hash);

    /* The reloaded catalog drives the same skip decision. */
    TEST_ASSERT_TRUE(jce_cook_entry_is_up_to_date(&in, "a/one.png",
                                                  0x0123456789ABCDEFull));
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&in, "a/one.png", 1u));

    jce_cook_catalog_free(&in);
}

/* ── per-run stale GC: begin_epoch + sweep_unseen drop vanished sources ──── */

static void test_sweep_unseen_prunes_stale_entries(void)
{
    JceCookCatalog cat;
    jce_cook_catalog_init(&cat);

    /* Epoch 1: two sources cooked in the same run. */
    jce_cook_catalog_begin_epoch(&cat);
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "keep.png",   0xAAAAu, 10, 100));
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "delete.png", 0xBBBBu, 20, 200));
    /* Nothing swept while both are seen this epoch. */
    TEST_ASSERT_EQUAL_size_t(0, jce_cook_catalog_sweep_unseen(&cat));
    TEST_ASSERT_EQUAL_size_t(2, cat.count);

    /* Epoch 2: the source tree now only contains keep.png (delete.png was
       removed/moved).  begin_epoch clears all liveness; only keep is re-seen. */
    jce_cook_catalog_begin_epoch(&cat);
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "keep.png", 0xAAAAu, 10, 100));

    /* mark_seen on an incremental HIT keeps an entry live without record();
       here keep.png is already seen via record() — exercise the no-op-absent
       branch to confirm it doesn't resurrect a stale key. */
    jce_cook_catalog_mark_seen(&cat, "not-in-catalog.png");

    size_t pruned = jce_cook_catalog_sweep_unseen(&cat);
    TEST_ASSERT_EQUAL_size_t(1, pruned);            /* delete.png pruned */
    TEST_ASSERT_EQUAL_size_t(1, cat.count);         /* only keep.png remains */

    /* The seen entry survives intact; the unseen one is gone. */
    const JceCookCatalogEntry *kept = jce_cook_catalog_find(&cat, "keep.png");
    TEST_ASSERT_NOT_NULL(kept);
    TEST_ASSERT_EQUAL_HEX64(0xAAAAu, kept->hash);
    TEST_ASSERT_EQUAL_UINT64(10, kept->size);
    TEST_ASSERT_NULL(jce_cook_catalog_find(&cat, "delete.png"));

    /* A fresh epoch with NO records sweeps everything. */
    jce_cook_catalog_begin_epoch(&cat);
    TEST_ASSERT_EQUAL_size_t(1, jce_cook_catalog_sweep_unseen(&cat));
    TEST_ASSERT_EQUAL_size_t(0, cat.count);

    jce_cook_catalog_free(&cat);
}

/* mark_seen rescues an entry that record() never touched (incremental HIT). */
static void test_mark_seen_rescues_unrecorded_entry(void)
{
    JceCookCatalog cat;
    jce_cook_catalog_init(&cat);

    jce_cook_catalog_begin_epoch(&cat);
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "a.png", 0x11u, 1, 1));
    TEST_ASSERT_TRUE(jce_cook_catalog_record(&cat, "b.png", 0x22u, 2, 2));

    /* Simulate a second run where a.png is an incremental cache HIT (no
       record() call — the cooker just marks it seen) and b.png vanished. */
    jce_cook_catalog_begin_epoch(&cat);
    jce_cook_catalog_mark_seen(&cat, "a.png");

    TEST_ASSERT_EQUAL_size_t(1, jce_cook_catalog_sweep_unseen(&cat));
    TEST_ASSERT_NOT_NULL(jce_cook_catalog_find(&cat, "a.png"));
    TEST_ASSERT_NULL(jce_cook_catalog_find(&cat, "b.png"));

    jce_cook_catalog_free(&cat);
}

/* A missing catalog file is the first-cook case: empty catalog, not error. */
static void test_load_missing_catalog_is_empty_not_error(void)
{
    char cat_path[1024];
    join_path(cat_path, sizeof(cat_path), "never_written");

    JceCookCatalog in;
    TEST_ASSERT_TRUE(jce_cook_catalog_load(&in, cat_path));
    TEST_ASSERT_EQUAL_size_t(0, in.count);
    TEST_ASSERT_FALSE(jce_cook_entry_is_up_to_date(&in, "anything", 5u));
    jce_cook_catalog_free(&in);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hash_buffer_is_deterministic_and_sensitive);
    RUN_TEST(test_predicate_lifecycle);
    RUN_TEST(test_hash_file_folds_source_and_sidecar);
    RUN_TEST(test_file_change_flips_up_to_date);
    RUN_TEST(test_catalog_save_load_round_trip);
    RUN_TEST(test_sweep_unseen_prunes_stale_entries);
    RUN_TEST(test_mark_seen_rescues_unrecorded_entry);
    RUN_TEST(test_load_missing_catalog_is_empty_not_error);
    return UNITY_END();
}
