/*
 * test_jce_hlod_bake.c
 *
 * Verifies the engine HLOD bake (jce_hlod_bake_proxy, Direction A2): two source
 * quads with different world matrices are merged into WORLD space, simplified,
 * and written as one proxy .glb.  Asserts the merge transforms vertices (the
 * baked POSITION bounds span both translated quads) and the output is a valid,
 * non-empty GLB.  Structural / headless (no bgfx).
 */

#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_hlod_bake.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct { float pos[3]; float nrm[3]; } V;   /* interleaved → strided feed */

static void mat_translate(float *m, float x, float y, float z)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x; m[13] = y; m[14] = z;
}

static void test_hlod_bake_merge_worldspace(void)
{
    V verts[4] = {
        { {-1,0,-1}, {0,1,0} }, { {1,0,-1}, {0,1,0} },
        { { 1,0, 1}, {0,1,0} }, { {-1,0, 1}, {0,1,0} },
    };
    uint32_t idx[6] = { 0,1,2,  0,2,3 };

    JceHlodMeshInput in[2];
    memset(in, 0, sizeof in);
    for (int s = 0; s < 2; ++s) {
        in[s].positions       = (const char *)verts + 0;          /* pos at offset 0 */
        in[s].position_stride = (uint32_t)sizeof(V);
        in[s].normals         = (const char *)verts + sizeof(float) * 3; /* nrm offset 12 */
        in[s].normal_stride   = (uint32_t)sizeof(V);
        in[s].vertex_count    = 4;
        in[s].indices         = idx;
        in[s].index_count     = 6;
    }
    mat_translate(in[0].world,  10.0f, 0.0f, 0.0f);   /* quad at x=+10 → x in [9,11]  */
    mat_translate(in[1].world, -10.0f, 0.0f, 0.0f);   /* quad at x=-10 → x in [-11,-9]*/

    const char *path = "tmp_hlod.glb";
    float col[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    JceHlodBakeStats st;
    /* ratio 1.0 → keep all (a 4-tri mesh is below any useful reduction target). */
    TEST_ASSERT_TRUE(jce_hlod_bake_proxy(in, 2, 1.0f, col, path, &st));
    TEST_ASSERT_EQUAL_UINT(8, st.in_vertices);
    TEST_ASSERT_EQUAL_UINT(4, st.in_triangles);
    TEST_ASSERT_TRUE(st.out_triangles >= 1);

    /* Read back; the POSITION accessor min/max must span both translated quads. */
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    TEST_ASSERT_EQUAL_INT(sz, (long)fread(buf, 1, (size_t)sz, f));
    fclose(f);

    uint32_t magic, jlen;
    memcpy(&magic, buf + 0, 4);
    memcpy(&jlen,  buf + 12, 4);
    TEST_ASSERT_EQUAL_HEX32(0x46546C67u, magic);
    JceJson *root = jce_json_parse((const char *)(buf + 20), jlen);
    TEST_ASSERT_NOT_NULL(root);
    JceJson *acc0 = jce_json_array_at(jce_json_get(root, "accessors"), 0);
    TEST_ASSERT_NOT_NULL(acc0);
    JceJson *mn = jce_json_get(acc0, "min");
    JceJson *mx = jce_json_get(acc0, "max");
    TEST_ASSERT_TRUE(jce_json_is_array(mn) && jce_json_is_array(mx));
    float minx = (float)jce_json_number_value(jce_json_array_at(mn, 0), 0.0);
    float maxx = (float)jce_json_number_value(jce_json_array_at(mx, 0), 0.0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -11.0f, minx);   /* world-space merge proven */
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  11.0f, maxx);
    jce_json_free(root);

    free(buf);
    remove(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hlod_bake_merge_worldspace);
    return UNITY_END();
}
