/*
 * test_jce_filesystem_host.c — Unit tests for host helpers in
 * jce_filesystem.h (jce_fs_host_*).
 *
 * Layer: L1.  Real disk I/O scoped to a tmp dir inside the build tree
 * so tests stay deterministic across CI.
 */

#include "unity.h"

#include <jce/os/core/jce_filesystem.h>

#include <stdint.h>
#include <string.h>

static char g_root[1024];

void setUp(void)
{
    /* Per-test unique tmp dir under build dir. */
    static int counter;
    char base[1024];
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_root, sizeof(g_root),
             "%s/_ut_fs_host_%d_%d", base, (int)(uintptr_t)setUp & 0xFFFF,
             ++counter);
    (void)jce_fs_host_remove_recursive(g_root);
    jce_fs_host_create_directory(g_root);
}

void tearDown(void)
{
    (void)jce_fs_host_remove_recursive(g_root);
}

static void join(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static void test_create_and_exists_dir(void)
{
    char d[1024];
    join(d, sizeof(d), "sub/deep");
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(d));
    TEST_ASSERT_TRUE(jce_fs_host_exists_dir(d));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(d));
}

static void test_write_read_and_size(void)
{
    char f[1024];
    join(f, sizeof(f), "hello.txt");
    const char *data = "hello, world";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(f, data, strlen(data)));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(f));

    uint64_t sz = 0;
    TEST_ASSERT_TRUE(jce_fs_host_get_size(f, &sz));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)strlen(data), sz);

    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(f, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)strlen(data), n);
    TEST_ASSERT_EQUAL_MEMORY(data, buf, (size_t)n);
    jce_fs_buffer_free(buf);
}

static void test_append_grows(void)
{
    char f[1024];
    join(f, sizeof(f), "log.txt");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(f, "A", 1));
    TEST_ASSERT_TRUE(jce_fs_host_append(f, "B",  1));
    TEST_ASSERT_TRUE(jce_fs_host_append(f, "C",  1));

    uint64_t sz = 0;
    TEST_ASSERT_TRUE(jce_fs_host_get_size(f, &sz));
    TEST_ASSERT_EQUAL_UINT64(3u, sz);
}

static void test_copy_and_rename_and_remove(void)
{
    char src[1024], dst[1024], moved[1024];
    join(src,   sizeof(src),   "src.bin");
    join(dst,   sizeof(dst),   "copy.bin");
    join(moved, sizeof(moved), "moved.bin");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(src, "xyz", 3));

    TEST_ASSERT_TRUE(jce_fs_host_copy_file(src, dst));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(dst));

    TEST_ASSERT_TRUE(jce_fs_host_rename(dst, moved));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(dst));
    TEST_ASSERT_TRUE (jce_fs_host_exists_file(moved));

    TEST_ASSERT_TRUE(jce_fs_host_remove_file(src));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(src));
}

static void test_make_unique_path_when_taken(void)
{
    char f[1024];
    join(f, sizeof(f), "name.dat");
    TEST_ASSERT_TRUE(jce_fs_host_write_all(f, "1", 1));

    char unique[1024];
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(f, unique, sizeof(unique)));
    /* Returned path must NOT yet exist. */
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(unique));
    /* And must differ from the desired one. */
    TEST_ASSERT_TRUE(strcmp(f, unique) != 0);
}

static int g_walk_count;

static bool walk_cb(const char *path, bool is_dir, void *u)
{
    (void)path; (void)is_dir; (void)u;
    ++g_walk_count;
    return true;
}

static void test_walk_visits_entries(void)
{
    char a[1024], b[1024], dir[1024];
    join(dir, sizeof(dir), "tree");
    jce_fs_host_create_directory(dir);
    join(a, sizeof(a), "tree/a.txt");
    join(b, sizeof(b), "tree/b.txt");
    jce_fs_host_write_all(a, "x", 1);
    jce_fs_host_write_all(b, "y", 1);

    g_walk_count = 0;
    TEST_ASSERT_TRUE(jce_fs_host_walk(dir, walk_cb, NULL));
    TEST_ASSERT_TRUE(g_walk_count >= 2);
}

static void test_base_path_nonempty(void)
{
    char buf[1024] = { 0 };
    TEST_ASSERT_TRUE(jce_fs_host_get_base_path(buf, sizeof(buf)));
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

/* ---- additional surface (was uncovered) ----------------------------- */

static int g_list_count;
static bool list_cb(const char *name, bool is_dir, void *u)
{
    (void)name; (void)is_dir; (void)u;
    ++g_list_count;
    return true;
}

static void test_list_dir_enumerates(void)
{
    char a[1024], b[1024];
    join(a, sizeof(a), "list_a.txt");
    join(b, sizeof(b), "list_b.txt");
    jce_fs_host_write_all(a, "1", 1);
    jce_fs_host_write_all(b, "2", 1);

    g_list_count = 0;
    TEST_ASSERT_TRUE(jce_fs_host_list_dir(g_root, list_cb, NULL));
    TEST_ASSERT_TRUE(g_list_count >= 2);
}

static void test_copy_recursive_clones_tree(void)
{
    char src_dir[1024], dst_dir[1024], a[1024];
    join(src_dir, sizeof(src_dir), "rsrc");
    join(dst_dir, sizeof(dst_dir), "rdst");
    jce_fs_host_create_directory(src_dir);
    join(a, sizeof(a), "rsrc/inner.txt");
    jce_fs_host_write_all(a, "deep", 4);

    TEST_ASSERT_TRUE(jce_fs_host_copy_recursive(src_dir, dst_dir));
    char dst_inner[1024];
    join(dst_inner, sizeof(dst_inner), "rdst/inner.txt");
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(dst_inner));
}

static void test_remove_recursive_deletes_nested_tree(void)
{
    char tree[1024], branch[1024], leaf[1024], sibling[1024];
    join(tree, sizeof(tree), "remove_tree");
    join(branch, sizeof(branch), "remove_tree/a/b/c");
    join(leaf, sizeof(leaf), "remove_tree/a/b/c/leaf.bin");
    join(sibling, sizeof(sibling), "remove_tree/a/sibling.txt");

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(branch));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(leaf, "leaf", 4));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(sibling, "sibling", 7));

    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(tree));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(tree));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(leaf));
}

static void test_read_capped_truncates(void)
{
    char f[1024];
    join(f, sizeof(f), "capped.bin");
    jce_fs_host_write_all(f, "abcdefghij", 10);

    uint64_t n = 0, total = 0;
    void *buf = jce_fs_host_read_capped(f, 4, &n, &total);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT64(4u, n);
    TEST_ASSERT_EQUAL_UINT64(10u, total);
    TEST_ASSERT_EQUAL_MEMORY("abcd", buf, 4);
    jce_fs_buffer_free(buf);
}

static void test_get_mtime_returns_nonzero(void)
{
    char f[1024];
    join(f, sizeof(f), "mtime.txt");
    jce_fs_host_write_all(f, "x", 1);

    int64_t t = 0;
    TEST_ASSERT_TRUE(jce_fs_host_get_mtime(f, &t));
    TEST_ASSERT_TRUE(t > 0);
}

static void test_missing_paths_report_false(void)
{
    char missing[1024];
    join(missing, sizeof(missing), "does_not_exist.x");
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(missing));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir (missing));
    uint64_t sz = 0;
    TEST_ASSERT_FALSE(jce_fs_host_get_size (missing, &sz));
    int64_t  mt = 0;
    TEST_ASSERT_FALSE(jce_fs_host_get_mtime(missing, &mt));
    TEST_ASSERT_NULL(jce_fs_host_read_all  (missing, &sz));
}

/* Atomic save: write to a sibling temp then rename over the target, so an
 * interrupted write can never leave a truncated/empty destination, and an
 * existing file is replaced (the save-over case). */
static void test_atomic_write_overwrites_and_no_temp(void)
{
    char f[1024], tmp[1024];
    join(f,   sizeof(f),   "scene.json");
    join(tmp, sizeof(tmp), "scene.json.tmp");

    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(f, "OLDDATA", 7));
    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(f, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT64(7u, n);
    TEST_ASSERT_EQUAL_MEMORY("OLDDATA", buf, 7);
    jce_fs_buffer_free(buf);

    /* Overwrite an existing file (the save case) — must replace, not fail. */
    TEST_ASSERT_TRUE(jce_fs_host_write_all_atomic(f, "NEW", 3));
    n = 0;
    buf = jce_fs_host_read_all(f, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT64(3u, n);
    TEST_ASSERT_EQUAL_MEMORY("NEW", buf, 3);
    jce_fs_buffer_free(buf);

    /* No stray temp sidecar after a successful write. */
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(tmp));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_exists_dir);
    RUN_TEST(test_write_read_and_size);
    RUN_TEST(test_append_grows);
    RUN_TEST(test_copy_and_rename_and_remove);
    RUN_TEST(test_make_unique_path_when_taken);
    RUN_TEST(test_walk_visits_entries);
    RUN_TEST(test_base_path_nonempty);
    RUN_TEST(test_list_dir_enumerates);
    RUN_TEST(test_copy_recursive_clones_tree);
    RUN_TEST(test_remove_recursive_deletes_nested_tree);
    RUN_TEST(test_read_capped_truncates);
    RUN_TEST(test_get_mtime_returns_nonzero);
    RUN_TEST(test_missing_paths_report_false);
    RUN_TEST(test_atomic_write_overwrites_and_no_temp);
    return UNITY_END();
}
