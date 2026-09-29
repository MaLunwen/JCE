/* test_jce_scene_reparent_names.c
 *
 * Creating N entities with the SAME name and parenting them to one node
 * ABORTED THE PROCESS.
 *
 * jce_scene_create_entity already knew flecs's name index is unique per scope
 * and uniquified on collision -- but it checks the scope the entity is created
 * in, which is always the root, and the entity is then MOVED.  Create-then-
 * parent is the ordinary order, so the check ran against a scope the name
 * never had to be unique in:
 *
 *     a = create("Row");  set_parent(a, list);   // "Row" leaves the root
 *     b = create("Row");  // root lookup finds nothing, keeps the plain name
 *     set_parent(b, list);                        // two "Row" here -> ABORT
 *
 * flecs aborts with no return value to check and nothing in the log, so it
 * surfaces as a crash in whatever ran next.  This is exactly how it was found:
 * a UI test building a list of rows died at 0xC0000409 and read as a bug in
 * the code under test.
 *
 * A LIVE-ABORT DEFECT MAKES ITS OWN NEGATIVE CONTROL.  There is no need to
 * reason about whether these assertions would have caught it: before the fix
 * this file does not report a failure, it kills the runner.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. the case that aborted ─────────────────────────────────────────── */
static void test_many_same_named_children_under_one_parent(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity list = jce_scene_create_entity(s, "List");

    enum { N = 64 };
    JceEntity rows[N];
    for (int i = 0; i < N; i++) {
        rows[i] = jce_scene_create_entity(s, "Row");
        jce_scene_set_parent(s, rows[i], list);
    }

    /* Every one of them is a distinct, live child of the list. */
    JceEntity kids[N];
    const int n = jce_scene_get_children(s, list, kids, N);
    TEST_ASSERT_EQUAL_INT_MESSAGE(N, n,
        "every same-named child must survive the reparent");
    for (int i = 0; i < N; i++)
        TEST_ASSERT_EQUAL_UINT64(jce_scene_get_parent(s, rows[i]),
                                 (uint64_t)list);

    jce_scene_destroy(s);
}

/* ── 2. the rename must not be observable ─────────────────────────────── */
static void test_the_authored_name_is_untouched(void)
{
    /* The fix renames the internal flecs name to keep the scope's index
     * valid.  If that reached the public API, saving the scene would write
     * "Row_470" and every lookup by the authored name would miss -- which is
     * the bug the authored-name component was introduced to fix in the first
     * place.  So the rename is only correct while this stays true. */
    JceScene *s = jce_scene_create();
    JceEntity list = jce_scene_create_entity(s, "List");

    JceEntity a = jce_scene_create_entity(s, "Row");
    JceEntity b = jce_scene_create_entity(s, "Row");
    jce_scene_set_parent(s, a, list);
    jce_scene_set_parent(s, b, list);

    TEST_ASSERT_EQUAL_STRING_MESSAGE("Row", jce_scene_entity_name(s, a),
        "the first child's authored name changed");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Row", jce_scene_entity_name(s, b),
        "the second child's authored name was uniquified into the public API");

    jce_scene_destroy(s);
}

/* ── 3. moving back out to the root ───────────────────────────────────── */
static void test_unparenting_into_an_occupied_root_is_also_safe(void)
{
    /* The root is a scope too.  A child moved OUT of a group lands beside
     * whatever is already at the root, so the same rule has to apply on the
     * way out -- and it is the direction easiest to forget, because the
     * collision is with entities the caller never mentioned. */
    JceScene *s = jce_scene_create();
    JceEntity list = jce_scene_create_entity(s, "List");

    JceEntity at_root = jce_scene_create_entity(s, "Row");   /* stays at root */
    JceEntity in_list = jce_scene_create_entity(s, "Row");
    jce_scene_set_parent(s, in_list, list);

    jce_scene_set_parent(s, in_list, JCE_ENTITY_INVALID);    /* back to root */

    TEST_ASSERT_EQUAL_UINT64((uint64_t)JCE_ENTITY_INVALID,
                             jce_scene_get_parent(s, in_list));
    TEST_ASSERT_TRUE(at_root != in_list);
    TEST_ASSERT_EQUAL_STRING("Row", jce_scene_entity_name(s, at_root));
    TEST_ASSERT_EQUAL_STRING("Row", jce_scene_entity_name(s, in_list));

    jce_scene_destroy(s);
}

/* ── 4. distinct names are left exactly as authored ───────────────────── */
static void test_distinct_names_are_not_renamed(void)
{
    /* The negative control for the fix itself: uniquifying is only allowed to
     * happen on an actual collision.  A fix that renamed on every reparent
     * would pass all three cases above and be wrong. */
    JceScene *s = jce_scene_create();
    JceEntity list = jce_scene_create_entity(s, "List");

    JceEntity a = jce_scene_create_entity(s, "Head");
    JceEntity b = jce_scene_create_entity(s, "Body");
    jce_scene_set_parent(s, a, list);
    jce_scene_set_parent(s, b, list);

    TEST_ASSERT_EQUAL_STRING("Head", jce_scene_entity_name(s, a));
    TEST_ASSERT_EQUAL_STRING("Body", jce_scene_entity_name(s, b));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_many_same_named_children_under_one_parent);
    RUN_TEST(test_the_authored_name_is_untouched);
    RUN_TEST(test_unparenting_into_an_occupied_root_is_also_safe);
    RUN_TEST(test_distinct_names_are_not_renamed);
    return UNITY_END();
}
