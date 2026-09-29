/*
 * test_jce_terrain_cache.c
 *
 * The cache exists to stop five owners of one terrain from disagreeing.  What
 * matters is therefore identity and invalidation, not terrain maths:
 *
 *   - every consumer must get the SAME pointer, or they are still five copies;
 *   - the revision must move on invalidate, or a consumer keeps drawing the
 *     pre-sculpt terrain forever and nothing tells it otherwise;
 *   - a failed load must not be retried every frame by every consumer;
 *   - the cache must never hand out a pointer it has freed.
 *
 * The loader is a counting stub, so "loaded once" is an assertion rather than
 * an assumption.
 */

#include "jce_terrain_cache.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_thread.h>

#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static int  g_loads;
static bool g_fail;

static JceTerrain *stub_load(void *ud, const char *path)
{
    (void)ud; (void)path;
    g_loads++;
    if (g_fail) return NULL;
    return jce_terrain_create(9, 9, 8.0f, 8.0f, 10.0f, 8);
}

static void reset(void) { g_loads = 0; g_fail = false; }

/* ── 1. One load, one pointer, however many consumers ask ──────────────  */

static void test_all_consumers_share_one_terrain(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();
    TEST_ASSERT_NOT_NULL(c);

    JceTerrain *renderer = jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    JceTerrain *physics  = jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    JceTerrain *pick     = jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);

    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_EQUAL_PTR(renderer, physics);
    TEST_ASSERT_EQUAL_PTR(renderer, pick);
    /* The whole point: ONE disk load, not three. */
    TEST_ASSERT_EQUAL_INT(1, g_loads);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_terrain_cache_resident(c));

    /* A different path is a different terrain. */
    JceTerrain *other = jce_terrain_cache_acquire(c, "t/b.json", stub_load, NULL);
    TEST_ASSERT_NOT_NULL(other);
    TEST_ASSERT_TRUE(other != renderer);
    TEST_ASSERT_EQUAL_INT(2, g_loads);

    jce_terrain_cache_destroy(c);
}

/* ── 2. Invalidate moves the revision -- the missing mechanism ─────────
 *
 * This is what did not exist before: an editor sculpt had no way to tell the
 * collider or the pick mesh that the ground had changed. */

static void test_invalidate_moves_the_revision(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();

    jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    const uint64_t before = jce_terrain_cache_revision(c, "t/a.json");
    TEST_ASSERT_TRUE(before > 0u);

    /* Re-acquiring without an edit must NOT move it, or consumers would
     * rebuild their chunk meshes every single frame. */
    jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    TEST_ASSERT_EQUAL_UINT64(before, jce_terrain_cache_revision(c, "t/a.json"));
    TEST_ASSERT_EQUAL_INT(1, g_loads);

    /* The editor saves. */
    jce_terrain_cache_invalidate(c, "t/a.json");
    TEST_ASSERT_EQUAL_UINT64(0u, jce_terrain_cache_revision(c, "t/a.json"));
    TEST_ASSERT_NULL(jce_terrain_cache_peek(c, "t/a.json"));

    jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    const uint64_t after = jce_terrain_cache_revision(c, "t/a.json");
    TEST_ASSERT_TRUE(after > before);      /* every consumer now rebuilds */
    TEST_ASSERT_EQUAL_INT(2, g_loads);     /* and it really was re-read */

    jce_terrain_cache_destroy(c);
}

/* ── 3. A reused slot never inherits the old asset's revision ──────────
 *
 * If it did, a consumer holding the previous number would conclude it was
 * already up to date and keep drawing a terrain that no longer exists. */

static void test_slot_reuse_does_not_reuse_revision(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();

    jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    const uint64_t rev_a = jce_terrain_cache_revision(c, "t/a.json");

    jce_terrain_cache_invalidate(c, "t/a.json");
    jce_terrain_cache_acquire(c, "t/b.json", stub_load, NULL);  /* reuses slot 0 */
    const uint64_t rev_b = jce_terrain_cache_revision(c, "t/b.json");

    TEST_ASSERT_TRUE(rev_b > rev_a);
    jce_terrain_cache_destroy(c);
}

/* ── 4. A failed load is remembered, not retried every frame ───────────  */

static void test_failure_is_remembered(void)
{
    reset();
    g_fail = true;
    JceTerrainCache *c = jce_terrain_cache_create();

    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "t/missing.json", stub_load, NULL));
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "t/missing.json", stub_load, NULL));
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "t/missing.json", stub_load, NULL));
    /* Three consumers, ONE attempt -- otherwise a missing terrain costs a
     * filesystem miss per consumer per frame, forever. */
    TEST_ASSERT_EQUAL_INT(1, g_loads);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_terrain_cache_revision(c, "t/missing.json"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_cache_resident(c));

    /* But a file that appears later must be picked up once invalidated. */
    g_fail = false;
    jce_terrain_cache_invalidate(c, "t/missing.json");
    TEST_ASSERT_NOT_NULL(jce_terrain_cache_acquire(c, "t/missing.json", stub_load, NULL));
    TEST_ASSERT_EQUAL_INT(2, g_loads);

    jce_terrain_cache_destroy(c);
}

/* ── 5. Full cache refuses rather than evicting ────────────────────────
 *
 * Every consumer holds a BORROWED pointer, so evicting to make room would free
 * a terrain the renderer is drawing from this frame. */

static void test_full_cache_refuses_instead_of_evicting(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();

    char path[64];
    JceTerrain *first = NULL;
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "t/%d.json", i);
        JceTerrain *t = jce_terrain_cache_acquire(c, path, stub_load, NULL);
        TEST_ASSERT_NOT_NULL(t);
        if (i == 0) first = t;
    }
    TEST_ASSERT_EQUAL_UINT32(16u, jce_terrain_cache_resident(c));

    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "t/17.json", stub_load, NULL));
    /* The 17th must not have been loaded at all... */
    TEST_ASSERT_EQUAL_INT(16, g_loads);
    /* ...and crucially, the first one is still alive and still the same. */
    TEST_ASSERT_EQUAL_PTR(first, jce_terrain_cache_peek(c, "t/0.json"));

    jce_terrain_cache_destroy(c);
}

/* ── 6. Invalidate-all clears everything ───────────────────────────────  */

static void test_invalidate_all(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();
    jce_terrain_cache_acquire(c, "t/a.json", stub_load, NULL);
    jce_terrain_cache_acquire(c, "t/b.json", stub_load, NULL);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_terrain_cache_resident(c));

    jce_terrain_cache_invalidate(c, NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_cache_resident(c));
    TEST_ASSERT_NULL(jce_terrain_cache_peek(c, "t/a.json"));
    TEST_ASSERT_NULL(jce_terrain_cache_peek(c, "t/b.json"));

    jce_terrain_cache_destroy(c);
}

/* ── 7. Authoring adopts one live object and advances revisions ───────── */

static void test_adopt_and_touch_live_terrain(void)
{
    reset();
    JceTerrainCache *c = jce_terrain_cache_create();
    JceTerrain *authored = jce_terrain_create(9, 9, 8.0f, 8.0f, 10.0f, 8);
    TEST_ASSERT_NOT_NULL(authored);

    TEST_ASSERT_TRUE(jce_terrain_cache_adopt(c, "t/live.json", authored));
    TEST_ASSERT_EQUAL_PTR(authored,
        jce_terrain_cache_acquire(c, "t/live.json", stub_load, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_loads);
    const uint64_t before = jce_terrain_cache_revision(c, "t/live.json");

    TEST_ASSERT_TRUE(jce_terrain_cache_touch(c, "t/live.json"));
    TEST_ASSERT_TRUE(jce_terrain_cache_revision(c, "t/live.json") > before);
    TEST_ASSERT_EQUAL_PTR(authored,
        jce_terrain_cache_peek(c, "t/live.json"));

    JceTerrain *replacement =
        jce_terrain_create(17, 17, 16.0f, 16.0f, 20.0f, 8);
    TEST_ASSERT_NOT_NULL(replacement);
    const uint64_t touched = jce_terrain_cache_revision(c, "t/live.json");
    TEST_ASSERT_TRUE(jce_terrain_cache_adopt(c, "t/live.json", replacement));
    TEST_ASSERT_EQUAL_PTR(replacement,
        jce_terrain_cache_peek(c, "t/live.json"));
    TEST_ASSERT_TRUE(jce_terrain_cache_revision(c, "t/live.json") > touched);

    jce_terrain_cache_destroy(c);
}

/* ── 8. NULL safety ────────────────────────────────────────────────────  */

static void test_null_safety(void)
{
    reset();
    jce_terrain_cache_destroy(NULL);
    jce_terrain_cache_invalidate(NULL, "x");
    TEST_ASSERT_FALSE(jce_terrain_cache_adopt(NULL, "x", NULL));
    TEST_ASSERT_FALSE(jce_terrain_cache_touch(NULL, "x"));
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(NULL, "x", stub_load, NULL));
    TEST_ASSERT_NULL(jce_terrain_cache_peek(NULL, "x"));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_terrain_cache_revision(NULL, "x"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_cache_resident(NULL));

    JceTerrainCache *c = jce_terrain_cache_create();
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, NULL, stub_load, NULL));
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "",   stub_load, NULL));
    TEST_ASSERT_NULL(jce_terrain_cache_acquire(c, "x",  NULL,      NULL));
    TEST_ASSERT_FALSE(jce_terrain_cache_adopt(c, NULL, NULL));
    TEST_ASSERT_FALSE(jce_terrain_cache_touch(c, "missing"));
    TEST_ASSERT_EQUAL_INT(0, g_loads);
    jce_terrain_cache_destroy(c);
}

/* ── 9. Loose authoring files enter through the same scene owner ───────── */

static void test_scene_acquires_loose_file_once(void)
{
    const char *tmp = getenv("TEMP");
    char meta_path[512];
    char bin_path[512];

    if (!tmp || !tmp[0])
        tmp = ".";
    snprintf(meta_path, sizeof meta_path,
             "%s/jce_terrain_cache_%llu.terrain.json", tmp,
             (unsigned long long)jce_thread_current_id());
    snprintf(bin_path, sizeof bin_path,
             "%s/jce_terrain_cache_%llu.terrain.bin", tmp,
             (unsigned long long)jce_thread_current_id());

    JceTerrain *source =
        jce_terrain_create(9, 9, 8.0f, 8.0f, 10.0f, 8);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_TRUE(jce_terrain_save_file(source, meta_path));
    jce_terrain_free(source);

    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceTerrain *first = jce_scene_acquire_terrain_file(
        scene, "terrains/cache_probe.terrain.json", meta_path);
    JceTerrain *second = jce_scene_acquire_terrain_file(
        scene, "terrains/cache_probe.terrain.json", "definitely/missing.json");

    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_PTR(first, jce_scene_peek_terrain(
        scene, "terrains/cache_probe.terrain.json"));
    TEST_ASSERT_EQUAL_INT(9, jce_terrain_width(first));
    TEST_ASSERT_EQUAL_INT(9, jce_terrain_height(first));

    jce_scene_destroy(scene);
    remove(meta_path);
    remove(bin_path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_consumers_share_one_terrain);
    RUN_TEST(test_invalidate_moves_the_revision);
    RUN_TEST(test_slot_reuse_does_not_reuse_revision);
    RUN_TEST(test_failure_is_remembered);
    RUN_TEST(test_full_cache_refuses_instead_of_evicting);
    RUN_TEST(test_invalidate_all);
    RUN_TEST(test_adopt_and_touch_live_terrain);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_scene_acquires_loose_file_once);
    return UNITY_END();
}
