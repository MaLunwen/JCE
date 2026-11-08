/* Actual world-history owner, host generation, reset admission and retained
 * prepass predicate/publication. Typed allocation/counter ports only; no GPU. */
#include "unity.h"
#include "middleware/scene/jce_sr_internal.h"
#include <jce/os/core/jce_trace.h>
#include <stdio.h>
#include <string.h>

static JceSceneRenderer *sr;
static unsigned scene_tokens[2];
static const JceScene *scene;
static uint64_t counter;
static uint16_t active_view;
static unsigned allocations;
static bool refuse_allocation, cache_hit, captured_cache_identity;

static void *world_calloc(size_t count, size_t size)
{
    ++allocations;
    return refuse_allocation ? NULL : jce__calloc_tracked(count, size);
}
#define jce__calloc_tracked world_calloc
#include JCE_TEST_VELOCITY_WORLD_OWNER
#undef jce__calloc_tracked
#include JCE_TEST_VELOCITY_HOST_OWNER
#include JCE_TEST_VELOCITY_RESET_OWNER

static uint64_t counter_port(const JceScene *owner)
{
    TEST_ASSERT_EQUAL_PTR(scene, owner);
    return counter;
}
#define jce_scene_get_xform_counter counter_port
static void cache_attempt(bool write_velocity)
{
    const uint16_t view_id_base = active_view;
    const bool want_normals = false;
    const int s_pre_cache_env = 1;
    const jce_mat4 pre_vp = jce_m4_identity();
    cache_hit = true;
#include JCE_TEST_VELOCITY_CACHE_ADMISSION
    cache_hit = false;
    captured_cache_identity = pre_cache_identity;
}
#undef jce_scene_get_xform_counter
static void cache_publish(bool write_velocity)
{
    const uint16_t view_id_base = active_view;
    const bool want_normals = false;
    const int s_pre_cache_env = 1;
    const bool pre_cache_identity = captured_cache_identity;
    const uint64_t pre_lg = sr->frame_list_gen, pre_xf = counter;
    const jce_mat4 pre_vp = jce_m4_identity();
    struct SrPrepassCacheSlot *pcslot = &sr->pre_cache[view_id_base & 3u];
#include JCE_TEST_VELOCITY_CACHE_PUBLICATION
}

static jce_mat4 world(float x)
{
    jce_mat4 matrix = jce_m4_identity();
    matrix.raw[3][0] = x;
    return matrix;
}
static void previous(JceEntity entity, float x)
{
    const jce_mat4 expected = world(x);
    const jce_mat4 *actual = sr_prev_xform_find(sr, entity);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_MEMORY(&expected, actual, sizeof expected);
}
static void store(JceEntity entity, float x)
{
    const jce_mat4 matrix = world(x);
    sr_prev_xform_store(sr, entity, &matrix);
}
static void host(void)
{
    jce_scene_renderer_begin_velocity_frame(sr);
    sr_prev_xform_begin(sr, scene);
}
void setUp(void)
{
    jce_trace_set_enabled(false);
    sr = JCE_CALLOC(1, sizeof *sr); TEST_ASSERT_NOT_NULL(sr);
    scene = (const JceScene *)&scene_tokens[0];
    counter = 10; active_view = 7; allocations = 0;
    refuse_allocation = cache_hit = captured_cache_identity = false;
    sr->frame_list_gen = 1; sr->ssao_target_generation = 1;
    sr->ssao_w = 2560; sr->ssao_h = 1440; sr->depth_history_reused = true;
}
void tearDown(void)
{
    JCE_FREE(sr->prev_xform);
    JCE_FREE(sr); sr = NULL;
}
static void test_all_views_consume_one_previous_snapshot(void)
{
    host(); store(11, 0);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, 11));
    sr_prev_xform_begin(sr, scene); store(11, 0);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, 11));
    host(); previous(11, 0); store(11, 5); previous(11, 0);
    const uint32_t generation = sr->vel_frame_gen;
    sr_prev_xform_begin(sr, scene); previous(11, 0);
    store(11, 5); previous(11, 0);
    TEST_ASSERT_EQUAL_UINT32(generation, sr->vel_frame_gen);
    host(); previous(11, 5); store(11, 5); previous(11, 5);
}
static void test_view_union_prunes_without_breaking_wrapped_probe_chains(void)
{
    host(); sr_prev_xform_grow(sr, 1);
    JceEntity a = 1;
    while (sr_prev_xform_hash(a) % sr->prev_xform_cap != sr->prev_xform_cap - 1u) ++a;
    const JceEntity b = a + sr->prev_xform_cap, c = b + sr->prev_xform_cap;
    store(a, 1); store(b, 2); store(c, 3);
    host(); previous(a, 1); store(a, 1);
    sr_prev_xform_begin(sr, scene); previous(b, 2); store(b, 2);
    host(); TEST_ASSERT_NULL(sr_prev_xform_find(sr, c));
    TEST_ASSERT_EQUAL_UINT32(2, sr->prev_xform_count);
    previous(a, 1); previous(b, 2); store(b, 2);
    host(); TEST_ASSERT_NULL(sr_prev_xform_find(sr, a));
    TEST_ASSERT_EQUAL_UINT32(1, sr->prev_xform_count); previous(b, 2);
}
static void test_full_entity_generation_scene_change_and_reset(void)
{
    const JceEntity old = (UINT64_C(3) << 32) | 9u;
    const JceEntity fresh = (UINT64_C(4) << 32) | 9u;
    host(); store(old, 1);
    host(); TEST_ASSERT_NULL(sr_prev_xform_find(sr, fresh));
    previous(old, 1); store(fresh, 99);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, fresh));
    TEST_ASSERT_EQUAL_UINT32(2, sr->prev_xform_count);
    const uint32_t capacity = sr->prev_xform_cap;
    jce_scene_renderer_reset_entity_caches(sr);
    TEST_ASSERT_EQUAL_UINT32(0, sr->prev_xform_count);
    TEST_ASSERT_EQUAL_UINT32(capacity, sr->prev_xform_cap);
    TEST_ASSERT_FALSE(sr->prev_xform_gen_valid);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, old));
    sr_prev_xform_begin(sr, scene); store(old, 10);
    scene = (const JceScene *)&scene_tokens[1];
    sr_prev_xform_begin(sr, scene);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, old));
    TEST_ASSERT_EQUAL_UINT32(0, sr->prev_xform_count);
    jce_scene_renderer_reset_entity_caches(NULL);
}
static void test_unhosted_pass_and_existing_host_generation_wrap(void)
{
    sr_prev_xform_begin(sr, scene); store(5, 1);
    TEST_ASSERT_FALSE(sr->velocity_frame_driven);
    TEST_ASSERT_EQUAL_UINT32(1, sr->vel_frame_gen);
    sr_prev_xform_begin(sr, scene); previous(5, 1); store(5, 2);
    TEST_ASSERT_EQUAL_UINT32(2, sr->vel_frame_gen);
    sr->vel_frame_gen = UINT32_MAX - 1u;
    host(); previous(5, 2); store(5, 3);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sr->vel_frame_gen);
    host(); previous(5, 3); store(5, 4);
    sr_prev_xform_begin(sr, scene); previous(5, 3);
    TEST_ASSERT_EQUAL_UINT32(0, sr->vel_frame_gen);
}
static void test_growth_preserves_both_snapshots(void)
{
    host();
    for (JceEntity e = 1; e <= 600; ++e) store(e, (float)e);
    TEST_ASSERT_TRUE(sr->prev_xform_cap > SR_PREV_XFORM_MAX);
    host();
    for (JceEntity e = 1; e <= 600; ++e) {
        previous(e, (float)e); store(e, (float)e + 1);
    }
    const uint32_t before = sr->prev_xform_cap;
    for (JceEntity e = 601; e <= 1800; ++e) store(e, (float)e);
    TEST_ASSERT_TRUE(sr->prev_xform_cap > before);
    for (JceEntity e = 1; e <= 600; ++e) previous(e, (float)e);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, 1800));
}
static void test_failed_grow_full_table_never_aliases_and_can_prune(void)
{
    host(); store(1, 1); refuse_allocation = true;
    for (JceEntity e = 2; e <= SR_PREV_XFORM_MAX; ++e) store(e, (float)e);
    TEST_ASSERT_EQUAL_UINT32(SR_PREV_XFORM_MAX, sr->prev_xform_count);
    store(SR_PREV_XFORM_MAX + 1u, 999);
    TEST_ASSERT_NULL(sr_prev_xform_find(sr, SR_PREV_XFORM_MAX + 1u));
    host(); previous(SR_PREV_XFORM_MAX, (float)SR_PREV_XFORM_MAX);
    store(SR_PREV_XFORM_MAX, (float)SR_PREV_XFORM_MAX);
    host(); TEST_ASSERT_EQUAL_UINT32(1, sr->prev_xform_count);
    previous(SR_PREV_XFORM_MAX, (float)SR_PREV_XFORM_MAX);
}
static void draw_cached(JceEntity entity, float x)
{
    jce_scene_renderer_begin_velocity_frame(sr);
    cache_attempt(true); TEST_ASSERT_FALSE(cache_hit);
    store(entity, x); cache_publish(true);
}
static void test_move_stop_then_static_refreshes_zero_before_reuse(void)
{
    draw_cached(21, 0);
    /* The cold result already has zero object motion. Allow either its reuse
     * or the new conservative second pass; the stop transition is the oracle. */
    jce_scene_renderer_begin_velocity_frame(sr);
    cache_attempt(true);
    if (!cache_hit) { store(21, 0); cache_publish(true); }
    jce_scene_renderer_begin_velocity_frame(sr);
    cache_attempt(true); TEST_ASSERT_TRUE(cache_hit);
    ++counter; draw_cached(21, 5); previous(21, 0);
    draw_cached(21, 5); previous(21, 5); /* Stop must replace nonzero motion. */
    const unsigned before_allocations = allocations;
    const uint32_t before_processed = sr->prev_xform_gen;
    for (unsigned i = 0; i < 3; ++i) {
        jce_scene_renderer_begin_velocity_frame(sr);
        cache_attempt(true); TEST_ASSERT_TRUE(cache_hit);
        TEST_ASSERT_EQUAL_UINT32(before_processed, sr->prev_xform_gen);
    }
    TEST_ASSERT_EQUAL_UINT(before_allocations, allocations);
    ++counter; draw_cached(21, 8); previous(21, 5);
}
static void test_no_velocity_cache_keeps_single_pass_eligibility(void)
{
    cache_attempt(false); TEST_ASSERT_FALSE(cache_hit); cache_publish(false);
    cache_attempt(false); TEST_ASSERT_TRUE(cache_hit);
    TEST_ASSERT_EQUAL_UINT32(0, sr->vel_frame_gen);
    TEST_ASSERT_EQUAL_UINT(0, allocations);
}
static void test_colliding_view_publication_cannot_certify_object_stop(void)
{
    active_view = 0; draw_cached(21, 0); draw_cached(21, 0);
    ++counter;
    jce_scene_renderer_begin_velocity_frame(sr);
    active_view = 128; sr->ssao_target_generation = 2;
    cache_attempt(true); TEST_ASSERT_FALSE(cache_hit);
    previous(21, 0); store(21, 5); cache_publish(true);
    active_view = 0; sr->ssao_target_generation = 1;
    cache_attempt(true); TEST_ASSERT_FALSE(cache_hit);
    previous(21, 0); store(21, 5); cache_publish(true);
    /* B and A shared one metadata slot and counter 11, but A's pass wrote
     * nonzero velocity. Only A remains next frame: it must refresh to zero. */
    draw_cached(21, 5); previous(21, 5);
    jce_scene_renderer_begin_velocity_frame(sr);
    cache_attempt(true); TEST_ASSERT_TRUE(cache_hit);
}
int main(void)
{
    printf("velocity_world.entry_bytes=%zu initial_capacity=%u (CPU history; not GPU memory)\n",
        sizeof(SrPrevXform), (unsigned)SR_PREV_XFORM_MAX);
    UNITY_BEGIN();
    RUN_TEST(test_all_views_consume_one_previous_snapshot);
    RUN_TEST(test_view_union_prunes_without_breaking_wrapped_probe_chains);
    RUN_TEST(test_full_entity_generation_scene_change_and_reset);
    RUN_TEST(test_unhosted_pass_and_existing_host_generation_wrap);
    RUN_TEST(test_growth_preserves_both_snapshots);
    RUN_TEST(test_failed_grow_full_table_never_aliases_and_can_prune);
    RUN_TEST(test_move_stop_then_static_refreshes_zero_before_reuse);
    RUN_TEST(test_no_velocity_cache_keeps_single_pass_eligibility);
    RUN_TEST(test_colliding_view_publication_cannot_certify_object_stop);
    return UNITY_END();
}
