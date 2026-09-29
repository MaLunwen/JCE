/* test_jce_platform_services.c
 *
 * Unit tests for the platform-services facade + LOCAL backend
 * (jce_platform_services).  Exercises the REAL file-backed local backend in a
 * temp directory — no mocks:
 *   - init / user id / backend identity
 *   - achievement define + unlock + is_unlocked, idempotent double-unlock,
 *     progress -> auto-unlock
 *   - persistence across a shutdown + re-init reload
 *   - stats set/get with default
 *   - leaderboard submit (best-per-user) + top-N ordering + tie-break
 *   - cloud-save write/read round-trips identical bytes; list reflects slots;
 *     delete removes the slot
 */

#include <jce/os/core/jce_platform_services.h>

#include <jce/os/core/jce_filesystem.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

static char g_root[1024];   /* temp data dir for the LOCAL backend */

void setUp(void)
{
    char base[768];
    base[0] = '\0';
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_root, sizeof(g_root), "%s/_ut_platsvc", base);
    (void)jce_fs_host_remove_recursive(g_root);   /* clean slate each test */
}

void tearDown(void)
{
    (void)jce_fs_host_remove_recursive(g_root);
}

/* ── Init / identity ─────────────────────────────────────────────────────── */

static void test_init_and_user_id(void)
{
    char id[128];
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_EQUAL_INT(JCE_PLATFORM_BACKEND_LOCAL, jce_platform_backend_kind(svc));
    TEST_ASSERT_EQUAL_STRING("local", jce_platform_backend_name(svc));

    id[0] = '\0';
    TEST_ASSERT_TRUE(jce_platform_user_id(svc, id, sizeof id));
    TEST_ASSERT_TRUE(id[0] != '\0');           /* a stable non-empty id */

    jce_platform_shutdown(svc);
}

/* ── Achievements ────────────────────────────────────────────────────────── */

static void test_achievement_unlock_and_idempotent(void)
{
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_TRUE(jce_platform_ach_define(svc, "first_blood", "First Blood", 1));
    TEST_ASSERT_FALSE(jce_platform_ach_is_unlocked(svc, "first_blood"));

    TEST_ASSERT_TRUE(jce_platform_ach_unlock(svc, "first_blood"));
    TEST_ASSERT_TRUE(jce_platform_ach_is_unlocked(svc, "first_blood"));

    /* Double-unlock is idempotent (still success, still unlocked). */
    TEST_ASSERT_TRUE(jce_platform_ach_unlock(svc, "first_blood"));
    TEST_ASSERT_TRUE(jce_platform_ach_is_unlocked(svc, "first_blood"));

    /* Unknown achievement reads as locked. */
    TEST_ASSERT_FALSE(jce_platform_ach_is_unlocked(svc, "does_not_exist"));

    jce_platform_shutdown(svc);
}

static void test_achievement_progress_autounlock(void)
{
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_TRUE(jce_platform_ach_define(svc, "marathon", "Run 10km", 10));
    TEST_ASSERT_TRUE(jce_platform_ach_set_progress(svc, "marathon", 4));
    TEST_ASSERT_EQUAL_INT(4, jce_platform_ach_progress(svc, "marathon"));
    TEST_ASSERT_FALSE(jce_platform_ach_is_unlocked(svc, "marathon"));

    /* Reaching the target auto-unlocks; over-shoot clamps to target. */
    TEST_ASSERT_TRUE(jce_platform_ach_set_progress(svc, "marathon", 999));
    TEST_ASSERT_EQUAL_INT(10, jce_platform_ach_progress(svc, "marathon"));
    TEST_ASSERT_TRUE(jce_platform_ach_is_unlocked(svc, "marathon"));

    jce_platform_shutdown(svc);
}

/* ── Persistence across reload ───────────────────────────────────────────── */

static void test_persistence_across_reload(void)
{
    char id_a[128], id_b[128];
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_TRUE(jce_platform_ach_define(svc, "boss_down", "Boss Down", 1));
    TEST_ASSERT_TRUE(jce_platform_ach_unlock(svc, "boss_down"));
    TEST_ASSERT_TRUE(jce_platform_stat_set(svc, "kills", 42));
    id_a[0] = '\0';
    TEST_ASSERT_TRUE(jce_platform_user_id(svc, id_a, sizeof id_a));

    /* Shutdown flushes to disk. */
    jce_platform_shutdown(svc);

    /* Re-init the SAME dir: state must reload. */
    svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);
    TEST_ASSERT_TRUE(jce_platform_ach_is_unlocked(svc, "boss_down"));
    TEST_ASSERT_EQUAL_INT64(42, jce_platform_stat_get(svc, "kills", -1));
    TEST_ASSERT_EQUAL_INT64(-1, jce_platform_stat_get(svc, "missing", -1));

    id_b[0] = '\0';
    TEST_ASSERT_TRUE(jce_platform_user_id(svc, id_b, sizeof id_b));
    TEST_ASSERT_EQUAL_STRING(id_a, id_b);      /* stable user id */

    jce_platform_shutdown(svc);
}

/* ── Leaderboards ────────────────────────────────────────────────────────── */

static void test_leaderboard_top_n_ordering(void)
{
    int64_t scores[8];
    char    users[8][32];
    int     n;
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "alice", 100));
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "bob",   300));
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "carol", 200));
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "dave",  250));

    /* Best-score-per-user: a lower resubmission must NOT replace the best. */
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "alice", 50));
    /* A higher resubmission DOES replace. */
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "hi", "alice", 275));

    n = jce_platform_lb_query_top(svc, "hi", 8, scores, &users[0][0], 32);
    TEST_ASSERT_EQUAL_INT(4, n);

    /* Expected order: bob 300, alice 275, dave 250, carol 200. */
    TEST_ASSERT_EQUAL_INT64(300, scores[0]); TEST_ASSERT_EQUAL_STRING("bob",   users[0]);
    TEST_ASSERT_EQUAL_INT64(275, scores[1]); TEST_ASSERT_EQUAL_STRING("alice", users[1]);
    TEST_ASSERT_EQUAL_INT64(250, scores[2]); TEST_ASSERT_EQUAL_STRING("dave",  users[2]);
    TEST_ASSERT_EQUAL_INT64(200, scores[3]); TEST_ASSERT_EQUAL_STRING("carol", users[3]);

    /* Top-N truncation. */
    n = jce_platform_lb_query_top(svc, "hi", 2, scores, &users[0][0], 32);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT64(300, scores[0]);
    TEST_ASSERT_EQUAL_INT64(275, scores[1]);

    /* Missing board -> 0. */
    TEST_ASSERT_EQUAL_INT(0, jce_platform_lb_query_top(svc, "nope", 8, scores, NULL, 0));

    jce_platform_shutdown(svc);
}

static void test_leaderboard_tie_break_stable(void)
{
    int64_t scores[4];
    char    users[4][32];
    int     n;
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    /* Equal scores: earlier submission ranks first (stable). */
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "tie", "first",  500));
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "tie", "second", 500));
    TEST_ASSERT_TRUE(jce_platform_lb_submit(svc, "tie", "third",  500));

    n = jce_platform_lb_query_top(svc, "tie", 4, scores, &users[0][0], 32);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("first",  users[0]);
    TEST_ASSERT_EQUAL_STRING("second", users[1]);
    TEST_ASSERT_EQUAL_STRING("third",  users[2]);

    jce_platform_shutdown(svc);
}

/* ── Cloud-save slots ────────────────────────────────────────────────────── */

static void test_cloud_save_roundtrip_and_list(void)
{
    /* Binary payload incl. embedded NUL to prove byte-exactness. */
    const unsigned char payload[] = { 0xDE, 0xAD, 0x00, 0xBE, 0xEF, 0x42, 0x00, 0x99 };
    unsigned char readback[64];
    uint32_t got = 0;
    char names[8][64];
    int  count, i;
    bool saw_a = false, saw_b = false;

    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_FALSE(jce_platform_save_exists(svc, "slot_a"));

    TEST_ASSERT_TRUE(jce_platform_save_write(svc, "slot_a", payload, (uint32_t)sizeof payload));
    TEST_ASSERT_TRUE(jce_platform_save_exists(svc, "slot_a"));

    /* Size-only query. */
    got = 0;
    TEST_ASSERT_FALSE(jce_platform_save_read(svc, "slot_a", NULL, 0, &got));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof payload, got);

    /* Full read returns identical bytes. */
    memset(readback, 0, sizeof readback);
    got = 0;
    TEST_ASSERT_TRUE(jce_platform_save_read(svc, "slot_a", readback, sizeof readback, &got));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof payload, got);
    TEST_ASSERT_EQUAL_MEMORY(payload, readback, sizeof payload);

    /* A second slot, then list reflects both. */
    TEST_ASSERT_TRUE(jce_platform_save_write(svc, "slot_b", "hello", 5));
    count = jce_platform_save_list(svc, &names[0][0], 8, 64);
    TEST_ASSERT_EQUAL_INT(2, count);
    for (i = 0; i < count; ++i) {
        if (strcmp(names[i], "slot_a") == 0) saw_a = true;
        if (strcmp(names[i], "slot_b") == 0) saw_b = true;
    }
    TEST_ASSERT_TRUE(saw_a);
    TEST_ASSERT_TRUE(saw_b);

    /* Delete removes the slot. */
    TEST_ASSERT_TRUE(jce_platform_save_delete(svc, "slot_a"));
    TEST_ASSERT_FALSE(jce_platform_save_exists(svc, "slot_a"));
    TEST_ASSERT_EQUAL_INT(1, jce_platform_save_list(svc, NULL, 0, 0));

    /* Read of a missing slot fails and zeroes the size out. */
    got = 123;
    TEST_ASSERT_FALSE(jce_platform_save_read(svc, "slot_a", readback, sizeof readback, &got));
    TEST_ASSERT_EQUAL_UINT32(0, got);

    jce_platform_shutdown(svc);
}

static void test_cloud_save_persists_across_reload(void)
{
    unsigned char readback[32];
    uint32_t got = 0;
    JcePlatformServices *svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);

    TEST_ASSERT_TRUE(jce_platform_save_write(svc, "campaign", "SAVEDATA", 8));
    jce_platform_shutdown(svc);

    svc = jce_platform_init_local(g_root);
    TEST_ASSERT_NOT_NULL(svc);
    TEST_ASSERT_TRUE(jce_platform_save_exists(svc, "campaign"));
    memset(readback, 0, sizeof readback);
    TEST_ASSERT_TRUE(jce_platform_save_read(svc, "campaign", readback, sizeof readback, &got));
    TEST_ASSERT_EQUAL_UINT32(8, got);
    TEST_ASSERT_EQUAL_MEMORY("SAVEDATA", readback, 8);

    jce_platform_shutdown(svc);
}

/* ── NULL-safety ─────────────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    jce_platform_shutdown(NULL);                                  /* no crash */
    TEST_ASSERT_FALSE(jce_platform_flush(NULL));
    TEST_ASSERT_FALSE(jce_platform_ach_unlock(NULL, "x"));
    TEST_ASSERT_FALSE(jce_platform_ach_is_unlocked(NULL, "x"));
    TEST_ASSERT_EQUAL_INT64(7, jce_platform_stat_get(NULL, "x", 7));
    TEST_ASSERT_EQUAL_INT(0, jce_platform_lb_query_top(NULL, "b", 4, NULL, NULL, 0));
    TEST_ASSERT_FALSE(jce_platform_save_exists(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(0, jce_platform_save_list(NULL, NULL, 0, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_and_user_id);
    RUN_TEST(test_achievement_unlock_and_idempotent);
    RUN_TEST(test_achievement_progress_autounlock);
    RUN_TEST(test_persistence_across_reload);
    RUN_TEST(test_leaderboard_top_n_ordering);
    RUN_TEST(test_leaderboard_tie_break_stable);
    RUN_TEST(test_cloud_save_roundtrip_and_list);
    RUN_TEST(test_cloud_save_persists_across_reload);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
