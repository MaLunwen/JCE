/* Actual ecull admission/hit/miss/repair/fold and shadow grid/cascade cache
 * owners. Real Scene/Flecs and Space; counted CPU resource/job boundary ports.
 * No device, native window, rasterized shadow or timing claim is made here. */
#include "unity.h"
#include "jce_sr_internal.h"
#include "jce_scene_internal.h"
#include "jce_shadow_key.h"
#include "jce_component_registry_internal.h"
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_hashmap.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>

#ifndef JCE_TEST_SHADOW_SPACE_DISABLED
#define JCE_TEST_SHADOW_SPACE_DISABLED 0
#endif
#ifndef JCE_TEST_SHADOW_INCREMENTAL_DISABLED
#define JCE_TEST_SHADOW_INCREMENTAL_DISABLED 0
#endif

enum { CAPACITY = 4100 };
static JceScene *scene;
static JceSceneRenderer renderer;
static EntityList list;
static JceEntity outside, no_bounds;
static uint64_t list_generation;
static unsigned bounds_calls, full_builds, parallel_chunks;
static unsigned space_creates, space_resets, space_inserts;
static unsigned fail_realloc, fail_create, fail_insert;
static bool per_cascade;
static int leaf_quad_cookie;
static unsigned opaque_queries;
static JceEntity opaque_entity, alias_entity;

static bool has_comp_port(const JceScene *s, JceEntity e, int id)
{
    ++opaque_queries;
    return jce_scene_has_comp(s, e, id);
}
static bool opaque_has(const JceScene *s, JceEntity e)
{ return s == scene && e != JCE_ENTITY_INVALID && e == opaque_entity; }
static void opaque_remove(JceScene *s, JceEntity e)
{ if (opaque_has(s, e)) opaque_entity = JCE_ENTITY_INVALID; }
static bool alias_has(const JceScene *s, JceEntity e)
{ return s == scene && e != JCE_ENTITY_INVALID && e == alias_entity; }
static void alias_remove(JceScene *s, JceEntity e)
{ if (alias_has(s, e)) alias_entity = JCE_ENTITY_INVALID; }

/* The fixture supplies a known unit-box geometry resource, not a second
 * renderer bound algorithm. Its optional path denotes a known wider box. */
static bool bounds_port(JceSceneRenderer *sr, JceScene *s, JceEntity e,
                         const jce_mat4 *world, jce_vec3 *mn, jce_vec3 *mx)
{
    TEST_ASSERT_TRUE(sr == &renderer);
    TEST_ASSERT_TRUE(s == scene);
    ++bounds_calls;
    if (e == no_bounds || !world) return false;
    /* The real bounds owner has no geometry to resolve for these rows. */
    if (!jce_scene_has_mesh_renderer(s, e) &&
        !jce_scene_has_skeletal_animator(s, e)) return false;
    const JceMeshRenderer *mr = jce_scene_get_mesh_renderer_const(s, e);
    const float half = mr && mr->mesh_path[0] ? 2.0f : 0.5f;
    *mn = jce_v3(world->raw[3][0] - half, world->raw[3][1] - half, world->raw[3][2] - half);
    *mx = jce_v3(world->raw[3][0] + half, world->raw[3][1] + half, world->raw[3][2] + half);
    return true;
}
static void focus_store_port(JceSceneRenderer *sr, JceEntity e, jce_vec3 mn, jce_vec3 mx)
{ (void)sr; (void)e; (void)mn; (void)mx; }
static bool focus_touch_port(JceSceneRenderer *sr, JceEntity e, jce_vec3 mn, jce_vec3 mx)
{ (void)sr; (void)e; (void)mn; (void)mx; return true; }
static const char *env_port(const char *name)
{
    if (strcmp(name, "JCE_DISABLE_SHADOWSPACECACHE") == 0)
        return JCE_TEST_SHADOW_SPACE_DISABLED ? "1" : "0";
    if (strcmp(name, "JCE_ECULL_INCR") == 0)
        return JCE_TEST_SHADOW_INCREMENTAL_DISABLED ? "0" : "1";
    return NULL;
}
static void *realloc_port(void *ptr, size_t size)
{
    if (fail_realloc && --fail_realloc == 0) return NULL;
    return JCE_REALLOC(ptr, size);
}
static JceSpaceIndex *create_port(const JceSpaceConfig *config)
{
    ++space_creates;
    if (fail_create && --fail_create == 0) return NULL;
    return jce_space_create(config);
}
static void reset_port(JceSpaceIndex *index, const JceAABB *bounds)
{ ++space_resets; jce_space_reset(index, bounds); }
static uint32_t insert_port(JceSpaceIndex *index, JceAABB bounds, uint32_t id)
{
    ++space_inserts;
    if (fail_insert && --fail_insert == 0) return 0;
    return jce_space_insert(index, bounds, id);
}
static int per_cascade_port(void) { return per_cascade ? 1 : 0; }
static uint64_t scatter_content_port(JceSceneRenderer *sr, JceScene *s, EntityList *entities)
{ (void)sr; (void)s; (void)entities; return 0; }
static bool grass_content_port(JceSceneRenderer *sr, JceScene *s,
    EntityList *entities, const JceCamera *camera, uint64_t *key)
{ (void)sr; (void)s; (void)entities; (void)camera; *key = 0; return true; }
/* Controlled CPU delivery uses the real chunk grain and join ordering. It
 * does not certify a native worker race, scheduler or GPU completion. */
static void parallel_port(JceThreadPool *pool, const char *name, uint32_t count,
                           uint32_t grain, JceTaskRangeFn function, void *user)
{
    (void)pool;
    TEST_ASSERT_EQUAL_STRING("scene.entity-cull", name);
    TEST_ASSERT_EQUAL_UINT32(4096, grain);
    for (uint32_t begin = 0; begin < count; begin += grain) {
        const uint32_t end = begin + grain < count ? begin + grain : count;
        ++parallel_chunks;
        function(begin, end, user);
    }
}

/* Load all real declarations before renaming this private fixture's owners.
 * The linked Engine keeps its own production symbols unchanged. */
#undef JCE_REALLOC
#define JCE_REALLOC realloc_port
#define getenv env_port
#define sr_wcache_find fixture_wcache_find
#define sr_wcache_store fixture_wcache_store
#define sr_wcache_maybe_prune fixture_wcache_maybe_prune
#define sr_ecull_build_miss fixture_ecull_build_miss
#define sr_ecull_reset_dynamic_list fixture_reset_dynamic_list
#define sr_ecull_metadata_begin fixture_metadata_begin
#define sr_ecull_metadata_commit fixture_metadata_commit
#define sr_classify_entity fixture_classify_entity
#define jce_scene_entity_is_dynamic fixture_entity_is_dynamic
#define sr_entity_casts_shadow fixture_entity_casts_shadow
#define sr_prepare_foliage_cluster_shadows fixture_prepare_cluster_shadows
#define sr_shadow_caster_aabb bounds_port
#define sr_focus_aabb_store focus_store_port
#define sr_focus_aabb_touch_update focus_touch_port
#define jce_thread_pool_parallel_for_named parallel_port
#define sr_shadow_percascade_on per_cascade_port
#define jce_space_create create_port
#define jce_space_reset reset_port
#define jce_space_insert insert_port
#define jce_scene_has_comp has_comp_port
#include JCE_TEST_SHADOW_WORLD_OWNER
#include JCE_TEST_SHADOW_PARTICIPATION_OWNER
#include JCE_TEST_SHADOW_ECULL_OWNER
#include JCE_TEST_SHADOW_HITS_OWNER
#undef sr_focus_aabb_store
#undef sr_focus_aabb_touch_update
#define sr_focus_aabb_find fixture_focus_aabb_find
#define sr_focus_aabb_store fixture_focus_aabb_store
#define sr_focus_aabb_touch_update fixture_focus_aabb_touch_update
#define sr_focus_aabb_maybe_prune fixture_focus_aabb_maybe_prune
#define sr_focus_aabb_remove fixture_focus_aabb_remove
#include JCE_TEST_SHADOW_FOCUS_OWNER
#include JCE_TEST_SHADOW_MODE_OWNER
#include JCE_TEST_SHADOW_GRID_OWNER

/* Actual pre-CSM dependency fold; unrelated producers retain neutral typed ports. */
#define sr_prepare_scatter_shadows scatter_content_port
#define sr_prepare_grass_shadows grass_content_port
static void content_step(void)
{
    JceSceneRenderer *sr = &renderer;
    const JceCamera *camera = NULL;
#include JCE_TEST_SHADOW_CONTENT_OWNER
    (void)grass_complete;
}
#undef sr_prepare_scatter_shadows
#undef sr_prepare_grass_shadows

static void frame(bool ecull_par)
{
    JceSceneRenderer *sr = &renderer;
    const uint64_t s_col_list_gen = list_generation;
    const uint64_t struct_epoch = jce_scene_get_structural_epoch(scene);
    const bool wcache_on = true;
    const uint32_t n_chunks = ((uint32_t)list.count + 4095u) / 4096u;
    JceThreadPool *ecull_pool = NULL;
    sr_ecull_metadata_begin(sr, scene, s_col_list_gen, struct_epoch);
#include JCE_TEST_SHADOW_ADMISSION_OWNER
    ++full_builds;
#include JCE_TEST_SHADOW_BUILD_OWNER
#include JCE_TEST_SHADOW_COMMIT_OWNER
}

/* Only the static dual-map global gate and exact VP predicate are consumed.
 * The dynamic-overlap provider must not be evaluated on this route. */
static bool cascade_dynamic_port(const JceSceneRenderer *sr, jce_vec3 right,
                                  jce_vec3 up, jce_vec3 center, float radius)
{
    (void)sr; (void)right; (void)up; (void)center; (void)radius;
    TEST_FAIL_MESSAGE("Static dual cache unexpectedly evaluated dynamic overlap");
    return true;
}
#define sr_cascade_has_dyn cascade_dynamic_port
static bool static_cascade_hit(void)
{
    JceSceneRenderer *sr = &renderer;
    const bool dual_on = true;
    JceCsmData csm = {0};
    csm.cascade_count = 1;
    csm.vp[0] = jce_m4_identity();
    const jce_vec3 scull_right = {1, 0, 0}, scull_up = {0, 1, 0};
#include JCE_TEST_SHADOW_CASCADE_OWNER
    return SR_CASCADE_CACHE_HIT(0);
#undef SR_CASCADE_CACHE_HIT
#undef SR_CASCADE_CACHE_STORE
}
#undef sr_cascade_has_dyn

static void move_entity(JceEntity e, float x)
{
    JceTransform transform = *jce_scene_get_transform(scene, e);
    transform.position.x = x;
    jce_scene_set_transform(scene, e, &transform);
}
static void mesh_entity(JceEntity e, bool cast, const char *path)
{
    JceMeshRenderer mesh = {0};
    mesh.shadow_cast_off = !cast;
    mesh.mesh_path = path;
    jce_scene_set_mesh_renderer(scene, e, &mesh);
}
static void remember_cascade(void)
{
    renderer.shadow_cache_valid[0] = true;
    renderer.shadow_cache_vp[0] = jce_m4_identity();
    renderer.shadow_cache_caster_key = renderer.shadow_caster_key;
    renderer.shadow_cache_static_caster_key = renderer.shadow_static_caster_key;
    renderer.shadow_cache_struct_epoch = jce_scene_get_structural_epoch(scene);
    renderer.shadow_cache_cascades = 1;
    renderer.shadow_cache_map_size = renderer.shadow_map_size;
}
static uint32_t query_at(float x, uint32_t *ids)
{
    const JceAABB region = {{x - 0.05f, -0.05f, -0.05f}, {x + 0.05f, 0.05f, 0.05f}};
    return jce_space_query_aabb(renderer.shadow_space, region, ids, CAPACITY);
}
static bool grid(void) { return sr_build_shadow_space(&renderer, &list, scene); }

void setUp(void)
{
    memset(&renderer, 0, sizeof(renderer));
    memset(&list, 0, sizeof(list));
    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    list.cap = CAPACITY;
    list.entities = (JceEntity *)JCE_CALLOC(CAPACITY, sizeof(JceEntity));
    renderer.ecull_cap = CAPACITY;
    renderer.ecull = (SrEntityCull *)JCE_CALLOC(CAPACITY, sizeof(SrEntityCull));
    renderer.ecull_world = (jce_mat4 *)JCE_CALLOC(CAPACITY, sizeof(jce_mat4));
    renderer.ecull_fast_byte = (uint8_t *)JCE_CALLOC(CAPACITY, 1);
    renderer.ecull_light_byte = (uint8_t *)JCE_CALLOC(CAPACITY, 1);
    renderer.ecull_miss = (uint32_t *)JCE_CALLOC(8192, sizeof(uint32_t));
    renderer.ecull_miss_counts = (uint32_t *)JCE_CALLOC(2, sizeof(uint32_t));
    renderer.ecull_key_parts = (uint64_t *)JCE_CALLOC(2, sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(list.entities);
    TEST_ASSERT_NOT_NULL(renderer.ecull);
    TEST_ASSERT_NOT_NULL(renderer.ecull_world);
    TEST_ASSERT_NOT_NULL(renderer.ecull_fast_byte);
    TEST_ASSERT_NOT_NULL(renderer.ecull_light_byte);
    TEST_ASSERT_NOT_NULL(renderer.ecull_miss);
    TEST_ASSERT_NOT_NULL(renderer.ecull_miss_counts);
    TEST_ASSERT_NOT_NULL(renderer.ecull_key_parts);
    renderer.kindcache_on = true;
    for (int i = 0; i < 4; ++i) {
        list.entities[list.count++] = jce_scene_create_entity(scene, "caster-control");
        move_entity(list.entities[i], (float)i * 20.0f);
        mesh_entity(list.entities[i], i != 2, "");
    }
    JceRigidBodyComponent body = {0};
    jce_scene_set_rigidbody(scene, list.entities[1], &body);
    outside = jce_scene_create_entity(scene, "outside-fixed-list");
    mesh_entity(outside, false, "");
    (void)jce_scene_take_dirty_entities(scene, NULL, 0, NULL);
    list_generation = 1;
    bounds_calls = full_builds = parallel_chunks = 0;
    space_creates = space_resets = space_inserts = 0;
    fail_realloc = fail_create = fail_insert = 0;
    no_bounds = 0;
    opaque_entity = alias_entity = JCE_ENTITY_INVALID;
    opaque_queries = 0;
    per_cascade = true;
}
void tearDown(void)
{
    if (renderer.shadow_space) jce_space_destroy(renderer.shadow_space);
    JCE_FREE(renderer.shadow_noaabb);
    JCE_FREE(renderer.shadow_query);
    JCE_FREE(renderer.shadow_dyn);
    JCE_FREE(renderer.eidx);
    JCE_FREE(renderer.wcache);
    JCE_FREE(renderer.focus_aabb);
    JCE_FREE(renderer.ecull);
    JCE_FREE(renderer.ecull_world);
    JCE_FREE(renderer.ecull_fast_byte);
    JCE_FREE(renderer.ecull_light_byte);
    JCE_FREE(renderer.ecull_miss);
    JCE_FREE(renderer.ecull_miss_counts);
    JCE_FREE(renderer.ecull_key_parts);
    JCE_FREE(list.entities);
    if (scene) jce_scene_destroy(scene);
    scene = NULL;
}

static void test_serial_and_two_chunk_mixed_keys_match_real_misses(void)
{
    /* More than the actual 4096 grain: both real partial slots are consumed. */
    while (list.count < 4098) {
        const int i = list.count++;
        list.entities[i] = jce_scene_create_entity(scene, "chunk-static");
        mesh_entity(list.entities[i], (i % 3) != 0, "");
    }
    frame(false);
    const uint64_t full = renderer.shadow_caster_key;
    const uint64_t statics = renderer.shadow_static_caster_key;
    TEST_ASSERT_NOT_EQUAL(full, statics);
    renderer.ecull_frz_valid = false;
    unsigned before = bounds_calls;
    frame(false);
    TEST_ASSERT_EQUAL_UINT64(full, renderer.shadow_caster_key);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_EQUAL_UINT(before + 1, bounds_calls); /* sole dynamic miss */
    renderer.ecull_frz_valid = false;
    before = bounds_calls;
    frame(true);
    TEST_ASSERT_EQUAL_UINT(2, parallel_chunks);
    TEST_ASSERT_EQUAL_UINT64(full, renderer.shadow_caster_key);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_EQUAL_UINT(before + 1, bounds_calls);
    TEST_ASSERT_EQUAL_UINT32(1, renderer.shadow_dyn_count);
}
static void test_noncaster_motion_keeps_grid_and_static_cascade(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    remember_cascade();
    const uint64_t full = renderer.shadow_caster_key, statics = renderer.shadow_static_caster_key;
    const unsigned builds = full_builds, bounds = bounds_calls, resets = space_resets;
    move_entity(list.entities[2], 48.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds, full_builds);
    TEST_ASSERT_EQUAL_UINT(bounds + 1, bounds_calls);
    TEST_ASSERT_EQUAL_UINT64(full, renderer.shadow_caster_key);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets, space_resets);
    TEST_ASSERT_TRUE(static_cascade_hit());
    move_entity(outside, 90.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds, full_builds);
    TEST_ASSERT_EQUAL_UINT64(full, renderer.shadow_caster_key);
}
static void test_dirty_drain_with_nonempty_suffix_rebuilds_missed_bounds(void)
{
    frame(false);
    const unsigned builds = full_builds;
    TEST_ASSERT_EQUAL_FLOAT(40.0f, renderer.ecull_world[2].raw[3][0]);

    move_entity(list.entities[2], 48.0f);
    TEST_ASSERT_TRUE(jce_scene_take_dirty_entities(scene, NULL, 0, NULL) > 0);
    move_entity(outside, 90.0f);
    TEST_ASSERT_TRUE(jce_scene_peek_dirty_entities(scene, NULL, 0, NULL) > 0);

    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds + 1, full_builds);
    TEST_ASSERT_EQUAL_FLOAT(48.0f, renderer.ecull_world[2].raw[3][0]);

    move_entity(list.entities[2], 56.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds + 1, full_builds);
    TEST_ASSERT_EQUAL_FLOAT(56.0f, renderer.ecull_world[2].raw[3][0]);
}
static void test_dynamic_motion_reuses_only_static_grid(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    remember_cascade();
    const uint64_t full = renderer.shadow_caster_key, statics = renderer.shadow_static_caster_key;
    const unsigned builds = full_builds, resets = space_resets;
    move_entity(list.entities[1], 24.0f);
    frame(true);
    TEST_ASSERT_EQUAL_UINT(builds, full_builds);
    TEST_ASSERT_NOT_EQUAL(full, renderer.shadow_caster_key);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_EQUAL_UINT32(1, renderer.shadow_dyn_count);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets, space_resets);
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(0, query_at(24.0f, ids));
    TEST_ASSERT_TRUE(static_cascade_hit());
}
static void test_static_motion_rebuilds_bounds_grid_and_old_cascade(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    remember_cascade();
    const uint64_t statics = renderer.shadow_static_caster_key;
    const unsigned builds = full_builds, resets = space_resets;
    move_entity(list.entities[0], 100.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds + 1, full_builds);
    TEST_ASSERT_NOT_EQUAL(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_FALSE(static_cascade_hit());
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets + 1, space_resets);
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(0, query_at(0.0f, ids));
    TEST_ASSERT_EQUAL_UINT32(1, query_at(100.0f, ids));
    TEST_ASSERT_EQUAL_UINT32(0, ids[0]);
}
static void test_component_cast_and_dynamic_classification_rebuild(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    const uint64_t structure = jce_scene_get_structural_epoch(scene);
    mesh_entity(list.entities[0], false, "");
    TEST_ASSERT_EQUAL_UINT64(structure, jce_scene_get_structural_epoch(scene));
    frame(false);
    TEST_ASSERT_FALSE(renderer.ecull[0].casts_shadow);
    TEST_ASSERT_TRUE(grid());
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(0, query_at(0.0f, ids));
    mesh_entity(list.entities[0], true, "");
    frame(true);
    TEST_ASSERT_TRUE(renderer.ecull[0].casts_shadow);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(1, query_at(0.0f, ids));
    JceRigidBodyComponent body = {0};
    jce_scene_set_rigidbody(scene, list.entities[3], &body);
    frame(true);
    TEST_ASSERT_TRUE(renderer.ecull[3].is_dyn_caster);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(0, query_at(60.0f, ids));
    jce_scene_remove_rigidbody(scene, list.entities[3]);
    frame(false);
    TEST_ASSERT_FALSE(renderer.ecull[3].is_dyn_caster);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(1, query_at(60.0f, ids));
}
static void test_geometry_metadata_without_transform_rekeys_grid_and_cascade(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    remember_cascade();
    const uint64_t xform = jce_scene_get_xform_counter(scene);
    const uint64_t statics = renderer.shadow_static_caster_key;
    mesh_entity(list.entities[0], true, "known-wide-box.glb");
    TEST_ASSERT_EQUAL_UINT64(xform, jce_scene_get_xform_counter(scene));
    frame(false);
    TEST_ASSERT_NOT_EQUAL(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_FALSE(static_cascade_hit());
    TEST_ASSERT_TRUE(grid());
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(1, query_at(1.5f, ids));
    TEST_ASSERT_EQUAL_UINT32(0, ids[0]);
}
static void test_same_count_reordered_list_rebuilds_positional_grid(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    const uint64_t statics = renderer.shadow_static_caster_key;
    const unsigned resets = space_resets;
    JceEntity temp = list.entities[0];
    list.entities[0] = list.entities[3];
    list.entities[3] = temp;
    ++list_generation;
    frame(false);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets + 1, space_resets);
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(1, query_at(0.0f, ids));
    TEST_ASSERT_EQUAL_UINT32(3, ids[0]);
}
static void test_static_only_and_full_modes_are_distinct_cache_identity(void)
{
    jce_scene_remove_rigidbody(scene, list.entities[1]);
    frame(false);
    TEST_ASSERT_EQUAL_UINT64(renderer.shadow_caster_key, renderer.shadow_static_caster_key);
    TEST_ASSERT_TRUE(grid());
    unsigned resets = space_resets;
    per_cascade = false;
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(++resets, space_resets);
    TEST_ASSERT_FALSE(renderer.shadow_space_static_only);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets, space_resets);
    per_cascade = true;
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(++resets, space_resets);
    TEST_ASSERT_TRUE(renderer.shadow_space_static_only);
    renderer.shadow_dyn_overflow = true; /* actual conservative mode selector */
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(++resets, space_resets);
    TEST_ASSERT_FALSE(renderer.shadow_space_static_only);
}
static void test_full_mode_dynamic_veto_is_retained(void)
{
    frame(false);
    per_cascade = false;
    TEST_ASSERT_TRUE(renderer.shadow_has_dynamic_caster);
    TEST_ASSERT_TRUE(grid());
    const unsigned resets = space_resets;
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets + 1, space_resets);
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(1, query_at(20.0f, ids));
    TEST_ASSERT_EQUAL_UINT32(1, ids[0]);
}
static void test_failed_grid_admission_never_publishes_valid_partial_index(void)
{
    frame(false);
    fail_realloc = 1;
    TEST_ASSERT_FALSE(grid());
    TEST_ASSERT_FALSE(renderer.shadow_space_valid);
    fail_create = 1;
    TEST_ASSERT_FALSE(grid());
    TEST_ASSERT_FALSE(renderer.shadow_space_valid);
    TEST_ASSERT_TRUE(grid());
    renderer.shadow_space_valid = false;
    fail_insert = 2; /* real first insertion, then an explicit provider failure */
    TEST_ASSERT_FALSE(grid());
    TEST_ASSERT_FALSE(renderer.shadow_space_valid);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_TRUE(renderer.shadow_space_valid);
    uint32_t ids[CAPACITY];
    TEST_ASSERT_EQUAL_UINT32(1, query_at(60.0f, ids));
}
static void test_ring_overflow_index_failure_enable_and_scene_use_full_route(void)
{
    frame(false);
    unsigned builds = full_builds;
    move_entity(list.entities[2], 44.0f);
    fail_realloc = 1; /* eidx admission */
    frame(false);
    TEST_ASSERT_EQUAL_UINT(++builds, full_builds);
    jce_scene_notify_physics_writeback(scene);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(++builds, full_builds);
    jce_scene_bump_enable_gen(scene);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(++builds, full_builds);
    renderer.ecull_frz_scene = NULL; /* actual owner identity mismatch */
    frame(false);
    TEST_ASSERT_EQUAL_UINT(++builds, full_builds);
}
static void test_no_aabb_membership_tracks_static_only_mode(void)
{
    no_bounds = list.entities[3];
    frame(false);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(1, renderer.shadow_noaabb_count);
    TEST_ASSERT_EQUAL_UINT32(3, renderer.shadow_noaabb[0]);
    JceRigidBodyComponent body = {0};
    jce_scene_set_rigidbody(scene, no_bounds, &body);
    frame(false);
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(0, renderer.shadow_noaabb_count);
    per_cascade = false;
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(1, renderer.shadow_noaabb_count);
}
static void test_disabled_grid_bypasses_reuse(void)
{
    frame(false);
    TEST_ASSERT_TRUE(grid());
    const unsigned resets = space_resets;
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT(resets + 1, space_resets);
}
static void test_legacy_non_geometry_rows_do_not_enter_shadow_population(void)
{
    JceEntity inert[16];
    for (int i = 0; i < 16; ++i) {
        inert[i] = jce_scene_create_entity(scene, "non-shadow-geometry");
        move_entity(inert[i], (float)(200 + i * 20));
        list.entities[list.count++] = inert[i];
    }
    JceDirectionalLight dir = {0}; jce_scene_set_dir_light(scene, inert[1], &dir);
    JcePointLight point = {0}; jce_scene_set_point_light(scene, inert[2], &point);
    JceSpotLight spot = {0}; jce_scene_set_spot_light(scene, inert[3], &spot);
    JceCameraComponent camera = {0}; jce_scene_set_camera(scene, inert[4], &camera);
    JceVirtualCameraComponent vcam = {0}; jce_scene_set_virtual_camera(scene, inert[5], &vcam);
    JceParticleEmitterComponent particles = {0}; jce_scene_set_particle_emitter(scene, inert[6], &particles);
    JceLineRendererComponent line = {0}; jce_scene_set_line_renderer(scene, inert[7], &line);
    JceTrailRendererComponent trail = {0}; jce_scene_set_trail_renderer(scene, inert[8], &trail);
    JceAudioSourceComponent audio = {0}; jce_scene_set_audio_source(scene, inert[9], &audio);
    JceCanvasComponent canvas = {0}; jce_scene_set_canvas(scene, inert[10], &canvas);
    JceUITextComponent text = {0}; jce_scene_set_ui_text(scene, inert[11], &text);
    JceUIImageComponent image = {0}; jce_scene_set_ui_image(scene, inert[12], &image);
    JceUIButtonComponent button = {0}; jce_scene_set_ui_button(scene, inert[13], &button);
    JceScriptComponent script = {0}; jce_scene_set_script(scene, inert[14], &script);
    JceRigidBodyComponent body = {0}; jce_scene_set_rigidbody(scene, inert[15], &body);
    frame(false);
    for (int i = 4; i < list.count; ++i)
        TEST_ASSERT_FALSE_MESSAGE(renderer.ecull[i].casts_shadow,
            "A legacy non-geometry entity cannot add a shadow draw");
    TEST_ASSERT_TRUE(grid());
    TEST_ASSERT_EQUAL_UINT32(0, renderer.shadow_noaabb_count);
    TEST_ASSERT_EQUAL_UINT32(1, renderer.shadow_dyn_count); /* original real mover only */
    const uint64_t key = renderer.shadow_caster_key;
    const unsigned queries = opaque_queries, builds = full_builds;
    frame(false);
    TEST_ASSERT_EQUAL_UINT(queries, opaque_queries);
    move_entity(inert[7], 333.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(queries, opaque_queries);
    TEST_ASSERT_EQUAL_UINT(builds, full_builds);
    TEST_ASSERT_EQUAL_UINT64(key, renderer.shadow_caster_key);
}
static void test_all_real_shadow_producers_retain_admission(void)
{
    JceEntity e = jce_scene_create_entity(scene, "producer-preservation");
    move_entity(e, 160.0f);
    JceSkeletalAnimatorComponent skin = {0}; jce_scene_set_skeletal_animator(scene, e, &skin);
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    jce_scene_remove_skeletal_animator(scene, e);
    JceTerrainComponent terrain = {0}; jce_scene_set_terrain(scene, e, &terrain);
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    jce_scene_remove_terrain(scene, e);
    JceFoliageClusterComponent foliage = {0}; jce_scene_set_foliage_cluster(scene, e, &foliage);
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    jce_scene_remove_foliage_cluster(scene, e);
    JceGrassFieldComponent grass = {0}; jce_scene_set_grass_field(scene, e, &grass);
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    jce_scene_remove_grass_field(scene, e);
    JceVegetationScatterComponent scatter = {0}; jce_scene_set_vegetation_scatter(scene, e, &scatter);
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    jce_scene_remove_vegetation_scatter(scene, e);
}
static void test_zero_and_aliased_legacy_custom_rows_remain_conservative(void)
{
    JceEntity e = jce_scene_create_entity(scene, "opaque-producer-preservation");
    move_entity(e, 160.0f);
    int id = jce_component_find("ShadowOpaqueControl");
    if (id == JCE_COMP_ID_INVALID) {
        JceComponentDesc desc = {0}; desc.name = "ShadowOpaqueControl";
        desc.has = opaque_has; desc.remove = opaque_remove;
        id = jce_component_register(&desc);
    }
    TEST_ASSERT_NOT_EQUAL(JCE_COMP_ID_INVALID, id);
    opaque_entity = e;
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    opaque_entity = JCE_ENTITY_INVALID;
    id = jce_component_find("ShadowAliasedControl");
    if (id == JCE_COMP_ID_INVALID) {
        JceComponentDesc desc = {0}; desc.name = "ShadowAliasedControl";
        desc.legacy_flag = JCE_COMP_FLAG_LINE_RENDERER;
        desc.has = alias_has; desc.remove = alias_remove;
        id = jce_component_register(&desc);
    }
    TEST_ASSERT_NOT_EQUAL(JCE_COMP_ID_INVALID, id);
    alias_entity = e;
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
    /* Existing MeshRenderer veto remains authoritative even on mixed entities. */
    mesh_entity(e, false, "");
    TEST_ASSERT_FALSE(fixture_entity_casts_shadow(scene, e));
    mesh_entity(e, true, "");
    TEST_ASSERT_TRUE(fixture_entity_casts_shadow(scene, e));
}
static void test_disabled_incremental_uses_complete_key_rebuild(void)
{
    frame(false);
    const unsigned builds = full_builds;
    const uint64_t full = renderer.shadow_caster_key, statics = renderer.shadow_static_caster_key;
    move_entity(list.entities[2], 43.0f);
    frame(false);
    TEST_ASSERT_EQUAL_UINT(builds + 1, full_builds);
    TEST_ASSERT_EQUAL_UINT64(full, renderer.shadow_caster_key);
    TEST_ASSERT_EQUAL_UINT64(statics, renderer.shadow_static_caster_key);
}
static JceEntity animated_cluster(void)
{
    JceEntity e = jce_scene_create_entity(scene, "animated-cluster");
    move_entity(e, 150.0f);
    JceFoliageClusterComponent fc = {0};
    fc.visible = true; fc.leaf_count = 8; fc.radius = 2;
    fc.squash_y = 1; fc.leaf_scale = 1;
    jce_scene_set_foliage_cluster(scene, e, &fc);
    list.entities[list.count++] = e;
    renderer.fcluster_quad = (JceMesh *)&leaf_quad_cookie;
    renderer.prog_fcluster_shadow.idx = 7; /* typed resource-presence port only */
    return e;
}
static void test_wind_cluster_time_invalidates_csm_on_every_ecull_route(void)
{
    JceEntity e = animated_cluster();
    frame(false); content_step(); remember_cascade();
    TEST_ASSERT_TRUE(static_cascade_hit());
    const uint64_t first = renderer.shadow_static_caster_key;
    renderer.water_time = 1.0f;
    frame(false); content_step();
    TEST_ASSERT_FALSE_MESSAGE(static_cascade_hit(), "Wind-deformed leaf depth cannot reuse an older time");
    TEST_ASSERT_NOT_EQUAL(first, renderer.shadow_static_caster_key);
    TEST_ASSERT_FALSE(renderer.ecull[list.count - 1].is_dyn_caster);
    TEST_ASSERT_FALSE(jce_scene_entity_is_dynamic(scene, e));
    remember_cascade();
    frame(false); content_step(); TEST_ASSERT_TRUE(static_cascade_hit()); /* paused time */
    renderer.ecull_frz_valid = false;
    frame(true); content_step(); TEST_ASSERT_TRUE(static_cascade_hit());
    renderer.water_time = 2.0f;
    frame(true); content_step(); TEST_ASSERT_FALSE(static_cascade_hit());
    remember_cascade();
    move_entity(outside, 93.0f); /* noncaster-only repair retains the bare keys */
    renderer.water_time = 3.0f;
    frame(false); content_step(); TEST_ASSERT_FALSE(static_cascade_hit());
}
static void test_nonanimated_cluster_states_do_not_age_with_time(void)
{
    JceEntity e = animated_cluster();
    for (int state = 0; state < 6; ++state) {
        JceFoliageClusterComponent fc = *jce_scene_get_foliage_cluster(scene, e);
        fc.visible = state != 0; fc.leaf_count = state == 1 ? 0 : 8;
        jce_scene_set_foliage_cluster(scene, e, &fc);
        renderer.fcluster_quad = state == 2 ? NULL : (JceMesh *)&leaf_quad_cookie;
        renderer.prog_fcluster_shadow.idx = state == 3 ? UINT16_MAX : 7;
        if (state == 4) mesh_entity(e, false, "");
        else jce_scene_remove_mesh_renderer(scene, e);
        JceEditorMeta meta = {0}; meta.enabled = state != 5;
        jce_scene_set_editor_meta(scene, e, &meta);
        frame(false); content_step(); remember_cascade();
        renderer.water_time += 1.0f;
        frame(false); content_step();
        TEST_ASSERT_TRUE_MESSAGE(static_cascade_hit(), "Static/hidden/vetoed cluster cannot age with wind time");
    }
}
static void test_cluster_geometry_and_alpha_changes_invalidate_without_transform(void)
{
    JceEntity e = animated_cluster();
    frame(false); content_step(); remember_cascade();
    JceFoliageClusterComponent fc = *jce_scene_get_foliage_cluster(scene, e);
    fc.radius += 0.25f;
    jce_scene_set_foliage_cluster(scene, e, &fc);
    frame(false); content_step(); TEST_ASSERT_FALSE(static_cascade_hit());
    remember_cascade();
    snprintf(fc.alpha_tex, sizeof fc.alpha_tex, "%s", "alternate-alpha.png");
    jce_scene_set_foliage_cluster(scene, e, &fc);
    frame(false); content_step(); TEST_ASSERT_FALSE(static_cascade_hit());
}
static void test_world_cache_distinguishes_reused_entity_slots(void)
{
    const JceEntity old_entity = UINT64_C(0x100000007);
    const JceEntity new_entity = UINT64_C(0x200000007);
    const jce_mat4 world = {0};
    SrEntityCull row = {0};
    row.has_aabb = true;
    row.wmin.x = 1.0f;
    fixture_wcache_store(&renderer, old_entity, &row, &world, 3, 5);
    TEST_ASSERT_NULL(fixture_wcache_find(&renderer, new_entity, 3, 5));

    row.wmin.x = 2.0f;
    fixture_wcache_store(&renderer, new_entity, &row, &world, 3, 5);
    SrWorldCacheEntry *old_entry = fixture_wcache_find(&renderer, old_entity, 3, 5);
    SrWorldCacheEntry *new_entry = fixture_wcache_find(&renderer, new_entity, 3, 5);
    TEST_ASSERT_NOT_NULL(old_entry);
    TEST_ASSERT_NOT_NULL(new_entry);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, old_entry->wmin.x);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, new_entry->wmin.x);
}

static void test_bounds_prune_preserves_wrapped_collision_survivors(void)
{
    const jce_mat4 world = {0};
    SrEntityCull row = {0};
    renderer.wcache_cap = 8;
    renderer.wcache = JCE_CALLOC(8, sizeof *renderer.wcache);
    renderer.focus_aabb_cap = 8;
    renderer.focus_aabb = JCE_CALLOC(8, sizeof *renderer.focus_aabb);
    TEST_ASSERT_NOT_NULL(renderer.wcache);
    TEST_ASSERT_NOT_NULL(renderer.focus_aabb);
    for (uint64_t i = 1; i <= 6; ++i) {
        const JceEntity entity = (i << 32) | 7;
        if (i <= 3) {
            row.wmin.x = (float)i;
            fixture_wcache_store(&renderer, entity, &row, &world, i == 1 ? 3 : 4, 5);
        }
        /* A loaded focus table at its prune threshold, wrapping from slot 7. */
        struct SrFocusAabbEntry *entry = &renderer.focus_aabb[(6 + i) & 7];
        entry->entity = entity;
        entry->wmin.x = (float)i;
        entry->used = true;
        entry->touched = (i % 2) == 0;
    }
    renderer.focus_aabb_count = 6;
    fixture_wcache_maybe_prune(&renderer, 4, true);
    fixture_focus_aabb_maybe_prune(&renderer);
    TEST_ASSERT_EQUAL_UINT(2, renderer.wcache_count);
    TEST_ASSERT_EQUAL_UINT(3, renderer.focus_aabb_count);
    for (uint64_t i = 1; i <= 6; ++i) {
        const JceEntity entity = (i << 32) | 7;
        SrWorldCacheEntry *wc = fixture_wcache_find(&renderer, entity, 4, 5);
        struct SrFocusAabbEntry *fc = fixture_focus_aabb_find(&renderer, entity);
        if (i == 2 || i == 3) {
            TEST_ASSERT_NOT_NULL(wc);
            TEST_ASSERT_FALSE(wc->touched);
            TEST_ASSERT_EQUAL_FLOAT((float)i, wc->wmin.x);
        } else {
            TEST_ASSERT_NULL(wc);
        }
        if (i % 2 == 0) {
            TEST_ASSERT_NOT_NULL(fc);
            TEST_ASSERT_FALSE(fc->touched);
            TEST_ASSERT_EQUAL_FLOAT((float)i, fc->wmin.x);
        } else {
            TEST_ASSERT_NULL(fc);
        }
    }
    sr_bounds_cache_reset(&renderer);
    TEST_ASSERT_EQUAL_UINT(0, renderer.wcache_count);
    TEST_ASSERT_EQUAL_UINT(0, renderer.focus_aabb_count);
    TEST_ASSERT_EQUAL_UINT(8, renderer.wcache_cap);
    TEST_ASSERT_EQUAL_UINT(8, renderer.focus_aabb_cap);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    UNITY_BEGIN();
    if (JCE_TEST_SHADOW_SPACE_DISABLED) {
        RUN_TEST(test_disabled_grid_bypasses_reuse);
    } else if (JCE_TEST_SHADOW_INCREMENTAL_DISABLED) {
        RUN_TEST(test_disabled_incremental_uses_complete_key_rebuild);
    } else {
        RUN_TEST(test_world_cache_distinguishes_reused_entity_slots);
        RUN_TEST(test_bounds_prune_preserves_wrapped_collision_survivors);
        RUN_TEST(test_serial_and_two_chunk_mixed_keys_match_real_misses);
        RUN_TEST(test_noncaster_motion_keeps_grid_and_static_cascade);
        RUN_TEST(test_dirty_drain_with_nonempty_suffix_rebuilds_missed_bounds);
        RUN_TEST(test_dynamic_motion_reuses_only_static_grid);
        RUN_TEST(test_static_motion_rebuilds_bounds_grid_and_old_cascade);
        RUN_TEST(test_component_cast_and_dynamic_classification_rebuild);
        RUN_TEST(test_geometry_metadata_without_transform_rekeys_grid_and_cascade);
        RUN_TEST(test_same_count_reordered_list_rebuilds_positional_grid);
        RUN_TEST(test_static_only_and_full_modes_are_distinct_cache_identity);
        RUN_TEST(test_full_mode_dynamic_veto_is_retained);
        RUN_TEST(test_failed_grid_admission_never_publishes_valid_partial_index);
        RUN_TEST(test_ring_overflow_index_failure_enable_and_scene_use_full_route);
        RUN_TEST(test_no_aabb_membership_tracks_static_only_mode);
        RUN_TEST(test_legacy_non_geometry_rows_do_not_enter_shadow_population);
        RUN_TEST(test_all_real_shadow_producers_retain_admission);
        RUN_TEST(test_zero_and_aliased_legacy_custom_rows_remain_conservative);
        RUN_TEST(test_wind_cluster_time_invalidates_csm_on_every_ecull_route);
        RUN_TEST(test_nonanimated_cluster_states_do_not_age_with_time);
        RUN_TEST(test_cluster_geometry_and_alpha_changes_invalidate_without_transform);
    }
    printf("Shadow owner CPU ports: sizeof(ecull)=%zu sizeof(renderer)=%zu; no GPU/timing claim\n",
        sizeof(SrEntityCull), sizeof(JceSceneRenderer));
    return UNITY_END();
}
