/*
 * test_jce_save_point_slot.c — JceSavePointComponent.slot picks the file.
 *
 * slot, kind and display_name were all authored, serialised and read by
 * nothing.  Every save point in a scene wrote "<save_id>.jsnp" however many
 * slots the designer laid out.
 *
 * This exercises rt_save_path -- the engine's own function, not a copy of its
 * arithmetic.  The first version of this test reimplemented the format string
 * and would have proved only that the two copies agreed with each other.
 *
 * kind and display_name reach the game as an on_save_point script message: a
 * require_interact point previously told it nothing at all, so the prompt
 * display_name exists for could not be shown.  That path needs a live script
 * host and is not reachable from here.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "application/jce_rt_internal.h"

#include <string.h>

static void test_unset_slot_keeps_the_historical_name(void)
{
    char p[256];
    TEST_ASSERT_TRUE(rt_save_path(p, sizeof p, "saves", "checkpoint", -1));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("saves/checkpoint.jsnp", p,
        "slot -1 is the parser's default, so every save point authored before "
        "this must still write the file it always wrote");
}

static void test_slots_are_distinct_files(void)
{
    char a[256], b[256];
    TEST_ASSERT_TRUE(rt_save_path(a, sizeof a, "saves", "checkpoint", 0));
    TEST_ASSERT_TRUE(rt_save_path(b, sizeof b, "saves", "checkpoint", 1));
    TEST_ASSERT_EQUAL_STRING("saves/checkpoint.slot0.jsnp", a);
    TEST_ASSERT_EQUAL_STRING("saves/checkpoint.slot1.jsnp", b);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(a, b) != 0,
        "two slots must not collide -- collapsing them is exactly what the "
        "field did before anything read it");
}

static void test_slot_zero_is_not_the_unset_path(void)
{
    char z[256], u[256];
    TEST_ASSERT_TRUE(rt_save_path(z, sizeof z, "saves", "cp", 0));
    TEST_ASSERT_TRUE(rt_save_path(u, sizeof u, "saves", "cp", -1));
    TEST_ASSERT_TRUE_MESSAGE(strcmp(z, u) != 0,
        "slot 0 is an AUTHORED slot and must not silently mean 'unset' -- "
        "that conflation is why the parser defaults to -1");
}

static void test_overflow_is_refused_not_truncated(void)
{
    char tiny[8];
    TEST_ASSERT_FALSE_MESSAGE(
        rt_save_path(tiny, sizeof tiny, "saves", "a_very_long_save_id", 3),
        "a path that does not fit must be REFUSED; a truncated one would "
        "silently write to the wrong file");
    TEST_ASSERT_FALSE(rt_save_path(NULL, 64, "saves", "x", 0));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unset_slot_keeps_the_historical_name);
    RUN_TEST(test_slots_are_distinct_files);
    RUN_TEST(test_slot_zero_is_not_the_unset_path);
    RUN_TEST(test_overflow_is_refused_not_truncated);
    return UNITY_END();
}
