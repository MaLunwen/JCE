/* test_jce_asset_path_identity.c
 *
 * Characterization tests for REF-007 (dedup audit): the asset manager's
 * registry key MUST use the same resource identity as the archive layer that
 * actually fetches the bytes.
 *
 * Before REF-007 the manager keyed its registry on XXH3(raw path string) while
 * jce_archive_hash_path() canonicalises separators and case before hashing.
 * Two spellings of one asset therefore occupied two slots, were decoded and
 * GPU-uploaded twice, and defeated the "already loaded -> bump refcount"
 * guarantee, even though both resolved to the same archive entry.
 *
 * These tests pin (a) the archive identity contract itself and (b) that
 * jce_asset_acquire() deduplicates across equivalent spellings.
 */

#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_asset.h>
#include <jce/resource/jce_pak_loader.h>

#include "unity.h"

#include <stdint.h>
#include <string.h>

/* The manager requires a PAK. Build a tiny synthetic JPAK v1 in memory via the
 * public archive writer, exactly as test_jce_pak_loader.c does. Its contents
 * are irrelevant here — these tests exercise registry IDENTITY, which is
 * resolved before any byte is fetched. */
static void       *g_pak_blob = NULL;
static JcePakArchive *g_pak   = NULL;

static void make_pak(void)
{
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, "placeholder.txt", "X", 1));

    size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &g_pak_blob, &sz));
    TEST_ASSERT_NOT_NULL(g_pak_blob);
    jce_archive_writer_destroy(w);

    g_pak = jce_pak_open(g_pak_blob, sz);
    TEST_ASSERT_NOT_NULL(g_pak);
}

static JceAssetManager *make_manager(void)
{
    JceAssetManagerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pak = g_pak;
    JceAssetManager *mgr = jce_asset_manager_create(&cfg);
    TEST_ASSERT_NOT_NULL(mgr);
    return mgr;
}

void setUp(void)    { make_pak(); }
void tearDown(void)
{
    if (g_pak) { jce_pak_close(g_pak); g_pak = NULL; }
    if (g_pak_blob) { jce_free(g_pak_blob); g_pak_blob = NULL; }
}

/* ── The identity contract the manager must share ────────────────────── */
static void test_archive_hash_is_spelling_insensitive(void)
{
    const uint64_t canonical = jce_archive_hash_path("textures/wall.png");

    /* Case differences. */
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("Textures/Wall.PNG"));
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("TEXTURES/WALL.PNG"));
    /* Separator style and redundant separators. */
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("textures\\wall.png"));
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("textures//wall.png"));
    /* Leading './' and leading separator. */
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("./textures/wall.png"));
    TEST_ASSERT_EQUAL_UINT64(canonical, jce_archive_hash_path("/textures/wall.png"));

    /* Genuinely different resources must NOT collide. */
    TEST_ASSERT_NOT_EQUAL_UINT64(canonical, jce_archive_hash_path("textures/floor.png"));
    TEST_ASSERT_NOT_EQUAL_UINT64(canonical, jce_archive_hash_path("meshes/wall.png"));
}

/* ── The regression this file exists for ─────────────────────────────── */
static void test_acquire_dedups_equivalent_spellings(void)
{
    JceAssetManager *mgr = make_manager();

    /* Async acquire (the default) registers the slot without needing the
     * bytes to exist, which is exactly the identity behaviour under test. */
    JceAssetHandle a = jce_asset_acquire(mgr, "textures/wall.png",
                                         JCE_ASSET_TEXTURE, NULL);
    JceAssetHandle b = jce_asset_acquire(mgr, "Textures/Wall.PNG",
                                         JCE_ASSET_TEXTURE, NULL);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(a.index, b.index,
        "two spellings of one asset must share a slot (REF-007 regression)");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2, jce_asset_ref_count(mgr, a),
        "the second acquire must bump the refcount, not allocate a slot");
    /* NOTE: jce_asset_count() reports assets that finished LOADING, not slots
     * in use. These paths are not present in the synthetic PAK, so the load
     * fails and the count stays 0 — irrelevant here. Slot identity plus the
     * refcount is the dedup contract, and it is resolved before any byte is
     * fetched. */

    jce_asset_release(mgr, b);
    TEST_ASSERT_EQUAL_UINT(1, jce_asset_ref_count(mgr, a));
    jce_asset_release(mgr, a);

    jce_asset_manager_destroy(mgr);
}

/* Separator style and redundant separators must dedup too. */
static void test_acquire_dedups_separator_variants(void)
{
    JceAssetManager *mgr = make_manager();

    JceAssetHandle a = jce_asset_acquire(mgr, "meshes/props/crate.glb",
                                         JCE_ASSET_MESH, NULL);
    JceAssetHandle b = jce_asset_acquire(mgr, "meshes\\props\\crate.glb",
                                         JCE_ASSET_MESH, NULL);
    JceAssetHandle c = jce_asset_acquire(mgr, "./meshes//props/crate.glb",
                                         JCE_ASSET_MESH, NULL);

    TEST_ASSERT_EQUAL_UINT(a.index, b.index);
    TEST_ASSERT_EQUAL_UINT(a.index, c.index);
    TEST_ASSERT_EQUAL_UINT(3, jce_asset_ref_count(mgr, a));

    jce_asset_release(mgr, c);
    jce_asset_release(mgr, b);
    jce_asset_release(mgr, a);
    jce_asset_manager_destroy(mgr);
}

/* Distinct resources, and the same path at a different asset TYPE, must
 * still occupy distinct slots — the key mixes type/params on purpose. */
static void test_distinct_resources_do_not_collide(void)
{
    JceAssetManager *mgr = make_manager();

    JceAssetHandle wall  = jce_asset_acquire(mgr, "textures/wall.png",
                                             JCE_ASSET_TEXTURE, NULL);
    JceAssetHandle floor = jce_asset_acquire(mgr, "textures/floor.png",
                                             JCE_ASSET_TEXTURE, NULL);
    TEST_ASSERT_NOT_EQUAL_UINT(wall.index, floor.index);

    /* Same path, different type -> different slot (type is mixed into the key). */
    JceAssetHandle as_raw = jce_asset_acquire(mgr, "textures/wall.png",
                                              JCE_ASSET_RAW, NULL);
    TEST_ASSERT_NOT_EQUAL_UINT(wall.index, as_raw.index);

    jce_asset_release(mgr, as_raw);
    jce_asset_release(mgr, floor);
    jce_asset_release(mgr, wall);
    jce_asset_manager_destroy(mgr);
}


/* ── jce_asset_reload reports WHY, not just whether ──────────────────── *
 *
 * reload() used to return void, so a hot-reload watcher could not tell
 * "busy, retry" from "broken, stop retrying" from "you passed a bad
 * handle".  It now returns the asset domain's own JceAssetState, and the
 * distinction is the whole point of the change — pin it.
 */

static void test_reload_rejects_bad_handle_as_unloaded(void)
{
    JceAssetManager *mgr = make_manager();

    /* Never-loaded / out-of-range handle: a caller bug, and retrying it
       cannot help — so it must NOT come back as FAILED (retryable-looking)
       or READY. */
    JceAssetHandle bogus;
    memset(&bogus, 0, sizeof bogus);
    bogus.index = 0xFFFFu;

    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_UNLOADED,
                          (int)jce_asset_reload(mgr, bogus));

    jce_asset_manager_destroy(mgr);
}

static void test_reload_of_never_loaded_slot_is_not_ready(void)
{
    JceAssetManager *mgr = make_manager();

    /* A zeroed handle addresses slot 0, which has no source path.  The
       contract says UNLOADED (nothing to reload from), and specifically not
       READY — a watcher must not conclude a reload happened. */
    JceAssetHandle zero;
    memset(&zero, 0, sizeof zero);

    const JceAssetState st = jce_asset_reload(mgr, zero);
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(JCE_ASSET_STATE_READY, (int)st,
        "reload of a slot with no source path reported success");
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_UNLOADED, (int)st);

    jce_asset_manager_destroy(mgr);
}

/* The four documented outcomes must be four DISTINCT values, otherwise the
   branch the header asks callers to write cannot be written. */
static void test_reload_outcomes_are_distinguishable(void)
{
    const int vals[] = {
        (int)JCE_ASSET_STATE_READY,     /* reloaded            */
        (int)JCE_ASSET_STATE_LOADING,   /* deferred, retry     */
        (int)JCE_ASSET_STATE_FAILED,    /* failed, old kept    */
        (int)JCE_ASSET_STATE_UNLOADED,  /* caller bug          */
    };
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = i + 1; j < 4; ++j)
            TEST_ASSERT_NOT_EQUAL_INT(vals[i], vals[j]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_archive_hash_is_spelling_insensitive);
    RUN_TEST(test_acquire_dedups_equivalent_spellings);
    RUN_TEST(test_acquire_dedups_separator_variants);
    RUN_TEST(test_distinct_resources_do_not_collide);
    RUN_TEST(test_reload_rejects_bad_handle_as_unloaded);
    RUN_TEST(test_reload_of_never_loaded_slot_is_not_ready);
    RUN_TEST(test_reload_outcomes_are_distinguishable);
    return UNITY_END();
}
