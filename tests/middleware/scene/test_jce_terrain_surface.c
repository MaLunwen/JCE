#include "jce_terrain_surface_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static JceTerrain *make_terrain(bool slope)
{
    JceTerrain *terrain = jce_terrain_create(9, 9, 8.0f, 8.0f, 10.0f, 8);
    TEST_ASSERT_NOT_NULL(terrain);
    uint16_t heights[81];
    for (int z = 0; z < 9; ++z)
        for (int x = 0; x < 9; ++x)
            heights[z * 9 + x] = slope
                ? (uint16_t)(4096 + 2048 * x + 4096 * z)
                : 32768u;
    TEST_ASSERT_TRUE(jce_terrain_import_heightmap_r16(terrain, heights, 9, 9));
    return terrain;
}

static void test_bed_uses_transformed_cell_centres(void)
{
    JceTerrain *terrain = make_terrain(true);
    jce_mat4 world = jce_m4_identity();
    world.col[0].x = 2.0f;
    world.col[1].y = 2.0f;
    world.col[2].z = 0.5f;
    world.col[3].x = 10.0f;
    world.col[3].y = -2.0f;
    world.col[3].z = 20.0f;

    float bed[16];
    TEST_ASSERT_TRUE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 1.0f, 10.0f, 20.0f, 3.0f, bed));
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) {
            const float local_x = ((float)x + 0.5f) / 2.0f;
            const float local_z = ((float)z + 0.5f) / 0.5f;
            const float norm = (4096.0f + 2048.0f * local_x + 4096.0f * local_z) / 65535.0f;
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.0f * norm - 5.0f, bed[z * 4 + x]);
        }

    jce_terrain_free(terrain);
}

static void test_bed_projects_onto_tilted_terrain(void)
{
    JceTerrain *terrain = make_terrain(false);
    jce_mat4 world = jce_m4_identity();
    const float c = 0.96824584f, s = 0.25f;
    world.col[1].y = c;
    world.col[1].z = s;
    world.col[2].y = -s;
    world.col[2].z = c;

    float bed[16];
    TEST_ASSERT_TRUE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 2.0f, 3.0f, 4.0f, bed));
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) {
            const float world_z = 3.0f + ((float)z + 0.5f) * 0.5f;
            const float local_height = 10.0f * (32768.0f / 65535.0f);
            const float expected = (local_height - s * world_z) / c - 4.0f;
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected, bed[z * 4 + x]);
        }

    jce_terrain_free(terrain);
}

static void test_bed_rejects_incomplete_or_out_of_range_samples(void)
{
    JceTerrain *terrain = make_terrain(false);
    jce_mat4 world = jce_m4_identity();
    float bed[16];
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 6.5f, 0.0f, 0.0f, bed));
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 0.0f, 0.0f, -65.0f, bed));
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 0.0f, 0.0f, 70.0f, bed));

    jce_terrain_set_hole(terrain, 0, 0, true);
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 0.0f, 0.0f, 0.0f, bed));
    TEST_ASSERT_TRUE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 1.0f, 0.0f, 0.0f, bed));

    jce_terrain_free(terrain);
}

static void test_bed_rejects_streamed_or_singular_surface(void)
{
    JceTerrain *streamed = jce_terrain_create_procedural(
        9, 9, 8.0f, 8.0f, 10.0f, 8, 1, 1u, 1.0f);
    TEST_ASSERT_NOT_NULL(streamed);
    jce_mat4 world = jce_m4_identity();
    float bed[16];
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        streamed, &world, 4u, 0.5f, 0.0f, 0.0f, 0.0f, bed));
    TEST_ASSERT_EQUAL_INT(0, jce_terrain_resident_tiles(streamed));
    jce_terrain_free(streamed);

    JceTerrain *terrain = make_terrain(false);
    world.col[0] = world.col[2];
    TEST_ASSERT_FALSE(jce_terrain_surface_sample_bed(
        terrain, &world, 4u, 0.5f, 0.0f, 0.0f, 0.0f, bed));
    jce_terrain_free(terrain);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bed_uses_transformed_cell_centres);
    RUN_TEST(test_bed_projects_onto_tilted_terrain);
    RUN_TEST(test_bed_rejects_incomplete_or_out_of_range_samples);
    RUN_TEST(test_bed_rejects_streamed_or_singular_surface);
    return UNITY_END();
}
