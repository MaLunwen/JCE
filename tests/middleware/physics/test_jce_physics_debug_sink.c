/* test_jce_physics_debug_sink.c
 *
 * THE CLAIM: a shipped game can draw its colliders.
 *
 * What this file can prove and what it cannot, said out loud, because the
 * difference is where this kind of wiring usually goes wrong.  It CAN prove
 * that jce_physics_debug_flush delivers real geometry to an installed sink --
 * that is CPU-side and needs no device.  It CANNOT prove pixels: that the
 * lines reach a backbuffer is jce_debug_draw_flush's job and needs a
 * renderer, which this machine cannot currently be asked for.  So the pixel
 * half is NOT claimed here or in the ledger row.
 *
 * THE LOAD-BEARING ASSERTION IS THE ONE ABOUT COUNTS.  "The sink received
 * lines" is equally true of a debug drawer that emits a fixed preamble and
 * never looks at the world.  So the test compares an EMPTY world against one
 * with bodies, and two bodies against one -- geometry that scales with the
 * bodies is geometry ABOUT the bodies.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_debug.h>

#include <stdbool.h>
#include <string.h>

static int      g_lines;
static jce_vec3 g_first_from, g_first_to;

static void count_line(jce_vec3 from, jce_vec3 to, uint32_t abgr, void *ud)
{
    (void)abgr;
    if (ud) *(int *)ud += 1;
    if (g_lines == 0) { g_first_from = from; g_first_to = to; }
    ++g_lines;
}

static int g_user_hits;

void setUp(void)
{
    g_lines = 0;
    g_user_hits = 0;
    memset(&g_first_from, 0, sizeof g_first_from);
    memset(&g_first_to, 0, sizeof g_first_to);
    jce_physics_debug_set_flags(JCE_PHYS_DBG_NONE);
    jce_physics_debug_set_line_sink(NULL, NULL);
}

void tearDown(void)
{
    jce_physics_debug_set_flags(JCE_PHYS_DBG_NONE);
    jce_physics_debug_set_line_sink(NULL, NULL);
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_bodies = 8u;
    return jce_physics_create(&wd);
}

static void add_box(JcePhysicsWorld *w, float x)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = JCE_BODY_STATIC;
    bd.shape = JCE_SHAPE_BOX;
    bd.position.x = x;
    bd.rotation.w = 1.0f;
    bd.half_extents.x = 0.5f;
    bd.half_extents.y = 0.5f;
    bd.half_extents.z = 0.5f;
    bd.collision_group = 0xFFFFFFFFu;
    bd.collision_mask  = 0xFFFFFFFFu;
    (void)jce_physics_body_create(w, &bd);
}

/* Lines delivered for a world holding `bodies` boxes, with `flags` set. */
static int flush_lines(int bodies, uint32_t flags)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    for (int i = 0; i < bodies; ++i)
        add_box(w, (float)i * 4.0f);

    g_lines = 0;
    jce_physics_debug_set_line_sink(count_line, &g_user_hits);
    jce_physics_debug_set_flags(flags);
    jce_physics_debug_flush(w);
    const int n = g_lines;

    jce_physics_debug_set_flags(JCE_PHYS_DBG_NONE);
    jce_physics_debug_set_line_sink(NULL, NULL);
    jce_physics_destroy(w);
    return n;
}

/* ---------------------------------------------------------------------- */

static void test_a_sink_receives_wireframe_geometry(void)
{
    const int n = flush_lines(1, JCE_PHYS_DBG_WIREFRAME);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n,
        "the installed sink received nothing for a world with one box -- "
        "which is what a shipped game saw before the default main installed "
        "a sink at all");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, g_user_hits,
        "the sink's userdata pointer never reached it");
}

static void test_the_geometry_is_about_the_bodies_not_a_preamble(void)
{
    /* THE ASSERTION THIS FILE EXISTS FOR.  A drawer that emits a fixed
     * preamble -- an origin gizmo, a world AABB -- and never walks the
     * bodies would pass the test above and draw nothing a developer is
     * looking for. */
    const int none = flush_lines(0, JCE_PHYS_DBG_WIREFRAME);
    const int one  = flush_lines(1, JCE_PHYS_DBG_WIREFRAME);
    const int two  = flush_lines(2, JCE_PHYS_DBG_WIREFRAME);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, none,
        "an EMPTY world produced lines, so what the sink receives is not "
        "the colliders");
    TEST_ASSERT_GREATER_THAN_INT(0, one);
    TEST_ASSERT_EQUAL_INT_MESSAGE(one * 2, two,
        "two identical boxes did not produce twice one box's lines -- the "
        "geometry does not scale with the bodies, so it is not theirs");
}

static void test_flags_none_costs_the_sink_nothing(void)
{
    /* The header promises flush is cheap while flags are NONE, and the
     * shipped main installs the sink UNCONDITIONALLY on that promise.  If
     * NONE still emitted, every shipped game would pay for a debug overlay
     * it never asked for. */
    const int n = flush_lines(2, JCE_PHYS_DBG_NONE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n,
        "flags NONE still pushed lines to the sink; the shipped main installs "
        "the sink for every game on the promise that it does not");
}

static void test_detaching_the_sink_silences_the_flush(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_box(w, 0.0f);

    jce_physics_debug_set_flags(JCE_PHYS_DBG_WIREFRAME);
    jce_physics_debug_set_line_sink(count_line, &g_user_hits);
    g_lines = 0;
    jce_physics_debug_flush(w);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, g_lines,
        "positive control failed: the sink was not receiving to begin with, "
        "so the silence asserted below would prove nothing");

    jce_physics_debug_set_line_sink(NULL, NULL);
    g_lines = 0;
    jce_physics_debug_flush(w);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_lines,
        "a detached sink still received lines");

    jce_physics_destroy(w);
}

static void test_the_flag_set_round_trips(void)
{
    jce_physics_debug_set_flags(JCE_PHYS_DBG_WIREFRAME | JCE_PHYS_DBG_AABB);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(JCE_PHYS_DBG_WIREFRAME | JCE_PHYS_DBG_AABB),
        jce_physics_debug_get_flags(),
        "the shipped main reads the flags back to decide whether to flush at "
        "all, so a setter that does not round-trip disables the whole path");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_sink_receives_wireframe_geometry);
    RUN_TEST(test_the_geometry_is_about_the_bodies_not_a_preamble);
    RUN_TEST(test_flags_none_costs_the_sink_nothing);
    RUN_TEST(test_detaching_the_sink_silences_the_flush);
    RUN_TEST(test_the_flag_set_round_trips);
    return UNITY_END();
}
