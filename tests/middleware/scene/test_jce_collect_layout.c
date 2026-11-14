/* CPU controls for exact collected-layout identity and existing ecull gates.
 * The runner extracts the actual production collector, metadata functions,
 * dirty repair and freeze admission. Scene/resource ports below are controlled
 * inputs; this fixture does not execute a native renderer or measure FPS. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jce_sr_metadata_key.h"

typedef uint64_t JceEntity;
typedef struct { float x, y, z; } jce_vec3;
typedef struct { jce_vec3 position; } JceTransform;
typedef struct { JceEntity *entities; int count, cap; } EntityList;
typedef struct { float min[3], max[3]; } JceModel;
typedef struct { int state; } JceAsyncTask;
struct SrModelJob { void *cpu; };
typedef struct {
    JceModel *model;
    bool pending;
    JceAsyncTask *task;
    struct SrModelJob *job;
    void *batch_members;
} SrModelCache;
typedef struct { uint64_t epoch; void *queue_head; bool fallback; } SrModelDeps;
typedef struct {
    uint64_t revision;
    bool ready, has_aabb;
    float min[3], max[3];
} SrModelDepEvent;
typedef struct {
    JceEntity entity;
    JceTransform transform;
    bool has_transform, caster, dynamic;
    bool bounds_valid;
    jce_vec3 bounds_min, bounds_max;
} TestEntity;
typedef struct {
    TestEntity entities[64];
    int count;
    uint64_t instance, roster, structure, xform, enable, data, dirty_base;
    JceEntity dirty[64];
    uint32_t dirty_count;
    bool dirty_overflow;
} JceScene;
struct SrFocusAabbEntry { jce_vec3 wmin, wmax; };
typedef struct { bool casts_shadow, is_dyn_caster; } SrEntityCull;
typedef struct {
    uint64_t model_bounds_scene, model_bounds_focus_revision, model_bounds_revision;
    uint64_t model_bounds_roster;
    bool model_bounds_enabled, model_bounds_focus_collect_valid, model_bounds_geometry_dirty;
    SrModelDeps model_bounds_deps;
    bool cull_frame_valid, shadow_space_valid, resource_prepared_valid;
    unsigned shadow_cache_cascades, ecull_cap;
    bool shadow_cache_valid[4];
    struct { bool valid; } pre_cache[4], depth_history, occlusion_history;
    bool depth_history_reused;
    SrModelCache **model_cache;
    uint32_t model_cache_capacity, model_cache_used;
    SrModelCache *model_lookup_hint;
    unsigned model_inflight;
    uint64_t model_cache_generation;
    bool kindcache_on, ecull_metadata_valid, ecull_metadata_trusted;
    SrMetadataKey ecull_metadata_key, ecull_metadata_next;
    bool ecull_frz_valid, ecull_frz_dyn;
    uint64_t ecull_frz_list_gen, ecull_frz_xform, ecull_frz_enable;
    uint64_t ecull_frz_struct, ecull_frz_caster_key, ecull_frz_static_caster_key;
    const void *ecull_frz_scene;
    uint64_t shadow_caster_key, shadow_static_caster_key, frame_list_gen;
    bool shadow_has_dynamic_caster;
    int32_t cull_movers_count;
    uint32_t cull_movers[1024];
    uint64_t eidx_list_gen;
    JceEntity indexed[64];
    int indexed_count;
    SrEntityCull ecull[64];
    struct SrFocusAabbEntry focus[64];
    bool has_focus[64];
} JceSceneRenderer;
typedef struct {
    float cull_focus_x, cull_focus_z, cull_radius;
    bool cull_focus_enabled;
} JceSceneRenderConfig;
typedef struct {
    EntityList *list;
    uint64_t generation;
    bool valid;
} CollectResult;
typedef struct { bool frozen, full; unsigned rebuilt; } CullResult;

static unsigned g_walks, g_allocations, g_rebuilds, g_drains;
static unsigned g_bounds_resets, g_model_publications;
static unsigned g_animation_resets, g_task_cancels, g_job_releases, g_model_releases;
static bool g_fail_allocation, g_collect_off, g_metadata_off;
static bool g_publish_during_miss;
static JceEntity g_rebuilt[64];
static JceModel g_model = {{-1, -1, -1}, {1, 1, 1}};

#define SR_MAX_ENTITIES 4
#define JCE_CSM_MAX_CASCADES 4
#define SR_MODEL_DEP_FALLBACK 1
#define LOG_TAG "collect-test"
static void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define LOG_WARN(...) test_log(__VA_ARGS__)
#define LOG_INFO(...) test_log(__VA_ARGS__)
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "ASSERTION FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return false; } } while (0)

static void *test_realloc(void *pointer, size_t size)
{
    ++g_allocations;
    return g_fail_allocation ? NULL : realloc(pointer, size);
}
#define JCE_REALLOC(pointer, size) test_realloc((pointer), (size))
#define JCE_FREE(pointer) free(pointer)
static const char *test_getenv(const char *name)
{
    if (g_collect_off && strcmp(name, "JCE_COLLECT_CACHE") == 0) return "0";
    if (g_metadata_off && strcmp(name, "JCE_DISABLE_ECULL_METADATA") == 0) return "1";
    return NULL;
}
#define getenv test_getenv
static uint64_t jce_time_perf_counter(void) { return 0; }
static double jce_time_perf_to_ms(uint64_t a, uint64_t b) { (void)a; (void)b; return 0; }
static void jce_perf_phase_add(const char *name, double ms) { (void)name; (void)ms; }
static uint64_t jce_scene_get_roster_epoch(JceScene *s) { return s->roster; }
static uint64_t jce_scene_get_structural_epoch(JceScene *s) { return s->structure; }
static uint64_t jce_scene_get_xform_counter(JceScene *s) { return s->xform; }
static uint64_t jce_scene_get_enable_gen(JceScene *s) { return s->enable; }
static uint64_t jce_scene_get_cull_data_gen(JceScene *s) { return s->data; }
static uint64_t jce_scene_internal_instance_id(JceScene *s) { return s->instance; }
static TestEntity *test_entity(JceScene *s, JceEntity entity)
{
    int i;
    for (i = 0; i < s->count; ++i)
        if (s->entities[i].entity == entity) return &s->entities[i];
    return NULL;
}
static bool jce_scene_has_transform(JceScene *s, JceEntity entity)
{
    TestEntity *e = test_entity(s, entity);
    return e && e->has_transform;
}
static JceTransform *jce_scene_get_transform(JceScene *s, JceEntity entity)
{
    TestEntity *e = test_entity(s, entity);
    return e && e->has_transform ? &e->transform : NULL;
}
static struct SrFocusAabbEntry *sr_focus_aabb_find(JceSceneRenderer *sr, JceEntity entity)
{
    return entity < 64 && sr->has_focus[entity] ? &sr->focus[entity] : NULL;
}
static void sr_model_bounds_drain(JceSceneRenderer *sr, JceScene *s,
                                  const EntityList *list, uint64_t generation);
static void sr_model_bounds_note(JceSceneRenderer *sr, const char *path, JceModel *model);
static void jce_scene_each_entity(JceScene *s,
                                  void (*callback)(JceScene *, JceEntity, void *),
                                  void *user)
{
    int i;
    ++g_walks;
    for (i = 0; i < s->count; ++i) callback(s, s->entities[i].entity, user);
}
static bool jce_scene_internal_dirty_base_matches(JceScene *s, uint64_t base)
{
    return s->dirty_base == base;
}
static uint32_t jce_scene_peek_dirty_entities(JceScene *s, JceEntity *out,
                                             uint32_t capacity, bool *overflow)
{
    uint32_t n = s->dirty_count < capacity ? s->dirty_count : capacity;
    if (out && n) memcpy(out, s->dirty, n * sizeof(*out));
    if (overflow) *overflow = s->dirty_overflow || s->dirty_count > capacity;
    return n;
}
static uint32_t jce_scene_take_dirty_entities(JceScene *s, JceEntity *out,
                                             uint32_t capacity, bool *overflow)
{
    uint32_t n = jce_scene_peek_dirty_entities(s, out, capacity, overflow);
    ++g_drains;
    s->dirty_count = 0;
    s->dirty_overflow = false;
    s->dirty_base = s->xform;
    return n;
}
static bool sr_eidx_build(JceSceneRenderer *sr, const EntityList *list)
{
    if (list->count > 64) return false;
    sr->indexed_count = list->count;
    memcpy(sr->indexed, list->entities, (size_t)list->count * sizeof(*sr->indexed));
    return true;
}
static int sr_eidx_find(const JceSceneRenderer *sr, uint32_t entity)
{
    int i;
    for (i = 0; i < sr->indexed_count; ++i)
        if (sr->indexed[i] == entity) return i;
    return -1;
}
static void sr_ecull_build_miss(JceSceneRenderer *sr, JceScene *s,
                               const EntityList *list, int slot,
                               uint64_t structure, bool wcache)
{
    TestEntity *e = test_entity(s, list->entities[slot]);
    (void)structure; (void)wcache;
    g_rebuilt[g_rebuilds++ % 64] = list->entities[slot];
    sr->ecull[slot].casts_shadow = e && e->caster;
    sr->ecull[slot].is_dyn_caster = e && e->caster && e->dynamic;
    if (sr->ecull[slot].is_dyn_caster) sr->shadow_has_dynamic_caster = true;
    if (e && e->bounds_valid && e->entity < 64) {
        sr->has_focus[e->entity] = true;
        sr->focus[e->entity].wmin = e->bounds_min;
        sr->focus[e->entity].wmax = e->bounds_max;
    }
    if (g_publish_during_miss) {
        g_publish_during_miss = false;
        sr_model_bounds_note(sr, "late-model.glb", &g_model);
    }
}

static bool jce_scene_entity_alive(void *scene, JceEntity entity)
{
    return test_entity((JceScene *)scene, entity) != NULL;
}
static void sr_bounds_cache_reset(JceSceneRenderer *sr)
{
    ++g_bounds_resets;
    memset(sr->has_focus, 0, sizeof(sr->has_focus));
}
static bool jce_model_get_aabb(JceModel *model, float *min, float *max)
{
    memcpy(min, model->min, sizeof(model->min));
    memcpy(max, model->max, sizeof(model->max));
    return true;
}
static void sr_model_deps_reset(SrModelDeps *deps, uint64_t epoch)
{
    memset(deps, 0, sizeof(*deps));
    deps->epoch = epoch;
}
static void sr_model_deps_prune(SrModelDeps *deps, uint64_t epoch,
                                bool (*alive)(void *, uint64_t), void *scene)
{
    (void)deps; (void)epoch; (void)alive; (void)scene;
}
static void sr_model_deps_publish(SrModelDeps *deps, uint64_t epoch,
                                  const char *path, const SrModelDepEvent *event)
{
    (void)deps; (void)epoch; (void)path; (void)event;
    ++g_model_publications;
}
static bool sr_model_bounds_repair_one(void *user, uint64_t entity, const char *path,
                                       const SrModelDepEvent *event)
{
    (void)user; (void)entity; (void)path; (void)event;
    return true;
}
static int sr_model_deps_drain(SrModelDeps *deps, uint64_t epoch,
        bool (*repair)(void *, uint64_t, const char *, const SrModelDepEvent *), void *user)
{
    (void)deps; (void)epoch; (void)repair; (void)user;
    return 0;
}
static void sr_anim_instances_destroy(JceSceneRenderer *sr) { (void)sr; ++g_animation_resets; }
static void sr_async_task_cancel_and_release(JceAsyncTask **task)
{
    ++g_task_cancels;
    free(*task);
    *task = NULL;
}
static void sr_model_job_destroy(struct SrModelJob *job)
{
    ++g_job_releases;
    if (job) free(job->cpu);
    free(job);
}
static void jce_model_destroy(JceModel *model) { ++g_model_releases; free(model); }
static void sr_model_cache_diagnostics_reset(JceSceneRenderer *sr)
{
    ++sr->model_cache_generation;
}

/* These files are byte-extracted by the runner from the selected real owner. */
#include "collect_callbacks.inc"
#include "collect_metadata.inc"
#include "collect_repair.inc"
#include "collect_model_bounds.inc"
#include "collect_model_drain.inc"
#include "collect_model_invalidate.inc"

static CollectResult collect_frame(JceSceneRenderer *sr, JceScene *scene,
                                   const JceSceneRenderConfig *cfg)
{
    uint64_t _t0_head = 0;
    CollectResult result;
#include "collect_frame.inc"
    result.list = &list;
    result.generation = s_col_list_gen;
    result.valid = s_col_valid;
    return result;
}

static CullResult cull_frame(JceSceneRenderer *sr, JceScene *scene, CollectResult collected)
{
    EntityList list = *collected.list;
    uint64_t s_col_list_gen = collected.generation;
    uint64_t struct_epoch = scene->structure;
    const bool wcache_on = true;
    CullResult result = {false, false, 0};
    unsigned old_rebuilds = g_rebuilds;
#include "collect_ecull_boundary.inc"
#include "collect_freeze.inc"
    /* Controlled full-build payload: bounds/resources are ports, while the
     * actual freeze/dirty/metadata owners above decide whether it is reached. */
    result.full = true;
    for (int i = 0; i < list.count; ++i)
        sr_ecull_build_miss(sr, scene, &list, i, struct_epoch, wcache_on);
    sr->ecull_frz_caster_key = sr->shadow_caster_key;
    sr->ecull_frz_static_caster_key = sr->shadow_static_caster_key;
    sr->ecull_frz_dyn = sr->shadow_has_dynamic_caster;
    sr->ecull_frz_valid = true;
ecull_build_done:
    sr_ecull_metadata_commit(sr);
    result.frozen = ecull_frozen;
    result.rebuilt = g_rebuilds - old_rebuilds;
    return result;
}

static void setup(JceScene *s, JceSceneRenderer *sr, JceSceneRenderConfig *cfg)
{
    int i;
    memset(s, 0, sizeof(*s));
    memset(sr, 0, sizeof(*sr));
    memset(cfg, 0, sizeof(*cfg));
    s->count = 3;
    s->instance = 100;
    s->roster = s->structure = s->xform = s->enable = s->data = s->dirty_base = 1;
    for (i = 0; i < s->count; ++i) {
        s->entities[i].entity = (JceEntity)(i + 1);
        s->entities[i].has_transform = true;
        s->entities[i].transform.position.x = (float)i;
    }
    sr->model_bounds_scene = 100;
    sr->kindcache_on = true;
    sr->ecull_cap = 64;
    cfg->cull_focus_enabled = true;
    cfg->cull_radius = 900;
}
static void move_entity(JceScene *s, JceEntity entity)
{
    s->dirty_base = s->xform;
    ++s->xform;
    s->dirty[0] = entity;
    s->dirty_count = 1;
}

static bool test_ordinary_path(void)
{
    JceScene s, replacement_scene;
    JceSceneRenderer sr, replacement_renderer;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    CullResult c;
    unsigned walks, rebuilds;
    setup(&s, &sr, &cfg);
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.valid && a.generation != 0 && a.list->count == 3);
    CHECK(sr.model_bounds_focus_collect_valid && !sr.model_bounds_enabled);
    c = cull_frame(&sr, &s, a);
    CHECK(c.full && c.rebuilt == 3 && !sr.ecull_metadata_trusted);

    /* First cold collect is deliberately unbounded; the next applies focus. */
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(g_walks == walks + 1 && b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);
    a = b;

    /* Default model-bounds-off and still camera retain the existing fast path. */
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(g_walks == walks && b.generation == a.generation);
    c = cull_frame(&sr, &s, b);
    CHECK(c.frozen && c.rebuilt == 0 && sr.ecull_metadata_trusted);

    /* Exact focus moves really recollect, yet identical IDs avoid cold ecull. */
    for (int i = 0; i < 20; ++i) {
        cfg.cull_focus_x += 0.125f;
        cfg.cull_focus_z -= 0.0625f;
        walks = g_walks;
        b = collect_frame(&sr, &s, &cfg);
        CHECK(g_walks == walks + 1 && b.generation == a.generation);
        c = cull_frame(&sr, &s, b);
        CHECK(c.frozen && c.rebuilt == 0 && sr.ecull_metadata_trusted);
    }

    /* Model the prior count clamp published by a downstream ecull refusal.
     * This controls recovery on a subsequent real recollect, not that owner. */
    --b.list->count;
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && b.list->count == 3);
    CHECK(cull_frame(&sr, &s, b).full);
    a = b;

    /* A moving noncaster must still enter the production dirty repair. */
    move_entity(&s, 2);
    s.entities[1].transform.position.x += 1;
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation);
    rebuilds = g_rebuilds;
    c = cull_frame(&sr, &s, b);
    CHECK(!c.frozen && !c.full && c.rebuilt == 1);
    CHECK(g_rebuilt[rebuilds % 64] == 2 && s.dirty_count == 0);

    /* Metadata edits cannot become trusted just because the ordered IDs match. */
    ++s.enable;
    c = cull_frame(&sr, &s, collect_frame(&sr, &s, &cfg));
    CHECK(c.full && !sr.ecull_metadata_trusted);
    ++s.data;
    c = cull_frame(&sr, &s, collect_frame(&sr, &s, &cfg));
    CHECK(c.full && !sr.ecull_metadata_trusted);

    /* Async bounds can change with the same roster/IDs/xform/data generation. */
    sr.model_bounds_enabled = true;
    a = collect_frame(&sr, &s, &cfg);
    cull_frame(&sr, &s, a);
    ++sr.model_bounds_focus_revision;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && b.list->count == 3);
    c = cull_frame(&sr, &s, b);
    CHECK(c.full && !sr.ecull_metadata_trusted && c.rebuilt == 3);
    a = b;
    sr.model_bounds_focus_collect_valid = false;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && sr.model_bounds_focus_collect_valid);
    c = cull_frame(&sr, &s, b);
    CHECK(c.full);

    /* Same-count order changes are genuine positional layout changes. */
    a = b;
    TestEntity swap = s.entities[0];
    s.entities[0] = s.entities[1]; s.entities[1] = swap;
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && b.list->entities[0] == 2);
    CHECK(cull_frame(&sr, &s, b).full);

    /* Full entity identity, including high bits, is compared before overwrite. */
    a = b;
    s.entities[1].entity += UINT64_C(1) << 32;
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && b.list->entities[1] > UINT32_MAX);
    CHECK(cull_frame(&sr, &s, b).full);

    /* Roster and hierarchy epochs invalidate even with identical ordered IDs. */
    a = b;
    ++s.roster;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);
    a = b;
    ++s.structure;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);

    /* Same pointer with a new scene-instance token must never reuse layout. */
    a = b;
    ++sr.model_bounds_scene;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);
    a = b;
    replacement_scene = s;
    b = collect_frame(&sr, &replacement_scene, &cfg);
    CHECK(b.generation != a.generation);
    CHECK(cull_frame(&sr, &replacement_scene, b).full);
    a = b;
    replacement_renderer = sr;
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&replacement_renderer, &replacement_scene, &cfg);
    CHECK(b.generation != a.generation);
    CHECK(cull_frame(&replacement_renderer, &replacement_scene, b).full);
    puts("PASS cold/default/moving-focus/dirty/metadata/bounds/order/owner controls");
    return true;
}

static bool test_membership_and_refusal(void)
{
    JceScene s;
    JceSceneRenderer sr;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    unsigned walks;
    setup(&s, &sr, &cfg);
    ++sr.model_bounds_scene;
    cfg.cull_radius = 10; /* Exact production effective radius is 80. */
    s.entities[2].transform.position.x = 200;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3); /* Conservative cold collection. */
    CHECK(cull_frame(&sr, &s, a).full);
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 2 && a.list->entities[1] == 2);
    cull_frame(&sr, &s, a);
    cfg.cull_focus_x = 150;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.list->count == 1 && b.list->entities[0] == 3 && b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);

    /* A huge surface whose origin is far away must retain the existing AABB test. */
    sr.has_focus[1] = true;
    sr.focus[1].wmin.x = -500; sr.focus[1].wmax.x = 500;
    sr.focus[1].wmin.z = -500; sr.focus[1].wmax.z = 500;
    cfg.cull_focus_x = 151;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 2 && a.list->entities[0] == 1 && a.list->entities[1] == 3);

    /* A shrinking result with an identical prefix still changes layout. */
    cfg.cull_radius = 900;
    cfg.cull_focus_x = 0;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3);
    move_entity(&s, 3);
    s.entities[2].transform.position.x = 2000;
    cfg.cull_focus_x = 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.list->count == 2 && b.list->entities[0] == 1 && b.list->entities[1] == 2);
    CHECK(b.generation != a.generation);
    move_entity(&s, 3);
    s.entities[2].transform.position.x = 2;
    cfg.cull_focus_x = 0.25f;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3 && a.generation != b.generation);

    /* Actual create/remove change count; no equality inferred from prior prefix. */
    cfg.cull_radius = 900;
    ++s.roster;
    s.count = 4;
    s.entities[3].entity = 4;
    s.entities[3].has_transform = true;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.list->count == 4 && b.generation != a.generation);
    a = b;
    --s.count;
    ++s.roster;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.list->count == 3 && b.generation != a.generation);

    /* A real reserve failure cannot cache its partial prefix indefinitely. */
    s.count = 12;
    ++s.roster;
    for (int i = 3; i < s.count; ++i) {
        s.entities[i].entity = (JceEntity)(i + 1);
        s.entities[i].has_transform = true;
    }
    g_fail_allocation = true;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(!a.valid && a.list->count < s.count && !sr.model_bounds_focus_collect_valid);
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(!b.valid && g_walks == walks + 1 && b.generation != a.generation);
    g_fail_allocation = false;
    walks = g_walks;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.valid && a.list->count == s.count && g_walks == walks + 1);
    CHECK(sr.model_bounds_focus_collect_valid);
    a = collect_frame(&sr, &s, &cfg); /* Return from cold-all to exact focus. */
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.valid && b.generation == a.generation && g_walks == walks);
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation && g_walks == walks + 1);
    puts("PASS membership/large-AABB/create/remove/reserve-refusal/recovery controls");
    return true;
}

static bool test_caster_and_dirty_failure(void)
{
    JceScene s;
    JceSceneRenderer sr;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    CullResult c;
    setup(&s, &sr, &cfg);
    sr.model_bounds_scene = 400;
    s.entities[1].caster = true;
    a = collect_frame(&sr, &s, &cfg);
    cull_frame(&sr, &s, a);
    a = collect_frame(&sr, &s, &cfg); /* Explicit bounded warmup. */
    cull_frame(&sr, &s, a);
    move_entity(&s, 2);
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation);
    c = cull_frame(&sr, &s, b);
    CHECK(c.full); /* A moving static caster retains the full rebuild. */
    s.entities[1].dynamic = true;
    ++s.data;
    cull_frame(&sr, &s, collect_frame(&sr, &s, &cfg));
    move_entity(&s, 2);
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    c = cull_frame(&sr, &s, b);
    CHECK(!c.full && !c.frozen && c.rebuilt == 1);
    move_entity(&s, 2);
    s.dirty_overflow = true;
    cfg.cull_focus_x += 0.125f;
    c = cull_frame(&sr, &s, collect_frame(&sr, &s, &cfg));
    CHECK(c.full);
    move_entity(&s, 2);
    s.dirty_base = 0;
    cfg.cull_focus_x += 0.125f;
    c = cull_frame(&sr, &s, collect_frame(&sr, &s, &cfg));
    CHECK(c.full);
    puts("PASS static/dynamic caster and dirty-ring refusal controls");
    return true;
}

static bool test_default_publication(void)
{
    JceScene s;
    JceSceneRenderer sr;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    CullResult c;
    unsigned resets, publications, walks, animation_resets;
    setup(&s, &sr, &cfg);
    sr.model_bounds_scene = 0;
    resets = g_bounds_resets;
    sr_model_bounds_enter(&sr, &s);
    CHECK(sr.model_bounds_scene == s.instance && g_bounds_resets == resets + 1);
    CHECK(!sr.model_bounds_enabled && !sr.model_bounds_focus_collect_valid);
    a = collect_frame(&sr, &s, &cfg);
    CHECK(cull_frame(&sr, &s, a).full);

    /* Actual publication owner, including a last async completion whose
     * pending count has already reached zero before the next collect. */
    publications = g_model_publications;
    sr.model_inflight = 0;
    sr_model_bounds_note(&sr, "async-ready.glb", &g_model);
    CHECK(sr.model_bounds_geometry_dirty && g_model_publications == publications);
    resets = g_bounds_resets;
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg); /* Deliberately stationary focus. */
    CHECK(g_bounds_resets == resets + 1 && !sr.model_bounds_geometry_dirty);
    CHECK(g_walks == walks + 1 && b.generation != a.generation);
    c = cull_frame(&sr, &s, b);
    CHECK(c.full && c.rebuilt == 3 && !sr.ecull_metadata_trusted);

    /* Several callback/eviction publications retire once at the boundary. */
    a = b;
    resets = g_bounds_resets;
    sr_model_bounds_note(&sr, "callback-ready.glb", &g_model);
    sr_model_bounds_note(&sr, "second-ready.glb", &g_model);
    sr_model_bounds_note(&sr, "evicted.glb", NULL);
    CHECK(g_bounds_resets == resets && sr.model_bounds_geometry_dirty);
    b = collect_frame(&sr, &s, &cfg);
    CHECK(g_bounds_resets == resets + 1 && b.generation != a.generation);
    CHECK(cull_frame(&sr, &s, b).full);

    /* Arrival after collect but before ecull reaches the actual second drain. */
    a = collect_frame(&sr, &s, &cfg);
    resets = g_bounds_resets;
    sr_model_bounds_note(&sr, "animation-ready.glb", &g_model);
    c = cull_frame(&sr, &s, a);
    CHECK(c.full && !sr.ecull_metadata_trusted && g_bounds_resets == resets + 1);
    CHECK(!sr.model_bounds_geometry_dirty);
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation);
    cull_frame(&sr, &s, b);

    /* A synchronous resolver inside a rebuild must survive its final valid
     * publication, then refresh at the following boundary. */
    ++s.data;
    a = collect_frame(&sr, &s, &cfg);
    g_publish_during_miss = true;
    c = cull_frame(&sr, &s, a);
    CHECK(c.full && sr.ecull_frz_valid && sr.model_bounds_geometry_dirty);
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && !sr.model_bounds_geometry_dirty);
    CHECK(cull_frame(&sr, &s, b).full);
    b = collect_frame(&sr, &s, &cfg); /* Warm exact-focus membership again. */
    cull_frame(&sr, &s, b);
    a = b;
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation && cull_frame(&sr, &s, b).frozen);

    /* Public invalidation's actual complete body owns loaded, pending and
     * failed slots and must also retire default-mode bounds. */
    sr.model_cache_capacity = sr.model_cache_used = 3;
    sr.model_cache = (SrModelCache **)calloc(3, sizeof(*sr.model_cache));
    CHECK(sr.model_cache != NULL);
    for (unsigned i = 0; i < 3; ++i) {
        sr.model_cache[i] = (SrModelCache *)calloc(1, sizeof(SrModelCache));
        CHECK(sr.model_cache[i] != NULL);
        sr.model_cache[i]->batch_members = malloc(8);
        CHECK(sr.model_cache[i]->batch_members != NULL);
    }
    sr.model_cache[0]->model = (JceModel *)malloc(sizeof(JceModel));
    CHECK(sr.model_cache[0]->model != NULL);
    sr.model_cache[1]->pending = true;
    sr.model_cache[1]->task = (JceAsyncTask *)malloc(sizeof(JceAsyncTask));
    sr.model_cache[1]->job = (struct SrModelJob *)calloc(1, sizeof(struct SrModelJob));
    CHECK(sr.model_cache[1]->task != NULL && sr.model_cache[1]->job != NULL);
    sr.model_cache[1]->job->cpu = malloc(8);
    CHECK(sr.model_cache[1]->job->cpu != NULL);
    sr.model_inflight = 1;
    sr.model_lookup_hint = sr.model_cache[0];
    resets = g_bounds_resets;
    animation_resets = g_animation_resets;
    unsigned cancels = g_task_cancels, jobs = g_job_releases, models = g_model_releases;
    jce_scene_renderer_invalidate_model_cache(&sr);
    CHECK(g_bounds_resets == resets + 1 && g_animation_resets == animation_resets);
    CHECK(g_task_cancels == cancels + 1 && g_job_releases == jobs + 1);
    CHECK(g_model_releases == models + 1);
    CHECK(!sr.model_cache && !sr.model_cache_capacity && !sr.model_cache_used);
    CHECK(!sr.model_lookup_hint && !sr.model_inflight);
    CHECK(!sr.ecull_frz_valid && !sr.ecull_metadata_valid && !sr.model_bounds_focus_collect_valid);
    a = b;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && cull_frame(&sr, &s, b).full);

    /* Reused scene address with a changed real instance retires previous data. */
    a = b;
    ++s.instance;
    sr_model_bounds_enter(&sr, &s);
    CHECK(sr.model_bounds_scene == s.instance && !sr.model_bounds_focus_collect_valid);
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation != a.generation && cull_frame(&sr, &s, b).full);
    sr.model_bounds_enabled = true;
    animation_resets = g_animation_resets;
    jce_scene_renderer_invalidate_model_cache(&sr);
    CHECK(g_animation_resets == animation_resets + 1);
    CHECK(g_model_publications == publications); /* No default dependency tracking. */
    puts("PASS actual default publication/drain/late-arrival/reset/instance controls");
    return true;
}

static bool test_far_origin_refresh(void)
{
    JceScene s;
    JceSceneRenderer sr;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    setup(&s, &sr, &cfg);
    s.instance = 900;
    sr_model_bounds_enter(&sr, &s);
    cfg.cull_focus_x = 150;
    cfg.cull_radius = 10;
    s.entities[0].transform.position.x = 0;
    s.entities[1].transform.position.x = 1000;
    s.entities[2].transform.position.x = 150;
    for (int i = 0; i < 3; ++i) {
        TestEntity *e = &s.entities[i];
        e->bounds_valid = true;
        e->bounds_min.x = e->transform.position.x - 1;
        e->bounds_max.x = e->transform.position.x + 1;
        e->bounds_min.z = -1; e->bounds_max.z = 1;
    }
    s.entities[0].bounds_min.x = -500;
    s.entities[0].bounds_max.x = 500; /* Distant origin, surface reaches focus. */

    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3 && a.list->entities[0] == 1);
    /* Publication at the second drain after a cold ALL collection cannot
     * leave the ALL cache permanently valid while its bounds proof is false. */
    sr_model_bounds_note(&sr, "late-cold-ready.glb", &g_model);
    CHECK(cull_frame(&sr, &s, a).full);
    CHECK(!sr.model_bounds_focus_collect_valid);
    unsigned walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(g_walks == walks + 1 && b.list->count == 3);
    CHECK(sr.model_bounds_focus_collect_valid);
    CHECK(cull_frame(&sr, &s, b).full);
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 2 && a.list->entities[0] == 1 && a.list->entities[1] == 3);
    CHECK(cull_frame(&sr, &s, a).full);
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation && cull_frame(&sr, &s, b).frozen);

    /* An unrelated publication clears all cached bounds; the large surface
     * must remain admitted while actual bounds are rebuilt from scratch. */
    sr_model_bounds_note(&sr, "unrelated-ready.glb", &g_model);
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3 && a.list->entities[0] == 1);
    CHECK(cull_frame(&sr, &s, a).full);
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.list->count == 2 && b.list->entities[0] == 1 && b.list->entities[1] == 3);
    CHECK(cull_frame(&sr, &s, b).full);

    /* The same invalid-proof recovery works when focus was deliberately off. */
    cfg.cull_focus_enabled = false;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 3);
    sr_model_bounds_note(&sr, "focus-off-ready.glb", &g_model);
    CHECK(cull_frame(&sr, &s, a).full);
    CHECK(!sr.model_bounds_focus_collect_valid);
    walks = g_walks;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(g_walks == walks + 1 && b.list->count == 3 && sr.model_bounds_focus_collect_valid);
    cull_frame(&sr, &s, b);
    cfg.cull_focus_enabled = true;
    a = collect_frame(&sr, &s, &cfg);
    CHECK(a.list->count == 2 && a.list->entities[0] == 1 && a.list->entities[1] == 3);
    cull_frame(&sr, &s, a);
    cfg.cull_focus_x += 0.125f;
    b = collect_frame(&sr, &s, &cfg);
    CHECK(b.generation == a.generation && cull_frame(&sr, &s, b).frozen);
    puts("PASS cold/far-origin/publication/second-drain/focus-off recovery controls");
    return true;
}

static bool test_explicit_bypass(void)
{
    JceScene s;
    JceSceneRenderer sr;
    JceSceneRenderConfig cfg;
    CollectResult a, b;
    setup(&s, &sr, &cfg);
    a = collect_frame(&sr, &s, &cfg);
    cull_frame(&sr, &s, a);
    a = collect_frame(&sr, &s, &cfg); /* Explicit bounded warmup. */
    cull_frame(&sr, &s, a);
    cfg.cull_focus_x += 0.25f;
    b = collect_frame(&sr, &s, &cfg);
    if (g_collect_off) CHECK(b.generation != a.generation);
    else CHECK(b.generation == a.generation);
    cull_frame(&sr, &s, b);
    if (g_metadata_off) CHECK(!sr.ecull_metadata_trusted);
    puts("PASS explicit diagnostic bypass");
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        g_collect_off = strcmp(argv[1], "collect-off") == 0;
        g_metadata_off = strcmp(argv[1], "metadata-off") == 0;
        return test_explicit_bypass() ? 0 : 1;
    }
    if (!test_ordinary_path() || !test_membership_and_refusal() ||
        !test_caster_and_dirty_failure() || !test_default_publication() ||
        !test_far_origin_refresh()) return 1;
    printf("PASS CPU owner controls: walks=%u allocation_calls=%u rebuild_ports=%u dirty_drains=%u\n",
           g_walks, g_allocations, g_rebuilds, g_drains);
    return 0;
}
