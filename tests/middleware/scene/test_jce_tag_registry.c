/*
 * test_jce_tag_registry.c — a tag name the registry cannot hold must not
 * silently mint a new slot on every lookup.
 *
 * THE MECHANISM, verified line by line before it was changed.  jce_tag_intern
 * looks a name up by comparing the FULL argument against the STORED name, then
 * stores with snprintf into a JCE_TAG_NAME_MAX buffer.  So for any name longer
 * than the buffer the two halves disagreed by construction:
 *
 *     strcmp(stored_truncated, full_name) != 0   -> never a hit
 *     snprintf(slot, MAX, "%s", full_name)       -> another truncated copy
 *
 * Interning the same long name twice returned two different ids, and every
 * jce_scene_find_with_tag() on it burned another of the 1024 slots.  A
 * per-frame gameplay query on a long tag therefore exhausted the process-wide
 * registry, after which every OTHER tag interned as 0 = Untagged.
 *
 * JceEditorMeta.tag is char[64] and the Inspector writes it with
 * sizeof(m->tag), so a 40-character tag was authorable -- the two limits were
 * 32 and 64 and nothing reconciled them.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>
#include <string.h>

/* 40 characters: longer than the old 32-byte slot, shorter than the new 64. */
static const char *k_long =
    "AnEnemySpawnerForTheNorthGateAreaLevel12";

/* 80 characters: longer than any slot this registry will ever hold. */
static const char *k_absurd =
    "AnEnemySpawnerForTheNorthGateAreaLevel12ThatAlsoDoesSomethingElseEntirelyOkFine!";

static void test_a_long_tag_interns_to_one_stable_id(void)
{
    TEST_ASSERT_TRUE_MESSAGE(strlen(k_long) > 32,
        "the fixture must exceed the OLD cap, or this asserts nothing");
    const uint16_t a = jce_tag_intern(k_long);
    const uint16_t b = jce_tag_intern(k_long);
    TEST_ASSERT_TRUE_MESSAGE(a != 0, "a 40-char tag must be representable");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(a, b,
        "interning the same name twice must give the same id -- it did not, "
        "because the lookup compared the full name against a truncated store");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(k_long, jce_tag_name(a),
        "and the name must come back whole, not as a 31-character prefix");
}

static void test_lookup_does_not_burn_a_slot_per_call(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Spawner");
    jce_scene_set_entity_tag_name(s, e, k_long);

    const int before = jce_tag_count();
    for (int i = 0; i < 200; ++i)
        (void)jce_scene_find_with_tag(s, k_long);
    const int after = jce_tag_count();

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
        "200 lookups of one tag must not add 200 registry slots -- the "
        "registry holds 1024, so a per-frame query used to exhaust it and "
        "then EVERY tag started interning as Untagged");
    jce_scene_destroy(s);
}

static void test_a_long_tag_is_findable(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Spawner");
    jce_scene_set_entity_tag_name(s, e, k_long);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)e,
        (uint64_t)jce_scene_find_with_tag(s, k_long),
        "the whole point of a tag: gameplay must be able to find it");
    jce_scene_destroy(s);
}

static void test_a_name_that_cannot_fit_is_untagged_and_stays_untagged(void)
{
    /* Untagged is the honest answer for a name the registry cannot hold, and
     * the important half is STABLE: the same name must give the same answer
     * every time, rather than a fresh slot per call. */
    const int before = jce_tag_count();
    const uint16_t a = jce_tag_intern(k_absurd);
    const uint16_t b = jce_tag_intern(k_absurd);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, a, "too long must read as Untagged");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(a, b, "and must do so consistently");
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, jce_tag_count(),
        "refusing must cost no slot at all");
}

static void test_short_tags_are_untouched(void)
{
    /* Everything the default presets and the project tag list hold in practice
     * is short.  Those must intern exactly as before. */
    const uint16_t p = jce_tag_intern("Player");
    const uint16_t q = jce_tag_intern("Player");
    TEST_ASSERT_TRUE(p != 0);
    TEST_ASSERT_EQUAL_UINT16(p, q);
    TEST_ASSERT_EQUAL_STRING("Player", jce_tag_name(p));

    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "P");
    jce_scene_set_entity_tag_name(s, e, "Player");
    TEST_ASSERT_EQUAL_UINT64((uint64_t)e,
                             (uint64_t)jce_scene_find_with_tag(s, "Player"));
    jce_scene_destroy(s);
}

static void test_the_empty_tag_is_untagged(void)
{
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, jce_tag_intern(""),
        "the zero value must stay Untagged");
    TEST_ASSERT_EQUAL_UINT16(0, jce_tag_intern(NULL));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_long_tag_interns_to_one_stable_id);
    RUN_TEST(test_lookup_does_not_burn_a_slot_per_call);
    RUN_TEST(test_a_long_tag_is_findable);
    RUN_TEST(test_a_name_that_cannot_fit_is_untagged_and_stays_untagged);
    RUN_TEST(test_short_tags_are_untouched);
    RUN_TEST(test_the_empty_tag_is_untagged);
    return UNITY_END();
}
