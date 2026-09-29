/*
 * test_jce_model_importer_firewall.c
 *
 * jce_model_importer.cpp is C++ behind a flat C ABI, and it is the TU that
 * feeds UNTRUSTED bytes to Assimp: models arrive from a PAK, from disk, or
 * from whatever a game ships.  Its bodies allocate std::vector / std::string
 * per mesh while parsing, so std::bad_alloc on a malformed or hostile file is
 * an expected failure, not an exotic one — and an exception crossing back
 * into C is undefined behaviour (in practice std::terminate, with a stack the
 * C caller cannot interpret).
 *
 * These tests pin the contract the firewall provides: a bad input makes the
 * entry point RETURN FALSE.  It never unwinds past the C boundary.
 *
 * The firewall's catch path was verified during development by injecting a
 * `throw std::runtime_error(...)` at the top of inspect_file's implementation
 * and confirming these same assertions still held — the process returned
 * false and logged, rather than terminating.
 */

#include "unity.h"

#include <jce/resource/jce_model_importer.h>

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_inspect_file_rejects_missing_path(void)
{
    JceModelInspectResult r;
    memset(&r, 0xAB, sizeof r);   /* poison: a false return must not be read */

    TEST_ASSERT_FALSE(jce_model_importer_inspect_file(
        "does/not/exist/nothing.fbx", false, &r));
    TEST_ASSERT_FALSE(jce_model_importer_inspect_file(NULL, false, &r));
}

static void test_inspect_file_rejects_null_out(void)
{
    /* Must not dereference NULL, and must not throw its way out either. */
    TEST_ASSERT_FALSE(jce_model_importer_inspect_file("whatever.obj",
                                                      false, NULL));
}

/* Garbage bytes with a plausible extension hint: Assimp is handed content it
 * cannot parse.  Whether it fails by returning null or by throwing is its
 * business — the contract here is that WE return false either way. */
static void test_inspect_memory_rejects_garbage(void)
{
    static const unsigned char junk[] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03,
        0xFF, 0xFE, 0xFD, 0xFC, 0x7F, 0x80, 0x81, 0x82,
    };
    JceModelInspectResult r;
    memset(&r, 0xAB, sizeof r);

    TEST_ASSERT_FALSE(jce_model_importer_inspect_memory(
        junk, sizeof junk, "fbx", false, &r));
    TEST_ASSERT_FALSE(jce_model_importer_inspect_memory(
        junk, sizeof junk, "gltf", false, &r));
    /* Zero length and NULL data are the degenerate cases of the same path. */
    TEST_ASSERT_FALSE(jce_model_importer_inspect_memory(
        junk, 0u, "obj", false, &r));
    TEST_ASSERT_FALSE(jce_model_importer_inspect_memory(
        NULL, sizeof junk, "obj", false, &r));
}

/* Truncated-but-well-signed input is the more interesting shape: the header
 * looks real enough for a parser to start allocating, then the data ends. */
static void test_inspect_memory_rejects_truncated_gltf(void)
{
    /* GLB magic + version + a total-length field that lies about the size. */
    static const unsigned char truncated_glb[] = {
        'g', 'l', 'T', 'F',            /* magic   */
        0x02, 0x00, 0x00, 0x00,        /* version */
        0xFF, 0xFF, 0xFF, 0x7F,        /* length: claims ~2 GiB, we have 20 B */
        0x10, 0x00, 0x00, 0x00,        /* chunk length, also a lie            */
        'J',  'S',  'O',  'N',
    };
    JceModelInspectResult r;
    memset(&r, 0xAB, sizeof r);

    TEST_ASSERT_FALSE(jce_model_importer_inspect_memory(
        truncated_glb, sizeof truncated_glb, "glb", false, &r));
}

static void test_load_parts_rejects_bad_input(void)
{
    JceModelParts parts;
    memset(&parts, 0xAB, sizeof parts);

    TEST_ASSERT_FALSE(jce_model_importer_load_parts_memory(NULL, 16u,
                                                           "obj", &parts));
    TEST_ASSERT_FALSE(jce_model_importer_load_parts_file(
        "does/not/exist/nothing.obj", &parts));
    TEST_ASSERT_FALSE(jce_model_importer_load_parts_file(NULL, &parts));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_inspect_file_rejects_missing_path);
    RUN_TEST(test_inspect_file_rejects_null_out);
    RUN_TEST(test_inspect_memory_rejects_garbage);
    RUN_TEST(test_inspect_memory_rejects_truncated_gltf);
    RUN_TEST(test_load_parts_rejects_bad_input);
    return UNITY_END();
}
