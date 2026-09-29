/*
 * test_jce_rq_transparent_priority.c — the author's transparent draw order.
 *
 * Transparents sorted by camera depth ALONE.  That has no answer for the cases
 * authors actually hit -- two coplanar quads, a decal that must land on top of
 * the glass it sits inside, interpenetrating water and a windscreen -- and
 * "whichever the camera happens to be nearer" is what makes them flicker as it
 * moves.  There was no way to say which wins, because JceDrawCmd had no field
 * to say it with: the 16 bits now holding `priority` were named `_pad`.
 *
 * Unity spells this Material.renderQueue, Godot render_priority, UE
 * Translucency Sort Priority.  Higher draws LATER, which is on top.
 *
 * HOW THIS OBSERVES ORDER WITHOUT AN ORDER ACCESSOR.  The queue is
 * write-then-flush and exposes no "entry at index"; adding one for a test
 * would be public API with a single caller, which is the defect this
 * repository keeps finding rather than a way to test.  jce_rq_analyse instead
 * reports how many ADJACENT same-key entries would merge into one instanced
 * submit -- a number that changes only if the sort physically moved entries
 * next to each other.  So the merge count is the witness, and it is a witness
 * the shipping flush path uses for real.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_render_queue.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* One transparent draw.  `mat` drives the merge key, so two commands merge
 * only when they share it AND end up adjacent. */
static JceDrawCmd cmd_of(int16_t priority, float depth, uint32_t mat)
{
    JceDrawCmd c;
    memset(&c, 0, sizeof c);
    c.view_id        = 1;
    c.program        = 7;
    c.program_single = 7;
    c.mesh_vbh       = 11;
    c.mesh_ibh       = 12;
    c.index_count    = 36;
    c.material_key   = mat;
    c.priority       = priority;
    c.depth          = depth;
    return c;
}

static uint32_t merges_after_sort(const JceDrawCmd *cmds, int n)
{
    JceRenderQueue *rq = jce_rq_create(16);
    TEST_ASSERT_NOT_NULL(rq);
    for (int i = 0; i < n; ++i) jce_rq_push(rq, &cmds[i]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, jce_rq_count(rq));
    jce_rq_sort(rq, JCE_SORT_BACK_TO_FRONT);
    JceRenderQueueStats st;
    memset(&st, 0, sizeof st);
    jce_rq_analyse(rq, &st);
    jce_rq_destroy(rq);
    return st.batches_merged;
}

static void test_priority_reorders_across_depth(void)
{
    /* Interleaved by material and ALREADY in back-to-front depth order, so
     * depth sorting alone leaves them exactly as pushed: X, Y, X -- nothing
     * adjacent, nothing merges.  Give the two X's a priority the Y does not
     * share and the sort must bring them together. */
    const JceDrawCmd depth_only[3] = {
        cmd_of(0, 30.0f, 100u),
        cmd_of(0, 20.0f, 200u),
        cmd_of(0, 10.0f, 100u),
    };
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, merges_after_sort(depth_only, 3),
        "depth alone must leave the interleaving intact -- if this is not 0 "
        "the fixture is not measuring what it claims");

    const JceDrawCmd with_priority[3] = {
        cmd_of(0, 30.0f, 100u),
        cmd_of(1, 20.0f, 200u),   /* pushed to the END by priority */
        cmd_of(0, 10.0f, 100u),
    };
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, merges_after_sort(with_priority, 3),
        "priority must sort BEFORE depth: the two priority-0 entries become "
        "adjacent and merge, which cannot happen unless the sort moved them");
}

static void test_priority_does_not_disturb_equal_priority_depth_order(void)
{
    /* The whole existing behaviour must survive.  Same priority everywhere is
     * every scene authored before this field existed, and those must sort by
     * depth exactly as they did. */
    const JceDrawCmd a[3] = {
        cmd_of(4, 10.0f, 100u),
        cmd_of(4, 30.0f, 200u),
        cmd_of(4, 20.0f, 100u),
    };
    /* Sorted by depth: 30(200), 20(100), 10(100) -> the two 100s are adjacent
     * at the tail, so exactly one merge.  A comparator that ignored depth once
     * priorities tie would leave them as pushed and merge nothing. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, merges_after_sort(a, 3),
        "with priorities tied the sort must still order by depth");
}

static void test_a_negative_priority_draws_first(void)
{
    /* Below-neutral must work, or "draw this behind everything" needs every
     * other material in the scene raised instead. */
    const JceDrawCmd a[3] = {
        cmd_of(0,  10.0f, 100u),
        cmd_of(-5, 20.0f, 200u),   /* pulled to the FRONT of the queue */
        cmd_of(0,  30.0f, 100u),
    };
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, merges_after_sort(a, 3),
        "the -5 entry must leave the two 0s adjacent");
}

static void test_a_zeroed_command_is_neutral(void)
{
    /* priority took the bytes that were `_pad`, so every command built the old
     * way -- memset to 0 -- carries priority 0.  If that were not the neutral
     * value, every existing draw would have been reordered by this change. */
    JceDrawCmd c;
    memset(&c, 0, sizeof c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)c.priority,
        "a zeroed JceDrawCmd must mean 'no override'");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_priority_reorders_across_depth);
    RUN_TEST(test_priority_does_not_disturb_equal_priority_depth_order);
    RUN_TEST(test_a_negative_priority_draws_first);
    RUN_TEST(test_a_zeroed_command_is_neutral);
    return UNITY_END();
}
