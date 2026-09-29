/* test_jce_foliage_scatter.c
 *
 * Unit tests for the deterministic vegetation scatter (jce_foliage_scatter):
 *   - identical inputs reproduce identical output (determinism)
 *   - instance count == density*area and all placements fall inside the area
 *   - with a terrain, Y is sampled from the heightfield (not the origin)
 */

#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>

#include "unity.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#define VEG_PREFAB "jce_veg_roundtrip.prefab.json"

void setUp(void)    {}
void tearDown(void) { remove(VEG_PREFAB); }

static JceFoliageScatterParams base_params(uint32_t seed)
{
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed = seed;
    p.density = 1.0f;          /* 1 per unit² */
    p.area_x = 8.0f;
    p.area_z = 8.0f;
    p.max_slope_deg = 90.0f;   /* no slope culling */
    p.scale_min = 1.0f;
    p.scale_max = 1.0f;
    return p;
}

static void test_scatter_deterministic(void)
{
    JceFoliageScatterParams p = base_params(42);
    jce_vec3 o = { 5.0f, 99.0f, 5.0f };
    JceFoliageInstance a[128], b[128];
    uint32_t na = jce_foliage_scatter(&p, NULL, &o, a, 128);
    uint32_t nb = jce_foliage_scatter(&p, NULL, &o, b, 128);
    TEST_ASSERT_TRUE(na > 0);
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    for (uint32_t i = 0; i < na; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(a[i].pos[0], b[i].pos[0]);
        TEST_ASSERT_EQUAL_FLOAT(a[i].pos[2], b[i].pos[2]);
        TEST_ASSERT_EQUAL_FLOAT(a[i].rot_y,  b[i].rot_y);
        TEST_ASSERT_EQUAL_FLOAT(a[i].scale,  b[i].scale);
    }
}

static void test_scatter_count_and_bounds(void)
{
    JceFoliageScatterParams p = base_params(7);
    jce_vec3 o = { 5.0f, 99.0f, 5.0f };   /* area [1,9] x [1,9] on XZ */
    JceFoliageInstance inst[256];
    uint32_t n = jce_foliage_scatter(&p, NULL, &o, inst, 256);
    TEST_ASSERT_EQUAL_UINT32(64u, n);     /* density(1) * 8 * 8 */
    for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(inst[i].pos[0] >= 1.0f - 1e-4f && inst[i].pos[0] <= 9.0f + 1e-4f);
        TEST_ASSERT_TRUE(inst[i].pos[2] >= 1.0f - 1e-4f && inst[i].pos[2] <= 9.0f + 1e-4f);
        TEST_ASSERT_EQUAL_FLOAT(99.0f, inst[i].pos[1]);  /* no terrain -> origin.y */
        TEST_ASSERT_EQUAL_FLOAT(1.0f, inst[i].scale);    /* scale_min == scale_max */
    }
}

static void test_scatter_follows_terrain(void)
{
    /* Flat terrain (heights 0) spanning world [0,64] on XZ. */
    JceTerrain *t = jce_terrain_create(65, 65, 64.0f, 64.0f, 10.0f, 64);
    TEST_ASSERT_NOT_NULL(t);

    JceFoliageScatterParams p = base_params(7);
    jce_vec3 o = { 5.0f, 99.0f, 5.0f };   /* candidates [1,9]x[1,9] are inside terrain */
    JceFoliageInstance inst[256];
    uint32_t n = jce_foliage_scatter(&p, t, &o, inst, 256);
    TEST_ASSERT_EQUAL_UINT32(64u, n);
    for (uint32_t i = 0; i < n; ++i) {
        /* Y comes from the (flat, height 0) terrain, NOT origin.y(99). */
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, inst[i].pos[1]);
    }
    jce_terrain_free(t);
}

/* The VegetationScatter component must survive the registry-driven scene
 * serialize → parse round-trip (every authorable field). */
static void test_component_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "veg");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceVegetationScatterComponent vs; memset(&vs, 0, sizeof vs);
    snprintf(vs.mesh_path, sizeof vs.mesh_path, "%s", "models/grass.glb");
    snprintf(vs.albedo_path, sizeof vs.albedo_path, "%s", "tex/grass.png");
    snprintf(vs.baked_placement_path, sizeof vs.baked_placement_path, "%s",
             "generated/foliage/grass.foliage.bin");
    vs.density = 2.5f; vs.seed = 9876u; vs.area_x = 12.0f; vs.area_z = 34.0f;
    vs.max_slope_deg = 40.0f; vs.scale_min = 0.5f; vs.scale_max = 1.5f;
    vs.tint[0] = 0.2f; vs.tint[1] = 0.8f; vs.tint[2] = 0.3f;
    vs.align_to_normal = true; vs.cast_shadow = false; vs.visible = true;
    jce_scene_set_vegetation_scatter(s, e, &vs);

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(s, e, VEG_PREFAB));
    jce_scene_destroy(s);

    JceScene *s2 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s2);
    JceEntity root = jce_prefab_instantiate_file(s2, VEG_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);

    JceVegetationScatterComponent *g = jce_scene_get_vegetation_scatter(s2, root);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_STRING("models/grass.glb", g->mesh_path);
    TEST_ASSERT_EQUAL_STRING("tex/grass.png", g->albedo_path);
    TEST_ASSERT_EQUAL_STRING("generated/foliage/grass.foliage.bin",
                             g->baked_placement_path);
    TEST_ASSERT_EQUAL_FLOAT(2.5f,  g->density);
    TEST_ASSERT_EQUAL_UINT32(9876u, g->seed);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, g->area_x);
    TEST_ASSERT_EQUAL_FLOAT(34.0f, g->area_z);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, g->max_slope_deg);
    TEST_ASSERT_EQUAL_FLOAT(0.5f,  g->scale_min);
    TEST_ASSERT_EQUAL_FLOAT(1.5f,  g->scale_max);
    TEST_ASSERT_EQUAL_FLOAT(0.8f,  g->tint[1]);
    TEST_ASSERT_TRUE(g->align_to_normal);
    TEST_ASSERT_FALSE(g->cast_shadow);
    TEST_ASSERT_TRUE(g->visible);

    jce_scene_destroy(s2);
}

/* ── align_to_normal: the field that was authored but never wired ─────
 *
 * align_to_normal was parsed, serialized, editor-exposed and round-trip tested
 * while doing nothing: the scatter computed a terrain gradient for its slope
 * test and discarded it, and the instance matrix only ever built a yaw.  These
 * lock the two properties that make the toggle real -- normals are produced on
 * request, and asking for them does not perturb the RNG. */

static float slope_h(float x, float z) { (void)z; return x * 0.5f; }

static void test_normals_absent_unless_requested(void)
{
    const jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed = 7u; p.density = 0.5f; p.area_x = 8.0f; p.area_z = 8.0f;
    p.max_slope_deg = 90.0f; p.scale_min = 1.0f; p.scale_max = 1.0f;
    p.want_normals = false;

    JceFoliageInstance out[256];
    int n = jce_foliage_scatter(&p, NULL, &origin, out, 256);
    TEST_ASSERT_TRUE(n > 0);
    for (int i = 0; i < n; i++) {
        /* Default is straight up, never uninitialised garbage. */
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out[i].normal[0]);
        TEST_ASSERT_EQUAL_FLOAT(1.0f, out[i].normal[1]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out[i].normal[2]);
    }
}

static void test_requesting_normals_does_not_move_instances(void)
{
    const jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed = 99u; p.density = 0.6f; p.area_x = 10.0f; p.area_z = 10.0f;
    p.max_slope_deg = 90.0f; p.scale_min = 0.5f; p.scale_max = 2.0f;

    JceFoliageInstance a[512], b[512];
    p.want_normals = false;
    int na = jce_foliage_scatter(&p, NULL, &origin, a, 512);
    p.want_normals = true;
    int nb = jce_foliage_scatter(&p, NULL, &origin, b, 512);

    /* Deriving normals consumes no randomness, so the kept set must be
     * byte-identical.  This is what keeps scatter cookable. */
    TEST_ASSERT_EQUAL_INT(na, nb);
    for (int i = 0; i < na; i++) {
        TEST_ASSERT_EQUAL_FLOAT(a[i].pos[0], b[i].pos[0]);
        TEST_ASSERT_EQUAL_FLOAT(a[i].pos[1], b[i].pos[1]);
        TEST_ASSERT_EQUAL_FLOAT(a[i].pos[2], b[i].pos[2]);
        TEST_ASSERT_EQUAL_FLOAT(a[i].rot_y,  b[i].rot_y);
        TEST_ASSERT_EQUAL_FLOAT(a[i].scale,  b[i].scale);
    }
}

static void test_normals_are_unit_length(void)
{
    const jce_vec3 origin = { 0.0f, 0.0f, 0.0f };
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed = 3u; p.density = 0.4f; p.area_x = 6.0f; p.area_z = 6.0f;
    p.max_slope_deg = 90.0f; p.scale_min = 1.0f; p.scale_max = 1.0f;
    p.want_normals = true;

    JceFoliageInstance out[256];
    int n = jce_foliage_scatter(&p, NULL, &origin, out, 256);
    TEST_ASSERT_TRUE(n > 0);
    for (int i = 0; i < n; i++) {
        const float *v = out[i].normal;
        const float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, len);
    }
    (void)slope_h;
}

typedef struct {
    float min_x;
    int calls;
} TestSurface;

static bool sample_test_surface(void *user, float world_x, float world_z,
                                bool want_normal, float *out_world_y,
                                float out_world_normal[3])
{
    TestSurface *surface = (TestSurface *)user;
    surface->calls++;
    if (world_x < surface->min_x) return false;
    *out_world_y = 7.0f + 0.25f * world_x - 0.5f * world_z;
    if (want_normal) {
        /* Deliberately non-unit: the public contract normalizes callbacks. */
        out_world_normal[0] = -0.5f;
        out_world_normal[1] = 2.0f;
        out_world_normal[2] = 1.0f;
    }
    return true;
}

static void test_surface_callback_is_world_space_and_rejects(void)
{
    JceFoliageScatterParams p = base_params(123u);
    p.area_x = 8.0f;
    p.area_z = 8.0f;
    p.want_normals = true;
    jce_vec3 origin = { 20.0f, 5.0f, -10.0f };
    TestSurface surface = { 20.0f, 0 };
    JceFoliageInstance inst[128];

    const uint32_t n = jce_foliage_scatter_on_surface(
        &p, sample_test_surface, &surface, &origin, inst, 128);
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(n < 64u);
    TEST_ASSERT_EQUAL_INT(64, surface.calls);
    for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(inst[i].pos[0] >= 20.0f);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f,
            7.0f + 0.25f * inst[i].pos[0] - 0.5f * inst[i].pos[2],
            inst[i].pos[1]);
        const float len = sqrtf(inst[i].normal[0] * inst[i].normal[0] +
                                inst[i].normal[1] * inst[i].normal[1] +
                                inst[i].normal[2] * inst[i].normal[2]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, len);
    }
}

static void test_surface_sampling_preserves_random_sequence(void)
{
    JceFoliageScatterParams p = base_params(321u);
    jce_vec3 origin = { 20.0f, 5.0f, -10.0f };
    TestSurface surface = { -1000.0f, 0 };
    JceFoliageInstance flat[128], sampled[128];

    const uint32_t nf = jce_foliage_scatter(&p, NULL, &origin, flat, 128);
    const uint32_t ns = jce_foliage_scatter_on_surface(
        &p, sample_test_surface, &surface, &origin, sampled, 128);
    TEST_ASSERT_EQUAL_UINT32(nf, ns);
    for (uint32_t i = 0; i < nf; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(flat[i].pos[0], sampled[i].pos[0]);
        TEST_ASSERT_EQUAL_FLOAT(flat[i].pos[2], sampled[i].pos[2]);
        TEST_ASSERT_EQUAL_FLOAT(flat[i].rot_y, sampled[i].rot_y);
        TEST_ASSERT_EQUAL_FLOAT(flat[i].scale, sampled[i].scale);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scatter_deterministic);
    RUN_TEST(test_scatter_count_and_bounds);
    RUN_TEST(test_scatter_follows_terrain);
    RUN_TEST(test_component_roundtrip);
    RUN_TEST(test_normals_absent_unless_requested);
    RUN_TEST(test_requesting_normals_does_not_move_instances);
    RUN_TEST(test_normals_are_unit_length);
    RUN_TEST(test_surface_callback_is_world_space_and_rejects);
    RUN_TEST(test_surface_sampling_preserves_random_sequence);
    return UNITY_END();
}
