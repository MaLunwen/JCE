/*
 * test_jce_spawn_manager.c — Unit tests for jce_spawn_manager.h (L4).
 *
 * Uses a deterministic ped sampler that always returns a point inside
 * the spawn ring and a callback that issues monotonically increasing
 * cookies, so we can drive the manager through a full spawn → cull
 * cycle without any real navmesh / road network.
 */

#include "unity.h"

#include <jce/middleware/world/jce_spawn_manager.h>
#include <jce/os/core/jce_math.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Test fixtures                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t created;
    uint32_t destroyed;
    uint64_t next_cookie;
    /* Sampler returns a point at fixed offset (15,0,0) from viewer
     * — well inside [10..30] ring used in most tests. */
    jce_vec3 sample_offset;
    /* Global actor-budget gate (large-world #2): when budget != 0, on_create
     * refuses past `live`, mirroring the runtime's rt_spawn_create. */
    uint32_t budget;
    uint32_t live;
} Fixture;

static uint64_t cb_create(const JceSpawnRequest *req, void *user)
{
    (void)req;
    Fixture *fx = (Fixture *)user;
    fx->created++;
    return ++fx->next_cookie;
}

static void cb_destroy(uint64_t cookie, void *user)
{
    (void)cookie;
    Fixture *fx = (Fixture *)user;
    fx->destroyed++;
}

static bool cb_ped_sample(jce_vec3 viewer, float min_r, float max_r,
                           jce_vec3 *out, void *user)
{
    (void)min_r; (void)max_r;
    Fixture *fx = (Fixture *)user;
    out->x = viewer.x + fx->sample_offset.x;
    out->y = viewer.y + fx->sample_offset.y;
    out->z = viewer.z + fx->sample_offset.z;
    return true;
}

static JceSpawnManagerDesc base_desc(Fixture *fx)
{
    JceSpawnManagerDesc d;
    memset(&d, 0, sizeof(d));
    d.max_peds         = 4;
    d.max_vehicles     = 0;       /* no road network needed */
    d.min_spawn_radius = 10.0f;
    d.max_spawn_radius = 30.0f;
    d.despawn_pad      = 5.0f;
    d.spawn_interval   = 0.1f;
    d.on_create        = cb_create;
    d.on_destroy       = cb_destroy;
    d.ped_sampler      = cb_ped_sample;
    d.user             = fx;
    d.rng_seed         = 1234ULL;
    fx->sample_offset  = jce_v3(15, 0, 0);
    return d;
}

/* ------------------------------------------------------------------ */
/* Lifecycle / config                                                   */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    JceSpawnManager *m = jce_spawn_manager_create(&d);
    TEST_ASSERT_NOT_NULL(m);
    jce_spawn_manager_destroy(m);
}

static void test_create_rejects_bad_radii(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    d.min_spawn_radius = 30.0f;
    d.max_spawn_radius = 10.0f;
    TEST_ASSERT_NULL(jce_spawn_manager_create(&d));
    TEST_ASSERT_NULL(jce_spawn_manager_create(NULL));
}

static void test_null_safety(void)
{
    jce_spawn_manager_destroy(NULL);
    jce_spawn_manager_set_viewer(NULL, jce_v3(0,0,0));
    jce_spawn_manager_update(NULL, 0.1f);
    jce_spawn_manager_clear(NULL);
    JceSpawnStats s = jce_spawn_manager_get_stats(NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, s.live_peds);
}

/* ------------------------------------------------------------------ */
/* Spawn cycle                                                          */
/* ------------------------------------------------------------------ */

static void test_update_without_viewer_does_nothing(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    JceSpawnManager *m = jce_spawn_manager_create(&d);
    /* No set_viewer → update is a no-op. */
    jce_spawn_manager_update(m, 1.0f);
    TEST_ASSERT_EQUAL_UINT32(0u, fx.created);
    jce_spawn_manager_destroy(m);
}

static void test_update_spawns_peds_up_to_cap(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    JceSpawnManager *m = jce_spawn_manager_create(&d);

    jce_spawn_manager_set_viewer(m, jce_v3(0, 0, 0));
    /* Many ticks: alternates ped/vehicle, only peds enabled.
     * Run enough intervals to fill 4 ped slots. */
    for (int i = 0; i < 50; i++)
        jce_spawn_manager_update(m, 0.1f);

    JceSpawnStats s = jce_spawn_manager_get_stats(m);
    TEST_ASSERT_EQUAL_UINT32(4u, s.live_peds);
    TEST_ASSERT_TRUE(s.spawn_succeeded >= 4u);
    jce_spawn_manager_destroy(m);
}

static void test_moving_viewer_culls_far_peds(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    JceSpawnManager *m = jce_spawn_manager_create(&d);

    jce_spawn_manager_set_viewer(m, jce_v3(0, 0, 0));
    for (int i = 0; i < 50; i++) jce_spawn_manager_update(m, 0.1f);
    JceSpawnStats s1 = jce_spawn_manager_get_stats(m);
    TEST_ASSERT_TRUE(s1.live_peds > 0u);

    /* Teleport viewer 10000 units away — all peds should be culled. */
    jce_spawn_manager_set_viewer(m, jce_v3(10000.0f, 0, 0));
    jce_spawn_manager_update(m, 0.1f);
    JceSpawnStats s2 = jce_spawn_manager_get_stats(m);
    TEST_ASSERT_EQUAL_UINT32(0u, s2.live_peds);
    TEST_ASSERT_TRUE(s2.despawned >= s1.live_peds);
    jce_spawn_manager_destroy(m);
}

/* Global actor budget: on_create returns 0 once the world pool is full, exactly
 * as the runtime's rt_spawn_create does.  Verifies the manager honours the abort
 * (live caps BELOW its own max_peds) and that despawn frees budget for re-spawn. */
static uint64_t cb_create_budgeted(const JceSpawnRequest *req, void *user)
{
    (void)req;
    Fixture *fx = (Fixture *)user;
    if (fx->budget && fx->live >= fx->budget) return 0;   /* over budget: abort */
    fx->created++; fx->live++;
    return ++fx->next_cookie;
}

static void cb_destroy_budgeted(uint64_t cookie, void *user)
{
    (void)cookie;
    Fixture *fx = (Fixture *)user;
    fx->destroyed++;
    if (fx->live) fx->live--;
}

static void test_actor_budget_caps_below_manager_max(void)
{
    Fixture fx = {0};
    fx.budget = 2;                        /* global pool smaller than max_peds */
    JceSpawnManagerDesc d = base_desc(&fx);
    d.max_peds   = 4;                     /* manager alone would allow 4 ...   */
    d.on_create  = cb_create_budgeted;    /* ... but the budget caps it at 2   */
    d.on_destroy = cb_destroy_budgeted;
    JceSpawnManager *m = jce_spawn_manager_create(&d);

    jce_spawn_manager_set_viewer(m, jce_v3(0, 0, 0));
    for (int i = 0; i < 50; i++) jce_spawn_manager_update(m, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_spawn_manager_get_stats(m).live_peds);
    TEST_ASSERT_EQUAL_UINT32(2u, fx.live);

    /* Cull all (viewer far) — budget frees. */
    jce_spawn_manager_set_viewer(m, jce_v3(10000.0f, 0, 0));
    jce_spawn_manager_update(m, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_spawn_manager_get_stats(m).live_peds);
    TEST_ASSERT_EQUAL_UINT32(0u, fx.live);

    /* Return — re-spawns up to the budget again (proves the pool recycles). */
    jce_spawn_manager_set_viewer(m, jce_v3(0, 0, 0));
    for (int i = 0; i < 50; i++) jce_spawn_manager_update(m, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_spawn_manager_get_stats(m).live_peds);
    jce_spawn_manager_destroy(m);
}

static void test_clear_destroys_all_live_slots(void)
{
    Fixture fx = {0};
    JceSpawnManagerDesc d = base_desc(&fx);
    JceSpawnManager *m = jce_spawn_manager_create(&d);

    jce_spawn_manager_set_viewer(m, jce_v3(0, 0, 0));
    for (int i = 0; i < 50; i++) jce_spawn_manager_update(m, 0.1f);
    uint32_t before = fx.created;
    TEST_ASSERT_TRUE(before > 0u);

    uint32_t destroyed_before = fx.destroyed;
    jce_spawn_manager_clear(m);
    /* Every live slot must have fired on_destroy. */
    TEST_ASSERT_TRUE(fx.destroyed > destroyed_before);
    jce_spawn_manager_destroy(m);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_create_rejects_bad_radii);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_update_without_viewer_does_nothing);
    RUN_TEST(test_update_spawns_peds_up_to_cap);
    RUN_TEST(test_moving_viewer_culls_far_peds);
    RUN_TEST(test_actor_budget_caps_below_manager_max);
    RUN_TEST(test_clear_destroys_all_live_slots);
    return UNITY_END();
}
