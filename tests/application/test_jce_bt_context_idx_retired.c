/*
 * test_jce_bt_context_idx_retired.c — JceBehaviorTree.context_handle_idx is
 * retired, and this is what "retired" has to mean at runtime.
 *
 * The field asked a scene component to select one of several JceBtContexts.
 * jce_bt.h gives an opaque create/destroy with no index space at all, and the
 * runtime owns exactly one context, published as jce_runtime_bt_context().  So
 * there is nothing to select, no writer anywhere in engine/src, editor/src,
 * scripting/ or tools/, and no JSON key in either direction: the field has been
 * 0 in every process and every file that has ever existed.
 *
 * A RETIRED marker in a header is a comment, and a comment cannot be relied on.
 * Two things make it a commitment instead:
 *   - check_component_field_consumed.py fails if any serializer, inspector
 *     panel or defaults blob names a field marked RETIRED (its two negative
 *     controls are in the commit message);
 *   - this test, which asserts the runtime consequence: a value put into the
 *     field cannot survive the only channel that persists anything.
 *
 * The neighbouring field is asserted in the same round trip on purpose.  A save
 * that dropped EVERYTHING would satisfy "the 7 is gone" for the wrong reason.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>

#include <stdbool.h>
#include <string.h>

static JceEntity g_found;

static void find_bt_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)ud;
    if (g_found == JCE_ENTITY_INVALID && jce_scene_get_behavior_tree(s, e))
        g_found = e;
}

/* Author a BehaviorTree with `ctx`, save the scene, load it into a fresh one,
 * and hand back what came out. */
static JceBehaviorTree roundtrip(uint32_t ctx)
{
    JceScene *src = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);

    JceEntity e = jce_scene_create_entity(src, "Brain");
    JceBehaviorTree bt;
    memset(&bt, 0, sizeof bt);
    snprintf(bt.tree_path, sizeof bt.tree_path, "ai/patrol.xml");
    bt.context_handle_idx = ctx;
    bt.tick_hz            = 12.0f;
    bt.active             = true;
    jce_scene_set_behavior_tree(src, e, &bt);

    JceJson *doc = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(doc);
    jce_scene_destroy(src);

    JceScene *dst = jce_scene_create();
    int loaded = jce_scene_load_json(dst, doc);
    jce_json_free(doc);
    TEST_ASSERT_TRUE_MESSAGE(loaded > 0, "the saved scene must load back");

    g_found = JCE_ENTITY_INVALID;
    jce_scene_each_entity(dst, find_bt_cb, NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_found != JCE_ENTITY_INVALID,
        "the reloaded scene must still carry a BehaviorTree");

    JceBehaviorTree out = *jce_scene_get_behavior_tree(dst, g_found);
    jce_scene_destroy(dst);
    return out;
}

static void test_the_neighbours_do_survive(void)
{
    /* The control for the assertion below: this component round-trips. */
    JceBehaviorTree out = roundtrip(0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ai/patrol.xml", out.tree_path,
        "if the whole component were dropped, 'the 7 is gone' would be true "
        "for the wrong reason");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12.0f, out.tick_hz, "tick_hz round-trips");
    TEST_ASSERT_TRUE_MESSAGE(out.active, "active round-trips");
}

static void test_context_idx_cannot_be_authored(void)
{
    JceBehaviorTree out = roundtrip(7);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, out.context_handle_idx,
        "a RETIRED field must not survive a save and a load -- the serializer "
        "writes no key for it and the parser reads none, which is what makes "
        "the marker a commitment rather than a comment");
}

static void test_zero_is_unremarkable(void)
{
    JceBehaviorTree out = roundtrip(0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, out.context_handle_idx,
        "0 is the only value that has ever existed, in any process or file");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_neighbours_do_survive);
    RUN_TEST(test_context_idx_cannot_be_authored);
    RUN_TEST(test_zero_is_unremarkable);
    return UNITY_END();
}
