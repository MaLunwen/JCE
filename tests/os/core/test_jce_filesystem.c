/*
 * test_jce_filesystem.c — Unit tests for the VFS API of jce_filesystem.h
 *
 * Layer: L1.  Uses PhysFS via jce_fs_create() + a tmp loose-file mount.
 * No PAK is constructed (that would require running the cooker tool).
 */

#include "unity.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static char g_root[1024];

void setUp(void)
{
    static int counter;
    char base[1024];
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_root, sizeof(g_root), "%s/_ut_vfs_%d", base, ++counter);
    (void)jce_fs_host_remove_recursive(g_root);
    jce_fs_host_create_directory(g_root);
}

void tearDown(void)
{
    jce_fs_set_active(NULL);
    (void)jce_fs_host_remove_recursive(g_root);
}

static void seed_file(const char *leaf, const char *data)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", g_root, leaf);
    jce_fs_host_write_all(p, data, strlen(data));
}

/* ---- create / destroy ------------------------------------------------ */

static void test_create_and_destroy(void)
{
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_fs_mounted_pak_count(fs));
    jce_fs_destroy(fs);
}

/* ---- mount dir + read all ------------------------------------------- */

static void test_mount_dir_read_all(void)
{
    seed_file("hello.txt", "hello, vfs");

    JceFileSystem *fs = jce_fs_create();
    jce_fs_mount_dir(fs, "assets/", g_root);

    TEST_ASSERT_TRUE(jce_fs_exists(fs, "assets/hello.txt"));

    uint64_t n = 0;
    void *buf = jce_fs_read_all(fs, "assets/hello.txt", &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)strlen("hello, vfs"), n);
    TEST_ASSERT_EQUAL_MEMORY("hello, vfs", buf, (size_t)n);
    jce_free(buf);

    jce_fs_destroy(fs);
}

/* ---- streaming reads ------------------------------------------------- */

static void test_open_read_size_close(void)
{
    seed_file("data.bin", "ABCDEFGHIJ");

    JceFileSystem *fs = jce_fs_create();
    jce_fs_mount_dir(fs, "/", g_root);

    JceFile *f = jce_fs_open(fs, "data.bin");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT64(10u, jce_fs_size(f));

    char tmp[4] = { 0 };
    uint64_t n = jce_fs_read(f, tmp, sizeof(tmp));
    TEST_ASSERT_EQUAL_UINT64(4u, n);
    TEST_ASSERT_EQUAL_MEMORY("ABCD", tmp, 4);

    jce_fs_close(f);
    jce_fs_destroy(fs);
}

/* ---- write through set_write_dir ------------------------------------ */

static void test_write_dir_and_write_all_then_read(void)
{
    JceFileSystem *fs = jce_fs_create();
    /* write dir must exist on host. */
    TEST_ASSERT_TRUE(jce_fs_set_write_dir(fs, g_root));

    const char *payload = "persisted";
    TEST_ASSERT_TRUE(jce_fs_write_all(fs, "out.txt", payload, strlen(payload)));

    /* Verify file was created on host. */
    char host_path[1024];
    snprintf(host_path, sizeof(host_path), "%s/out.txt", g_root);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(host_path));

    jce_fs_destroy(fs);
}

/* ---- missing file path ---------------------------------------------- */

static void test_missing_file_returns_null(void)
{
    JceFileSystem *fs = jce_fs_create();
    jce_fs_mount_dir(fs, "x/", g_root);
    TEST_ASSERT_FALSE(jce_fs_exists(fs, "x/missing.bin"));
    TEST_ASSERT_NULL (jce_fs_open  (fs, "x/missing.bin"));
    jce_fs_destroy(fs);
}

/* ---- active fs override ---------------------------------------------- */

static void test_set_active_round_trip(void)
{
    JceFileSystem *fs = jce_fs_create();
    jce_fs_set_active(fs);
    TEST_ASSERT_EQUAL_PTR(fs, jce_fs_get_active());
    jce_fs_set_active(NULL);
    TEST_ASSERT_NULL(jce_fs_get_active());
    jce_fs_destroy(fs);
}

static void test_active_isolated_policy_blocks_relative_host_fallback(void)
{
    seed_file("host_only.txt", "host");

    const char *base = strrchr(g_root, '/');
    if (!base) base = strrchr(g_root, '\\');
    TEST_ASSERT_NOT_NULL(base);
    ++base;

    char relative_path[256];
    char absolute_path[1024];
    snprintf(relative_path, sizeof(relative_path), "%s/host_only.txt", base);
    snprintf(absolute_path, sizeof(absolute_path), "%s/host_only.txt", g_root);

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);

    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(relative_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    jce_fs_buffer_free(buf);

    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);
    TEST_ASSERT_EQUAL_INT(JCE_FS_ACTIVE_ISOLATED,
                          jce_fs_get_active_policy());
    TEST_ASSERT_NULL(jce_fs_host_read_all(relative_path, &n));

    /* Absolute editor/tooling paths remain host paths even while content
     * lookup is isolated. */
    buf = jce_fs_host_read_all(absolute_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    jce_fs_buffer_free(buf);

    jce_fs_set_active(NULL);
    jce_fs_destroy(fs);
}


/* ---- path traversal (plan §35.3) ------------------------------------- *
 *
 * The VFS is what mods, downloaded content and scene JSON reach the disk
 * through, and every one of those is attacker-influenced input.  A virtual
 * path must never be able to name a file outside the mount.
 *
 * PhysFS is documented to enforce this, but "the library says so" is not
 * coverage — nothing here asserted it, so a future backend swap, or a
 * well-meaning "resolve the path first" change in jce_filesystem.c, would
 * silently open the sandbox.  These tests seed a REAL secret next to the
 * mount root and then try to reach it, so a regression fails loudly with
 * the secret's own contents rather than on a mocked expectation.
 */

static void seed_outside(const char *leaf, const char *data)
{
    char p[1024];
    /* Sibling of g_root, i.e. exactly one "../" away from the mount. */
    snprintf(p, sizeof(p), "%s/../%s", g_root, leaf);
    jce_fs_host_write_all(p, data, strlen(data));
}

static void unlink_outside(const char *leaf)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/../%s", g_root, leaf);
    (void)jce_fs_host_remove_file(p);
}

static void test_traversal_cannot_escape_the_mount(void)
{
    seed_file("inside.txt", "public");
    seed_outside("_ut_secret.txt", "TOP-SECRET-DO-NOT-LEAK");

    /* Non-vacuity: a traversal test that passes because the target was never
       created proves nothing.  Assert the secret really is on disk, one
       directory above the mount, before trying to reach it. */
    {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/../_ut_secret.txt", g_root);
        TEST_ASSERT_TRUE_MESSAGE(jce_fs_host_exists_file(probe),
            "test setup failed: the secret was never written, so the "
            "traversal assertions below would pass vacuously");
    }

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "assets/", g_root));

    /* Sanity: the mount itself works, so a NULL below means "blocked",
       not "nothing was mounted". */
    TEST_ASSERT_TRUE(jce_fs_exists(fs, "assets/inside.txt"));

    static const char *const kEscapes[] = {
        "assets/../_ut_secret.txt",
        "assets/../../_ut_secret.txt",
        "../_ut_secret.txt",
        "assets/./../_ut_secret.txt",
        "assets/subdir/../../_ut_secret.txt",
        /* Backslash form: Windows separators must not become an escape
           hatch that the '/'-only checks miss. */
        "assets\..\_ut_secret.txt",
    };

    for (unsigned i = 0; i < sizeof(kEscapes) / sizeof(kEscapes[0]); ++i) {
        uint64_t n = 0;
        void *buf = jce_fs_read_all(fs, kEscapes[i], &n);
        if (buf) {
            /* If anything came back, prove it is not the secret before
               failing — that distinguishes "leaked" from "read something
               harmless". */
            const bool leaked = (n >= 10) &&
                memcmp(buf, "TOP-SECRET", 10) == 0;
            jce_free(buf);
            TEST_ASSERT_FALSE_MESSAGE(leaked, kEscapes[i]);
        }
        TEST_ASSERT_FALSE_MESSAGE(jce_fs_exists(fs, kEscapes[i]), kEscapes[i]);
    }

    jce_fs_destroy(fs);
    unlink_outside("_ut_secret.txt");
}

/* An absolute host path handed to the VFS must not be honoured either —
   this is the other half of the same escape, and the one that a
   "convenience" fallback to the host filesystem tends to reintroduce. */
static void test_absolute_host_path_is_not_a_vfs_path(void)
{
    seed_outside("_ut_secret2.txt", "TOP-SECRET-ABS");

    char abs_secret[1024];
    snprintf(abs_secret, sizeof(abs_secret), "%s/../_ut_secret2.txt", g_root);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(abs_secret));   /* it IS there */

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_TRUE(jce_fs_mount_dir(fs, "assets/", g_root));

    uint64_t n = 0;
    void *buf = jce_fs_read_all(fs, abs_secret, &n);
    if (buf) {
        const bool leaked = (n >= 10) && memcmp(buf, "TOP-SECRET", 10) == 0;
        jce_free(buf);
        TEST_ASSERT_FALSE_MESSAGE(leaked, "absolute host path read through VFS");
    }

    jce_fs_destroy(fs);
    unlink_outside("_ut_secret2.txt");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_destroy);
    RUN_TEST(test_mount_dir_read_all);
    RUN_TEST(test_open_read_size_close);
    RUN_TEST(test_write_dir_and_write_all_then_read);
    RUN_TEST(test_missing_file_returns_null);
    RUN_TEST(test_set_active_round_trip);
    RUN_TEST(test_active_isolated_policy_blocks_relative_host_fallback);
    RUN_TEST(test_traversal_cannot_escape_the_mount);
    RUN_TEST(test_absolute_host_path_is_not_a_vfs_path);
    return UNITY_END();
}
