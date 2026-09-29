/* test_jce_runtime_collider_resolve.c
 *
 * Regression: model-based colliders (Compound / Mesh) must spawn a physics
 * body in the EDITOR / loose-asset case, where the scene stores a project-
 * relative model path that does NOT resolve against the process CWD.
 *
 * ROOT CAUSE (fixed): rt_spawn_cooked_body / rt_try_load_cached_collider read
 * the raw `model_path` (and "<model_path>.jcol" sibling) directly via jce_fs.
 * The editor never chdir's and mounts no global VFS in loose Play, so the raw
 * relative path missed the filesystem — the collider silently failed to spawn
 * while the visual mesh still rendered (the renderer resolves through the
 * editor asset cache).  Crates worked because a BoxCollider needs no file.
 *
 * THE FIX: JceRuntimeDesc.resolve_path_fn — an optional host resolver the
 * runtime calls to map the stored path to a readable host path before the
 * direct load (the same resolution the renderer uses).  The editor wires it
 * to jce_editor_scene_asset_cache_resolve_mesh_path.
 *
 * This test cooks a box trimesh, serialises it to a ".jcol" blob beside a
 * "real" model name, then drives jce_runtime_create on a scene whose
 * CompoundCollider points at a PHANTOM relative path that only a resolver can
 * map to the real one.  WITH the resolver a downward raycast hits the cooked
 * trimesh (body spawned); WITHOUT it the raycast misses (the unfixed
 * behaviour).  Links the full engine aggregate because jce_runtime_create with
 * enable_physics stands up Bullet + the whole middleware stack.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* The relative path stored in the scene — deliberately under a directory that
 * does not exist relative to the test CWD, so only a resolver can map it. */
#define PHANTOM_MODEL "phantom_dir/rt_col_resolve.obj"
/* The real model name the resolver maps to; its sibling ".jcol" is written to
 * the test CWD below. */
#define REAL_MODEL    "rt_col_resolve_real.obj"
#define REAL_BLOB     REAL_MODEL ".jcol"

void setUp(void)    {}
void tearDown(void) { remove(REAL_BLOB); }

/* Resolver: map only the phantom path to the real one (return false otherwise,
 * exercising the runtime's raw-path fallback for unrelated paths). */
static bool test_resolve(void *ud, const char *in, char *out, int n)
{
    (void)ud;
    if (in && strcmp(in, PHANTOM_MODEL) == 0) {
        snprintf(out, (size_t)n, "%s", REAL_MODEL);
        return true;
    }
    return false;
}

/* Cook a unit box (-1..1) trimesh and serialise it to REAL_BLOB. */
static void write_box_blob(void)
{
    static const float v[] = {
        -1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1,   /* z=-1 */
        -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,   /* z=+1 */
    };
    static const uint32_t idx[] = {
        0,1,2, 0,2,3,   4,6,5, 4,7,6,   /* back, front  */
        0,4,5, 0,5,1,   3,2,6, 3,6,7,   /* bottom, top  */
        0,3,7, 0,7,4,   1,5,6, 1,6,2,   /* left, right  */
    };

    JceColliderPart part;
    memset(&part, 0, sizeof part);
    part.name         = "box";
    part.vertices     = v;
    part.vertex_count = 8;
    part.indices      = idx;
    part.index_count  = (uint32_t)(sizeof idx / sizeof idx[0]);
    /* identity part→model transform */
    part.transform[0] = part.transform[5] = part.transform[10] =
        part.transform[15] = 1.0f;

    JceColliderCookConfig cfg = jce_collider_cook_config_default();
    cfg.mode      = JCE_COLLIDER_MODE_TRIANGLE_MESH;
    cfg.split     = JCE_COLLIDER_SPLIT_BY_PART;
    cfg.is_static = true;

    JceCookedCollider cooked;
    memset(&cooked, 0, sizeof cooked);
    TEST_ASSERT_TRUE_MESSAGE(jce_collider_cook(&part, 1, &cfg, &cooked),
                             "cook box trimesh");
    TEST_ASSERT_GREATER_THAN_UINT32(0u, cooked.child_count);

    void    *bytes = NULL;
    uint32_t size  = 0;
    TEST_ASSERT_TRUE_MESSAGE(jce_collider_serialize(&cooked, &bytes, &size),
                             "serialize cooked collider");
    jce_collider_cooked_free(&cooked);

    FILE *f = fopen(REAL_BLOB, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, f));
    fclose(f);
    jce_free(bytes);
}

/* Build a one-entity scene with a static CompoundCollider at the origin
 * referencing PHANTOM_MODEL. */
static JceScene *make_scene(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Box");

    JceTransform *tf = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(tf);
    tf->position = jce_v3(0.0f, 0.0f, 0.0f);
    tf->rotation.x = tf->rotation.y = tf->rotation.z = 0.0f;
    tf->rotation.w = 1.0f;
    tf->scale = jce_v3(1.0f, 1.0f, 1.0f);

    JceCompoundColliderComponent cc;
    memset(&cc, 0, sizeof cc);
    snprintf(cc.model_path, sizeof cc.model_path, "%s", PHANTOM_MODEL);
    cc.mode      = JCE_COLLIDER_MODE_AUTO;   /* irrelevant: blob loads as-is */
    cc.split     = JCE_COLLIDER_SPLIT_BY_PART;
    cc.is_static = true;
    cc.friction  = 0.5f;
    jce_scene_set_compound_collider(s, e, &cc);
    return s;
}

/* Returns true if a downward ray from above the origin hits a body. */
static bool ray_hits(JceRuntime *rt)
{
    const float origin[3] = { 0.0f, 5.0f, 0.0f };
    const float dir[3]    = { 0.0f, -1.0f, 0.0f };
    float hit_y = 0.0f, n[3] = { 0 };
    return jce_runtime_ground_raycast(rt, origin, dir, 20.0f, &hit_y, n);
}

/* WITH the resolver, the phantom path maps to the real ".jcol" → a static
 * trimesh body spawns → the ray hits.  This FAILS on the unfixed runtime
 * (which ignores resolve_path_fn and reads the raw phantom path). */
static void test_collider_spawns_with_resolver(void)
{
    write_box_blob();
    JceScene *s = make_scene();

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene           = s;
    rd.pak             = NULL;              /* loose host-FS, like the editor */
    rd.enable_physics  = true;
    rd.resolve_path_fn = test_resolve;

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);    /* harmless; body is static */

    TEST_ASSERT_TRUE_MESSAGE(ray_hits(rt),
        "collider body must spawn when the resolver maps the model path");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* WITHOUT a resolver, the raw phantom path resolves nowhere → no body → the
 * ray misses.  Documents the unfixed behaviour and proves the resolver (not
 * some unrelated default) is what makes the collider appear. */
static void test_collider_absent_without_resolver(void)
{
    write_box_blob();
    JceScene *s = make_scene();

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene           = s;
    rd.pak             = NULL;
    rd.enable_physics  = true;
    rd.resolve_path_fn = NULL;             /* no resolver */

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    TEST_ASSERT_FALSE_MESSAGE(ray_hits(rt),
        "without a resolver the phantom path cannot load → no collider body");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_collider_spawns_with_resolver);
    RUN_TEST(test_collider_absent_without_resolver);
    return UNITY_END();
}
