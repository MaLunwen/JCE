/*
 * test_jce_scene_entity_generation.c — dangling entity handles must be
 * detected, including for generation 0 (audit C5-01 / C5-04).
 *
 * A JceEntity is an index in the low 32 bits and a generation in the high 32.
 * jce_scene_resolve_entity used to branch on "high bits zero => the caller
 * stripped the generation, look up whoever is in that slot now".  A
 * FIRST-GENERATION entity has generation 0, so its full and perfectly valid
 * handle is bit-identical to a bare index — every gen-0 handle took the
 * stripped path.  Destroy such an entity, let flecs recycle the index, and a
 * stale handle resolved to the NEW occupant: reads returned another entity's
 * components, and jce_scene_set_* WROTE to it.
 *
 * Generation 0 is not a corner: it covers every entity in a scene that has
 * not recycled an index yet, which is most of them for most of a session.
 *
 * These tests are written to fail on the old behaviour rather than to
 * describe the new one — the recycling test in particular asserts on the
 * VALUE observed through the stale handle, so "resolved to the wrong entity"
 * is distinguishable from "correctly refused".
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static JceScene *g_s;

void setUp(void)    { g_s = jce_scene_create(); }
void tearDown(void) { if (g_s) { jce_scene_destroy(g_s); g_s = NULL; } }

/* Force index reuse: create, destroy, create again until a new entity lands
 * on the same index as the destroyed one.  Returns 0 if flecs never reused
 * the slot within the budget (then the caller skips rather than pretends). */
static JceEntity recycle_index_of(JceEntity dead, int budget)
{
    const uint32_t want = (uint32_t)dead;
    for (int i = 0; i < budget; ++i) {
        JceEntity e = jce_scene_create_entity(g_s, "recycled");
        if ((uint32_t)e == want) return e;
    }
    return 0;
}

static void test_live_entity_is_alive(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "live");
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, e);
    TEST_ASSERT_TRUE(jce_scene_entity_alive(g_s, e));

    /* The predicate must be honest about its degenerate inputs. */
    TEST_ASSERT_FALSE(jce_scene_entity_alive(g_s, 0));
    TEST_ASSERT_FALSE(jce_scene_entity_alive(NULL, e));
}

static void test_destroyed_entity_is_not_alive(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "doomed");
    TEST_ASSERT_TRUE(jce_scene_entity_alive(g_s, e));

    jce_scene_destroy_entity(g_s, e);
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_entity_alive(g_s, e),
        "a destroyed entity still reports alive");
}

/* The one that matters: the first entity in a scene has generation 0, so its
 * handle looks exactly like a bare index. */
static void test_generation_zero_handle_is_generation_checked(void)
{
    JceEntity first = jce_scene_create_entity(g_s, "gen0");
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, first);
    /* Precondition: this really IS a generation-0 handle, or the test is
       exercising something else entirely. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, (uint32_t)(first >> 32),
        "test precondition failed: the first entity is not generation 0");

    jce_scene_destroy_entity(g_s, first);

    JceEntity reused = recycle_index_of(first, 4096);
    if (reused == 0)
        TEST_IGNORE_MESSAGE("flecs did not recycle the index within budget");

    /* The slot is live again, under a NEW handle... */
    TEST_ASSERT_TRUE(jce_scene_entity_alive(g_s, reused));
    TEST_ASSERT_NOT_EQUAL_UINT64_MESSAGE(first, reused,
        "recycled entity kept the same handle — generation did not advance");

    /* ...but the OLD handle must not come back to life. */
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_entity_alive(g_s, first),
        "stale generation-0 handle resolved to the recycled slot's new "
        "occupant — the dangling check is bypassed for gen-0 entities");
}

/* Component access through a stale gen-0 handle must not touch the entity
 * that now owns the slot.  This is the consequence that actually corrupts
 * state, so assert on observed VALUES, not just on a bool. */
static void test_stale_handle_cannot_read_or_write_the_new_occupant(void)
{
    JceEntity first = jce_scene_create_entity(g_s, "gen0-rw");
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)(first >> 32));

    jce_scene_destroy_entity(g_s, first);
    JceEntity reused = recycle_index_of(first, 4096);
    if (reused == 0)
        TEST_IGNORE_MESSAGE("flecs did not recycle the index within budget");

    /* Give the NEW occupant a distinctive transform. */
    JceTransform want;
    memset(&want, 0, sizeof want);
    want.position.x = 111.0f;
    want.scale.x = want.scale.y = want.scale.z = 1.0f;
    jce_scene_set_transform(g_s, reused, &want);
    TEST_ASSERT_TRUE(jce_scene_has_transform(g_s, reused));

    /* A read through the stale handle must not hand back the new occupant. */
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_has_transform(g_s, first),
        "stale handle reported the RECYCLED entity's component as its own");
    TEST_ASSERT_NULL_MESSAGE(jce_scene_get_transform(g_s, first),
        "stale handle returned a pointer into the recycled entity's data");

    /* A write through the stale handle must not land on the new occupant. */
    JceTransform poison;
    memset(&poison, 0, sizeof poison);
    poison.position.x = -999.0f;
    poison.scale.x = poison.scale.y = poison.scale.z = 1.0f;
    jce_scene_set_transform(g_s, first, &poison);

    JceTransform *now = jce_scene_get_transform(g_s, reused);
    TEST_ASSERT_NOT_NULL(now);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(111.0f, now->position.x,
        "a write through a stale handle overwrote the entity that now owns "
        "the recycled index");
}

/* jce_scene_entity_from_index is the documented escape hatch: it CANNOT
 * detect staleness, and the test says so rather than pretending otherwise. */
static void test_from_index_resolves_current_occupant_by_design(void)
{
    JceEntity first = jce_scene_create_entity(g_s, "byindex");
    const uint32_t idx = (uint32_t)first;

    TEST_ASSERT_EQUAL_UINT64(first, jce_scene_entity_from_index(g_s, idx));

    jce_scene_destroy_entity(g_s, first);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, jce_scene_entity_from_index(g_s, idx),
        "an index with no live occupant should resolve to 0");

    JceEntity reused = recycle_index_of(first, 4096);
    if (reused == 0)
        TEST_IGNORE_MESSAGE("flecs did not recycle the index within budget");

    /* By design, and the reason new code must keep the full handle: the same
       index now names a different entity, and this call cannot tell. */
    TEST_ASSERT_EQUAL_UINT64(reused, jce_scene_entity_from_index(g_s, idx));

    TEST_ASSERT_EQUAL_UINT64(0u, jce_scene_entity_from_index(g_s, 0));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_scene_entity_from_index(NULL, idx));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_live_entity_is_alive);
    RUN_TEST(test_destroyed_entity_is_not_alive);
    RUN_TEST(test_generation_zero_handle_is_generation_checked);
    RUN_TEST(test_stale_handle_cannot_read_or_write_the_new_occupant);
    RUN_TEST(test_from_index_resolves_current_occupant_by_design);
    return UNITY_END();
}
