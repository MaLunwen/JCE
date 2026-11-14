/* Complete production cache functions and lifetime regions, with typed world,
 * hash, allocation and expanded-mesh ports. No Scene, renderer or GPU is made. */
#include "unity.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t JceEntity;
typedef struct { unsigned token; } JceMesh;
typedef struct { unsigned unused; } JceScene;
typedef struct { float x, y, z; } jce_vec3;
typedef struct { float x, y, z, w; } jce_vec4;
typedef struct { jce_vec4 col[4]; } jce_mat4;
typedef struct {
    int leaf_count;
    float radius, squash_y, leaf_scale;
    uint32_t seed;
} JceFoliageClusterComponent;

enum { JCE_GPU_TIER_LOW = 0 };
static void *allocations[256];
static unsigned allocation_calls, free_calls, mesh_creates, mesh_destroys;
static unsigned warnings, geometry_key;
static bool refuse_next_allocation;
static jce_vec3 world_origin;

static unsigned allocation_index(void *pointer)
{
    for (unsigned i = 0; i < 256u; ++i)
        if (allocations[i] == pointer) return i;
    TEST_FAIL_MESSAGE("Unknown or repeated allocation retirement");
    return 0u;
}

static void *counted_realloc(void *pointer, size_t size)
{
    ++allocation_calls;
    if (refuse_next_allocation) {
        refuse_next_allocation = false;
        return NULL;
    }
    const unsigned index = allocation_index(pointer);
    void *result = realloc(pointer, size);
    TEST_ASSERT_NOT_NULL(result);
    allocations[index] = result;
    return result;
}

static void counted_free(void *pointer)
{
    if (!pointer) return;
    allocations[allocation_index(pointer)] = NULL;
    ++free_calls;
    free(pointer);
}

static void jce_mesh_destroy(JceMesh *mesh)
{
    if (!mesh) return;
    ++mesh_destroys;
    counted_free(mesh);
}

static JceMesh *sr_fcluster_build_expanded_mesh(const float *instances, uint32_t count)
{
    TEST_ASSERT_NOT_NULL(instances);
    TEST_ASSERT_GREATER_THAN_UINT32(0, count);
    JceMesh *mesh = counted_realloc(NULL, sizeof *mesh);
    if (mesh) {
        mesh->token = ++mesh_creates;
    }
    return mesh;
}

static int jce_renderer_get_tier(void) { return JCE_GPU_TIER_LOW + 1; }
static jce_vec3 jce_v3(float x, float y, float z)
{
    jce_vec3 result = {x, y, z};
    return result;
}
static jce_mat4 jce_scene_get_world_matrix(JceScene *scene, JceEntity entity)
{
    (void)scene; (void)entity;
    jce_mat4 result = {{{0}}};
    result.col[3].x = world_origin.x;
    result.col[3].y = world_origin.y;
    result.col[3].z = world_origin.z;
    return result;
}
/* Identity is an explicitly controlled dependency, not a copied geometry hash. */
static uint32_t sr_foliage_hash(const JceFoliageClusterComponent *component,
                               const jce_vec3 *origin)
{
    (void)component; (void)origin;
    return geometry_key;
}
#define JCE_REALLOC(pointer, size) counted_realloc(pointer, size)
#define JCE_FREE(pointer) counted_free(pointer)
#define LOG_WARN(...) do { ++warnings; } while (0)
#include JCE_TEST_FCLUSTER_OWNER

static JceFoliageClusterComponent component(void)
{
    JceFoliageClusterComponent result = {48, 1.75f, 0.67f, 1.1f, 12345u};
    return result;
}
static unsigned live_allocations(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < 256u; ++i) count += allocations[i] != NULL;
    return count;
}
static void populate(JceSceneRenderer *sr, unsigned count)
{
    JceFoliageClusterComponent fc = component();
    for (unsigned i = 0; i < count; ++i)
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sr_fcluster_ensure_cache(sr, NULL, 7000u + i, &fc));
}
void setUp(void)
{
    memset(allocations, 0, sizeof allocations);
    allocation_calls = free_calls = mesh_creates = mesh_destroys = warnings = 0u;
    refuse_next_allocation = false;
    geometry_key = 1u;
    world_origin = jce_v3(4.0f, 6.0f, -2.0f);
}
void tearDown(void)
{
    /* Ordinary negative assertions may leave a live tail; retire the fixture's
     * tracked allocations without dereferencing the departed stack renderer. */
    for (unsigned i = 0; i < 256u; ++i) {
        free(allocations[i]);
        allocations[i] = NULL;
    }
}

static void test_static_52_cluster_working_set_reuses_geometry(void)
{
    JceSceneRenderer sr = {0};
    JceFoliageClusterComponent fc = component();
    populate(&sr, 52u);
    const unsigned initial_allocations = allocation_calls;
    const unsigned initial_frees = free_calls;
    for (unsigned pass = 0; pass < 6u; ++pass)
        for (unsigned i = 0; i < 52u; ++i) {
            const unsigned offset = pass & 1u ? 51u - i : i;
            TEST_ASSERT_GREATER_OR_EQUAL_INT(0,
                sr_fcluster_ensure_cache(&sr, NULL, 7000u + offset, &fc));
        }
    TEST_ASSERT_EQUAL_UINT(initial_allocations, allocation_calls);
    TEST_ASSERT_EQUAL_UINT(initial_frees, free_calls);
    TEST_ASSERT_EQUAL_UINT(52u, live_allocations());
    destroy_clusters(&sr);
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
}

static void test_over_budget_eviction_stays_bounded_and_deterministic(void)
{
    JceSceneRenderer sr = {0};
    JceFoliageClusterComponent fc = component();
    populate(&sr, SR_FCLUSTER_CACHE_CAPACITY);
    for (unsigned i = 0; i < 9u; ++i) {
        const JceEntity entity = 9000u + i;
        const unsigned victim = (uint32_t)entity % SR_FCLUSTER_CACHE_CAPACITY;
        const unsigned before_frees = free_calls;
        TEST_ASSERT_EQUAL_INT(victim, sr_fcluster_ensure_cache(&sr, NULL, entity, &fc));
        TEST_ASSERT_EQUAL_UINT64(entity, sr.fcluster_cache[victim].entity);
        TEST_ASSERT_EQUAL_UINT(before_frees + 1u, free_calls);
        TEST_ASSERT_EQUAL_UINT(SR_FCLUSTER_CACHE_CAPACITY, live_allocations());
    }
    destroy_clusters(&sr);
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
}

static void test_full_entity_identity_and_changed_hash_are_independent(void)
{
    JceSceneRenderer sr = {0};
    JceFoliageClusterComponent fc = component();
    const JceEntity first = 42u;
    const JceEntity next_generation = (UINT64_C(1) << 32u) | first;
    const int a = sr_fcluster_ensure_cache(&sr, NULL, first, &fc);
    const int b = sr_fcluster_ensure_cache(&sr, NULL, next_generation, &fc);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_UINT64(first, sr.fcluster_cache[a].entity);
    TEST_ASSERT_EQUAL_UINT64(next_generation, sr.fcluster_cache[b].entity);
    const float old_x = sr.fcluster_cache[a].inst[0];
    world_origin.x += 3.0f;
    ++geometry_key;
    TEST_ASSERT_EQUAL_INT(a, sr_fcluster_ensure_cache(&sr, NULL, first, &fc));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, old_x + 3.0f, sr.fcluster_cache[a].inst[0]);
    TEST_ASSERT_EQUAL_UINT(2u, allocation_calls);
    destroy_clusters(&sr);
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
}

static void test_allocation_refusal_retains_old_entry_and_retries(void)
{
    JceSceneRenderer sr = {0};
    JceFoliageClusterComponent fc = component();
    refuse_next_allocation = true;
    TEST_ASSERT_EQUAL_INT(-1, sr_fcluster_ensure_cache(&sr, NULL, 5u, &fc));
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
    const int slot = sr_fcluster_ensure_cache(&sr, NULL, 5u, &fc);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, slot);
    float *original = sr.fcluster_cache[slot].inst;
    const uint32_t old_hash = sr.fcluster_cache[slot].param_hash;
    fc.leaf_count = 300;
    ++geometry_key;
    refuse_next_allocation = true;
    TEST_ASSERT_EQUAL_INT(-1, sr_fcluster_ensure_cache(&sr, NULL, 5u, &fc));
    TEST_ASSERT_EQUAL_PTR(original, sr.fcluster_cache[slot].inst);
    TEST_ASSERT_EQUAL_UINT(48u, sr.fcluster_cache[slot].count);
    TEST_ASSERT_EQUAL_UINT(old_hash, sr.fcluster_cache[slot].param_hash);
    TEST_ASSERT_EQUAL_UINT(0u, free_calls);
    TEST_ASSERT_EQUAL_INT(slot, sr_fcluster_ensure_cache(&sr, NULL, 5u, &fc));
    TEST_ASSERT_EQUAL_UINT(256u, sr.fcluster_cache[slot].count);
    TEST_ASSERT_EQUAL_UINT(256u * 8u, sr.fcluster_cache[slot].inst_cap);
    destroy_clusters(&sr);
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
}

static void test_reset_retires_every_cpu_and_expanded_slot(void)
{
    JceSceneRenderer sr = {0};
    sr.fcluster_expanded_cards = true;
    populate(&sr, SR_FCLUSTER_CACHE_CAPACITY);
    TEST_ASSERT_EQUAL_UINT(2u * SR_FCLUSTER_CACHE_CAPACITY, live_allocations());
    TEST_ASSERT_EQUAL_INT(SR_FCLUSTER_CACHE_CAPACITY, reset_clusters(&sr));
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
    TEST_ASSERT_EQUAL_UINT(SR_FCLUSTER_CACHE_CAPACITY, mesh_destroys);
    TEST_ASSERT_EQUAL_INT(0, reset_clusters(&sr));
    populate(&sr, 1u);
    TEST_ASSERT_EQUAL_INT(1, reset_clusters(&sr));
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
}

static void test_destruction_retires_every_cpu_and_expanded_slot(void)
{
    JceSceneRenderer sr = {0};
    sr.fcluster_expanded_cards = true;
    populate(&sr, SR_FCLUSTER_CACHE_CAPACITY);
    destroy_clusters(&sr);
    TEST_ASSERT_EQUAL_UINT(0u, live_allocations());
    TEST_ASSERT_EQUAL_UINT(SR_FCLUSTER_CACHE_CAPACITY, mesh_destroys);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_static_52_cluster_working_set_reuses_geometry);
    RUN_TEST(test_over_budget_eviction_stays_bounded_and_deterministic);
    RUN_TEST(test_full_entity_identity_and_changed_hash_are_independent);
    RUN_TEST(test_allocation_refusal_retains_old_entry_and_retries);
    RUN_TEST(test_reset_retires_every_cpu_and_expanded_slot);
    RUN_TEST(test_destruction_retires_every_cpu_and_expanded_slot);
    return UNITY_END();
}
