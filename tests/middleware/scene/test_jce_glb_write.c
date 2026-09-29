/*
 * test_jce_glb_write.c
 *
 * Verifies the minimal .glb writer (jce_glb_write, used by the A2 HLOD bake)
 * produces a well-formed binary glTF: correct header/chunk framing, valid JSON
 * (parsed by the engine JSON lib), accessor counts/componentTypes, and a binary
 * chunk whose POSITION/NORMAL/index data round-trips byte-for-byte.  Structural
 * (no bgfx) — the layout mirrors the proven tools/worldgen/gen_hlod.py writer.
 */

#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_glb_write.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_glb_write_quad(void)
{
    /* A unit quad: 4 vertices, 2 triangles (6 indices). */
    float pos[12] = { -1,0,-1,   1,0,-1,   1,0,1,   -1,0,1 };
    float nrm[12] = {  0,1,0,    0,1,0,    0,1,0,    0,1,0  };
    uint32_t idx[6] = { 0,1,2,  0,2,3 };
    float color[4] = { 0.5f, 0.6f, 0.7f, 1.0f };

    const char *path = "tmp_glb_quad.glb";
    TEST_ASSERT_TRUE(jce_glb_write_mesh(path, pos, nrm, 4, idx, 6, color));

    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    TEST_ASSERT_TRUE(sz > 28);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    TEST_ASSERT_EQUAL_INT(sz, (long)fread(buf, 1, (size_t)sz, f));
    fclose(f);

    /* Header. */
    uint32_t magic, ver, total;
    memcpy(&magic, buf + 0, 4); memcpy(&ver, buf + 4, 4); memcpy(&total, buf + 8, 4);
    TEST_ASSERT_EQUAL_HEX32(0x46546C67u, magic);   /* "glTF" */
    TEST_ASSERT_EQUAL_UINT32(2u, ver);
    TEST_ASSERT_EQUAL_INT((long)total, sz);

    /* JSON chunk — must be valid JSON the engine lib parses. */
    uint32_t jlen, jtype;
    memcpy(&jlen, buf + 12, 4); memcpy(&jtype, buf + 16, 4);
    TEST_ASSERT_EQUAL_HEX32(0x4E4F534Au, jtype);   /* "JSON" */
    JceJson *root = jce_json_parse((const char *)(buf + 20), jlen);
    TEST_ASSERT_NOT_NULL(root);
    JceJson *acc = jce_json_get(root, "accessors");
    TEST_ASSERT_TRUE(jce_json_is_array(acc));
    TEST_ASSERT_EQUAL_INT(3, jce_json_array_size(acc));
    TEST_ASSERT_EQUAL_INT(4, (int)jce_json_get_number(jce_json_array_at(acc, 0), "count", -1.0));
    TEST_ASSERT_EQUAL_INT(6, (int)jce_json_get_number(jce_json_array_at(acc, 2), "count", -1.0));
    TEST_ASSERT_EQUAL_INT(5126, (int)jce_json_get_number(jce_json_array_at(acc, 0), "componentType", -1.0)); /* FLOAT  */
    TEST_ASSERT_EQUAL_INT(5125, (int)jce_json_get_number(jce_json_array_at(acc, 2), "componentType", -1.0)); /* UINT32 */
    /* one mesh / one primitive present */
    JceJson *meshes = jce_json_get(root, "meshes");
    TEST_ASSERT_TRUE(jce_json_is_array(meshes) && jce_json_array_size(meshes) == 1);
    jce_json_free(root);

    /* BIN chunk — POSITION/NORMAL/index data round-trips byte-for-byte. */
    uint32_t blen, btype;
    memcpy(&blen, buf + 20 + jlen, 4); memcpy(&btype, buf + 20 + jlen + 4, 4);
    TEST_ASSERT_EQUAL_HEX32(0x004E4942u, btype);   /* "BIN\0" */
    const uint8_t *bin = buf + 20 + jlen + 8;

    float rx, ry, rz;
    memcpy(&rx, bin + 0, 4); memcpy(&ry, bin + 4, 4); memcpy(&rz, bin + 8, 4);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.0f, rx);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.0f, ry);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.0f, rz);
    /* indices follow positions(48) + normals(48) = offset 96 */
    uint32_t i0, i5;
    memcpy(&i0, bin + 96, 4); memcpy(&i5, bin + 96 + 5 * 4, 4);
    TEST_ASSERT_EQUAL_UINT32(0u, i0);
    TEST_ASSERT_EQUAL_UINT32(3u, i5);

    free(buf);
    remove(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_glb_write_quad);
    return UNITY_END();
}
