/* test_jce_terrain_holes.c
 *
 * Unit tests for terrain holes (large-world #C: cut cells for caves / tunnels /
 * building interiors):
 *   - set_hole / cell_is_hole / has_holes round-trip on the monolithic grid
 *   - build_collision_mesh drops 6 indices per cut cell (physics + render agree)
 *   - chunk render mesh drops 6 indices per cut cell at LOD0
 *   - the circular hole brush cuts / fills cells under it
 *   - .bin serialization round-trips the mask (v2), and a holeless terrain
 *     stays v1 (load yields has_holes == false)
 *
 * Exercises the REAL terrain mesh + IO path; no editor / GPU.
 */

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_alloc.h>   /* build_collision_mesh output frees via jce_free */

#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void temp_path(char *out, size_t cap, const char *leaf)
{
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(out, cap, "%s/%s", tmp, leaf);
}

/* ---- mask set / get / has ---- */
static void test_hole_set_get(void)
{
    JceTerrain *t = jce_terrain_create(9, 9, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(jce_terrain_has_holes(t));
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, 3, 3));

    jce_terrain_set_hole(t, 3, 3, true);
    TEST_ASSERT_TRUE(jce_terrain_cell_is_hole(t, 3, 3));
    TEST_ASSERT_TRUE(jce_terrain_has_holes(t));
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, 4, 3));

    /* out-of-range and the last (non-cell) row/col are always solid */
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, 8, 8));
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, -1, 0));

    jce_terrain_set_hole(t, 3, 3, false);
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, 3, 3));
    TEST_ASSERT_FALSE(jce_terrain_has_holes(t));
    jce_terrain_free(t);
}

/* ---- collision mesh drops 6 indices per cut cell ---- */
static void test_collision_mesh_skips_holes(void)
{
    const int W = 9, H = 9;
    JceTerrain *t = jce_terrain_create(W, H, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t);

    float *v0 = NULL, *v1 = NULL;
    uint32_t *i0 = NULL, *i1 = NULL;
    uint32_t vc0 = 0, ic0 = 0, vc1 = 0, ic1 = 0;

    TEST_ASSERT_TRUE(jce_terrain_build_collision_mesh(t, &v0, &vc0, &i0, &ic0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)((W - 1) * (H - 1) * 6), ic0);

    jce_terrain_set_hole(t, 2, 2, true);
    jce_terrain_set_hole(t, 5, 6, true);
    TEST_ASSERT_TRUE(jce_terrain_build_collision_mesh(t, &v1, &vc1, &i1, &ic1));
    TEST_ASSERT_EQUAL_UINT32(ic0 - 12u, ic1);   /* two cells * 6 indices */
    TEST_ASSERT_EQUAL_UINT32(vc0, vc1);          /* vertex grid unchanged */

    jce_free(v0); jce_free(i0); jce_free(v1); jce_free(i1);
    jce_terrain_free(t);
}

static void test_collision_mesh_region_is_exact_and_bounded(void)
{
    JceTerrain *t = jce_terrain_create(9, 9, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t);
    jce_terrain_set_hole(t, 3, 3, true);

    float *verts = NULL;
    uint32_t *indices = NULL;
    uint32_t vcount = 0, icount = 0;
    TEST_ASSERT_TRUE(jce_terrain_build_collision_mesh_region(
        t, 4.1f, 4.1f, 7.9f, 7.9f, 9u,
        &verts, &vcount, &indices, &icount));
    TEST_ASSERT_EQUAL_UINT32(9u, vcount);
    TEST_ASSERT_EQUAL_UINT32(18u, icount); /* four cells minus one hole */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.0f, verts[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.0f, verts[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.0f, verts[(vcount - 1u) * 3u]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 8.0f, verts[(vcount - 1u) * 3u + 2u]);
    jce_free(verts);
    jce_free(indices);

    verts = NULL;
    indices = NULL;
    vcount = icount = 0;
    TEST_ASSERT_FALSE(jce_terrain_build_collision_mesh_region(
        t, 4.1f, 4.1f, 7.9f, 7.9f, 8u,
        &verts, &vcount, &indices, &icount));
    TEST_ASSERT_NULL(verts);
    TEST_ASSERT_NULL(indices);
    TEST_ASSERT_EQUAL_UINT32(0u, vcount);
    TEST_ASSERT_EQUAL_UINT32(0u, icount);
    jce_terrain_free(t);
}

/* ---- chunk render mesh drops 6 indices per cut cell at LOD0 ---- */
static void test_chunk_mesh_skips_holes(void)
{
    JceTerrain *t = jce_terrain_create(9, 9, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t);

    int vcap = 0, icap = 0;
    jce_terrain_chunk_mesh_size(t, 0, 0, 0, &vcap, &icap);
    TEST_ASSERT_TRUE(vcap > 0 && icap > 0);

    JceTerrainVertex *verts = (JceTerrainVertex *)malloc((size_t)vcap * sizeof(*verts));
    uint32_t         *idx   = (uint32_t *)malloc((size_t)icap * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(verts);
    TEST_ASSERT_NOT_NULL(idx);

    int vo0 = 0, io0 = 0, vo1 = 0, io1 = 0;
    jce_terrain_chunk_build_mesh(t, 0, 0, 0, verts, vcap, idx, icap, &vo0, &io0);
    TEST_ASSERT_TRUE(io0 > 0);

    jce_terrain_set_hole(t, 1, 1, true);   /* inside chunk 0 */
    jce_terrain_chunk_build_mesh(t, 0, 0, 0, verts, vcap, idx, icap, &vo1, &io1);
    TEST_ASSERT_EQUAL_INT(io0 - 6, io1);

    free(verts); free(idx);
    jce_terrain_free(t);
}

/* ---- circular brush cuts then fills ---- */
static void test_hole_brush(void)
{
    JceTerrain *t = jce_terrain_create(17, 17, 32.0f, 32.0f, 8.0f, 16);
    TEST_ASSERT_NOT_NULL(t);

    /* Cut a disc at the terrain centre. */
    jce_terrain_hole_apply(t, 16.0f, 16.0f, 4.0f, false);
    TEST_ASSERT_TRUE(jce_terrain_has_holes(t));
    /* The centre cell must be cut. */
    TEST_ASSERT_TRUE(jce_terrain_cell_is_hole(t, 8, 8));

    /* Erase the same disc -> centre solid again. */
    jce_terrain_hole_apply(t, 16.0f, 16.0f, 4.0f, true);
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(t, 8, 8));
    jce_terrain_free(t);
}

/* ---- .bin serialization: v2 round-trips holes; holeless stays v1 ---- */
static void test_serialize_roundtrip(void)
{
    char meta[512];
    temp_path(meta, sizeof meta, "jce_test_terrain_holes.terrain.json");

    JceTerrain *t = jce_terrain_create(9, 9, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t);
    jce_terrain_set_hole(t, 2, 3, true);
    jce_terrain_set_hole(t, 6, 1, true);
    TEST_ASSERT_TRUE(jce_terrain_save_file(t, meta));
    jce_terrain_free(t);

    JceTerrain *r = jce_terrain_load_file(meta);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(jce_terrain_has_holes(r));
    TEST_ASSERT_TRUE(jce_terrain_cell_is_hole(r, 2, 3));
    TEST_ASSERT_TRUE(jce_terrain_cell_is_hole(r, 6, 1));
    TEST_ASSERT_FALSE(jce_terrain_cell_is_hole(r, 0, 0));
    jce_terrain_free(r);

    /* Holeless terrain: save (v1) then load -> no holes. */
    char meta2[512];
    temp_path(meta2, sizeof meta2, "jce_test_terrain_nohole.terrain.json");
    JceTerrain *t2 = jce_terrain_create(9, 9, 16.0f, 16.0f, 8.0f, 8);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_TRUE(jce_terrain_save_file(t2, meta2));
    jce_terrain_free(t2);
    JceTerrain *r2 = jce_terrain_load_file(meta2);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(jce_terrain_has_holes(r2));
    jce_terrain_free(r2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hole_set_get);
    RUN_TEST(test_collision_mesh_skips_holes);
    RUN_TEST(test_collision_mesh_region_is_exact_and_bounded);
    RUN_TEST(test_chunk_mesh_skips_holes);
    RUN_TEST(test_hole_brush);
    RUN_TEST(test_serialize_roundtrip);
    return UNITY_END();
}
