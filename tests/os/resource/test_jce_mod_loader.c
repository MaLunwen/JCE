/*
 * test_jce_mod_loader.c  Unit tests for the game-side modding / external-PAK
 * loader (FEATURE 9.5, L2 / resource).
 *
 * Exercises the REAL scan -> mount -> read path through the public API:
 *   - a base PAK plus two mod PAKs that override the same asset are written to
 *     a temp directory on disk;
 *   - jce_mod_loader_scan() discovers the mods, jce_mod_loader_mount() builds
 *     the layered override stack, and reads go through the live mount handle
 *     (jce_archive_mount_read) — the same path a shipped game would use;
 *   - override precedence: the highest-load-order ENABLED mod's copy wins,
 *     falling back to the base for assets no mod provides;
 *   - a disabled mod never contributes;
 *   - reordering load order changes which mod wins;
 *   - an optional sidecar manifest sets id / load_order / enabled.
 *
 * Layout: the base archive lives at <tmp>/base.jpak (NOT scanned) and the mods
 * live in <tmp>/mods/ (the scanned directory) — mirroring a shipped game whose
 * content sits next to a separate user "mods" folder.
 */

#include "unity.h"

#include <jce/resource/jce_mod_loader.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>

#include <string.h>
#include <stdio.h>

static char g_tmp[1024];   /* <tmp> root (holds base.jpak)        */
static char g_mods[1024];  /* <tmp>/mods (the scanned directory)  */

void setUp(void) {
    snprintf(g_tmp,  sizeof(g_tmp),  "test_jce_mod_loader_tmp");
    snprintf(g_mods, sizeof(g_mods), "%s/mods", g_tmp);
    jce_fs_host_remove_recursive(g_tmp);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_tmp));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_mods));
}

void tearDown(void) {
    jce_fs_host_remove_recursive(g_tmp);
}

/* Write a one- or two-entry archive to <dir>/<filename>. */
static void write_archive(const char *dir, const char *filename,
                          const char *p0, const char *v0,
                          const char *p1, const char *v1) {
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, p0, v0, strlen(v0)));
    if (p1) TEST_ASSERT_TRUE(jce_archive_writer_add(w, p1, v1, strlen(v1)));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    TEST_ASSERT_NOT_NULL(buf);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, buf, (uint64_t)sz));
    jce_free(buf);
}

/* Write a raw text file (used for sidecar manifests) to <dir>/<filename>. */
static void write_text(const char *dir, const char *filename, const char *text) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, text, (uint64_t)strlen(text)));
}

/* Open <g_tmp>/base.jpak as a borrowed base archive. */
static JceArchive *open_base(void) {
    char base_path[1024];
    snprintf(base_path, sizeof(base_path), "%s/base.jpak", g_tmp);
    JceArchive *base = jce_archive_open_file(base_path);
    TEST_ASSERT_NOT_NULL(base);
    return base;
}

/* Read a path through the loader's live mount and compare against expected. */
static void assert_mount_reads(const JceModLoader *ml, const char *path,
                               const char *expected) {
    const JceArchiveMount *m = jce_mod_loader_mount_handle(ml);
    TEST_ASSERT_NOT_NULL(m);
    char out[128];
    size_t n = jce_archive_mount_read(m, path, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(strlen(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, n);
}

/* ── core: scan, override precedence, disable, reorder ────────────────── */

static void test_scan_mount_override_precedence(void) {
    /* Base ships a.txt (base-only) and shared.txt; both mods override
     * shared.txt with distinct content.  bravo has the higher load order so it
     * wins when both are enabled. */
    write_archive(g_tmp,  "base.jpak", "cfg/a.txt", "BASE-A",
                                       "cfg/shared.txt", "BASE-SHARED");
    write_archive(g_mods, "mod_a.jpak", "cfg/shared.txt", "MODA-SHARED", NULL, NULL);
    write_archive(g_mods, "mod_b.jpak", "cfg/shared.txt", "MODB-SHARED", NULL, NULL);

    /* Sidecars: explicit ids + load orders.  bravo (20) > alpha (10). */
    write_text(g_mods, "mod_a.mod.json",
               "{\"id\":\"alpha\",\"name\":\"Alpha\",\"version\":\"1.0\","
               "\"load_order\":10,\"enabled\":true}");
    write_text(g_mods, "mod_b.mod.json",
               "{\"id\":\"bravo\",\"name\":\"Bravo\",\"version\":\"2.1\","
               "\"load_order\":20,\"enabled\":true}");

    JceArchive *base = open_base();

    JceModLoader *ml = jce_mod_loader_create();
    TEST_ASSERT_NOT_NULL(ml);

    int found = jce_mod_loader_scan(ml, g_mods);
    TEST_ASSERT_EQUAL_INT(2, found);                 /* mod_a + mod_b          */
    TEST_ASSERT_EQUAL_size_t(2, jce_mod_loader_count(ml));

    /* Manifest ids + load orders were honored. */
    TEST_ASSERT_NOT_EQUAL(-1, jce_mod_loader_find(ml, "alpha"));
    TEST_ASSERT_NOT_EQUAL(-1, jce_mod_loader_find(ml, "bravo"));
    TEST_ASSERT_EQUAL_INT(10, jce_mod_loader_get_load_order(ml, "alpha"));
    TEST_ASSERT_EQUAL_INT(20, jce_mod_loader_get_load_order(ml, "bravo"));

    jce_mod_loader_set_base(ml, base);
    int layers = jce_mod_loader_mount(ml);
    TEST_ASSERT_EQUAL_INT(3, layers);                /* base + 2 mods          */
    TEST_ASSERT_EQUAL_size_t(2, jce_mod_loader_mounted_count(ml));

    /* a.txt only in base → base value. */
    assert_mount_reads(ml, "cfg/a.txt", "BASE-A");
    /* shared.txt: highest-order ENABLED mod (bravo) wins. */
    assert_mount_reads(ml, "cfg/shared.txt", "MODB-SHARED");

    /* Disable bravo → alpha now wins shared.txt; bravo must not contribute. */
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "bravo", false));
    layers = jce_mod_loader_mount(ml);
    TEST_ASSERT_EQUAL_INT(2, layers);                /* base + alpha           */
    TEST_ASSERT_EQUAL_size_t(1, jce_mod_loader_mounted_count(ml));
    assert_mount_reads(ml, "cfg/shared.txt", "MODA-SHARED");

    /* Disable BOTH mods → base copy resurfaces. */
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "alpha", false));
    layers = jce_mod_loader_mount(ml);
    TEST_ASSERT_EQUAL_INT(1, layers);                /* base only              */
    TEST_ASSERT_EQUAL_size_t(0, jce_mod_loader_mounted_count(ml));
    assert_mount_reads(ml, "cfg/shared.txt", "BASE-SHARED");

    /* Re-enable both, then REORDER so alpha outranks bravo → alpha wins. */
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "alpha", true));
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "bravo", true));
    TEST_ASSERT_TRUE(jce_mod_loader_set_load_order(ml, "alpha", 100));
    layers = jce_mod_loader_mount(ml);
    TEST_ASSERT_EQUAL_INT(3, layers);
    assert_mount_reads(ml, "cfg/shared.txt", "MODA-SHARED");

    /* And flip it back: bravo above alpha again. */
    TEST_ASSERT_TRUE(jce_mod_loader_set_load_order(ml, "alpha", 5));
    jce_mod_loader_mount(ml);
    assert_mount_reads(ml, "cfg/shared.txt", "MODB-SHARED");

    jce_mod_loader_destroy(ml);
    jce_archive_close(base);
}

/* ── default id from file stem when no manifest is present ────────────── */

static void test_scan_no_manifest_defaults(void) {
    write_archive(g_tmp,  "base.jpak", "cfg/x.txt", "BASE-X", NULL, NULL);
    write_archive(g_mods, "skin_pack.jpak", "cfg/x.txt", "SKIN-X", NULL, NULL);

    JceArchive *base = open_base();

    JceModLoader *ml = jce_mod_loader_create();
    TEST_ASSERT_NOT_NULL(ml);

    int found = jce_mod_loader_scan(ml, g_mods);
    TEST_ASSERT_EQUAL_INT(1, found);
    TEST_ASSERT_EQUAL_size_t(1, jce_mod_loader_count(ml));

    int idx = jce_mod_loader_find(ml, "skin_pack");
    TEST_ASSERT_NOT_EQUAL(-1, idx);

    JceModInfo info;
    TEST_ASSERT_TRUE(jce_mod_loader_get(ml, 0, &info));
    TEST_ASSERT_EQUAL_STRING("skin_pack", info.id);
    TEST_ASSERT_EQUAL_STRING("skin_pack", info.name);   /* name defaults to id */
    TEST_ASSERT_EQUAL_STRING("", info.version);         /* unspecified         */
    TEST_ASSERT_TRUE(info.enabled);                     /* default enabled     */
    TEST_ASSERT_EQUAL_INT(0, info.load_order);          /* default order       */
    TEST_ASSERT_NOT_NULL(info.file);

    jce_mod_loader_set_base(ml, base);
    TEST_ASSERT_EQUAL_INT(2, jce_mod_loader_mount(ml));  /* base + skin_pack   */
    /* The skin pack overrides x.txt over the base. */
    assert_mount_reads(ml, "cfg/x.txt", "SKIN-X");

    jce_mod_loader_destroy(ml);
    jce_archive_close(base);
}

/* ── empty / missing directory and idempotent re-scan ────────────────── */

static void test_missing_dir_and_rescan_idempotent(void) {
    JceModLoader *ml = jce_mod_loader_create();
    TEST_ASSERT_NOT_NULL(ml);

    /* Missing directory is not an error. */
    TEST_ASSERT_EQUAL_INT(0, jce_mod_loader_scan(ml, "no_such_dir_xyz"));
    TEST_ASSERT_EQUAL_size_t(0, jce_mod_loader_count(ml));

    write_archive(g_mods, "only.jmod", "cfg/y.txt", "MOD-Y", NULL, NULL);

    /* First scan finds it; a second scan finds 0 NEW and preserves edits. */
    TEST_ASSERT_EQUAL_INT(1, jce_mod_loader_scan(ml, g_mods));
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "only", false));
    TEST_ASSERT_EQUAL_INT(0, jce_mod_loader_scan(ml, g_mods));   /* idempotent */
    TEST_ASSERT_EQUAL_size_t(1, jce_mod_loader_count(ml));
    TEST_ASSERT_FALSE(jce_mod_loader_is_enabled(ml, "only"));    /* edit kept  */

    /* No base, mod disabled → mount has 0 layers, read misses. */
    TEST_ASSERT_EQUAL_INT(0, jce_mod_loader_mount(ml));
    const JceArchiveMount *m = jce_mod_loader_mount_handle(ml);
    TEST_ASSERT_NOT_NULL(m);
    char out[64];
    TEST_ASSERT_EQUAL_size_t(0, jce_archive_mount_read(m, "cfg/y.txt", out, sizeof(out)));

    /* Enable it → its asset now reads (no base needed). */
    TEST_ASSERT_TRUE(jce_mod_loader_set_enabled(ml, "only", true));
    TEST_ASSERT_EQUAL_INT(1, jce_mod_loader_mount(ml));
    assert_mount_reads(ml, "cfg/y.txt", "MOD-Y");

    jce_mod_loader_destroy(ml);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scan_mount_override_precedence);
    RUN_TEST(test_scan_no_manifest_defaults);
    RUN_TEST(test_missing_dir_and_rescan_idempotent);
    return UNITY_END();
}
