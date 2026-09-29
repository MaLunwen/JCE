/* test_jce_mesh_lod.c
 *
 * Headless unit tests for the meshoptimizer-backed auto-LOD core
 * (engine/src/resource/jce_mesh_lod_cook.{h,c}).  Pure CPU: a procedural
 * subdivided grid is simplified in-memory — no GPU, no Assimp, no files.
 *
 * The INTERNAL resource header is reached by relative path; the CMake target
 * adds engine/src/resource to the include path and links jce_resource (which
 * carries jce_mesh_lod_cook + meshoptimizer).
 *
 * Coverage:
 *   - jce_mesh_simplify reduces a dense grid and emits valid triangles.
 *   - jce_mesh_generate_lod_chain produces a monotonic, valid LOD chain.
 *   - degenerate inputs (icount < 3, vcount == 0, NULL) are crash-safe.
 */

#include "jce_mesh_lod_cook.h"
#include "os/core/jce_memory.h"   /* JCE_FREE — match the converter's allocator */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

/* Cook entry point (exported by jce_resource; declared inline in
 * jce_bundle_pack.c).  Forward-declared here for the GLB round-trip test that
 * proves the auto-LOD chain is PERSISTED into the cooked .glb (P1 #6). */
extern int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                     const char *ext_hint,
                                     uint8_t **out_buf, size_t *out_size);

void setUp(void)    {}
void tearDown(void) {}

/* ---- procedural mesh: an N x N subdivided unit grid in the XZ plane ----- *
 * (N+1)^2 vertices, N*N*2 triangles.  N=32 => 1089 verts, 2048 tris. */
#define GRID_N 32

static float       *g_pos    = NULL;   /* float[3] stream */
static unsigned int *g_idx   = NULL;
static size_t        g_vcount = 0;
static size_t        g_icount = 0;

static void build_grid(void)
{
    const int n = GRID_N;
    g_vcount = (size_t)(n + 1) * (size_t)(n + 1);
    g_icount = (size_t)n * (size_t)n * 6u;

    g_pos = (float *)malloc(g_vcount * 3 * sizeof(float));
    g_idx = (unsigned int *)malloc(g_icount * sizeof(unsigned int));
    TEST_ASSERT_NOT_NULL(g_pos);
    TEST_ASSERT_NOT_NULL(g_idx);

    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            size_t v = (size_t)z * (n + 1) + x;
            g_pos[v * 3 + 0] = (float)x / (float)n;
            g_pos[v * 3 + 1] = 0.0f;
            g_pos[v * 3 + 2] = (float)z / (float)n;
        }
    }

    size_t k = 0;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            unsigned int a = (unsigned int)((size_t)z * (n + 1) + x);
            unsigned int b = (unsigned int)((size_t)z * (n + 1) + x + 1);
            unsigned int c = (unsigned int)((size_t)(z + 1) * (n + 1) + x);
            unsigned int d = (unsigned int)((size_t)(z + 1) * (n + 1) + x + 1);
            g_idx[k++] = a; g_idx[k++] = c; g_idx[k++] = b;
            g_idx[k++] = b; g_idx[k++] = c; g_idx[k++] = d;
        }
    }
    TEST_ASSERT_EQUAL_UINT(g_icount, (unsigned)k);
}

static void free_grid(void)
{
    free(g_pos); g_pos = NULL;
    free(g_idx); g_idx = NULL;
    g_vcount = g_icount = 0;
}

static void assert_indices_valid(const unsigned int *idx, size_t n, size_t vcount)
{
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)(n % 3u));  /* whole triangles */
    for (size_t i = 0; i < n; ++i)
        TEST_ASSERT_TRUE(idx[i] < vcount);           /* in-range */
}

/* ----------------------------------------------------------------------- */

static void test_simplify_reduces_and_is_valid(void)
{
    build_grid();

    unsigned int *out = (unsigned int *)malloc(g_icount * sizeof(unsigned int));
    TEST_ASSERT_NOT_NULL(out);

    size_t n = jce_mesh_simplify(g_pos, g_vcount, 3 * sizeof(float),
                                 g_idx, g_icount, 0.5f, 0.01f, out);

    /* Never exceeds the input. */
    TEST_ASSERT_TRUE(n <= g_icount);
    /* Actually reduced (allow meshopt topology slack, but it MUST drop). */
    TEST_ASSERT_TRUE((double)n < 0.95 * (double)g_icount);
    /* Roughly hit the 50% target (allow generous slack for slop/topology). */
    TEST_ASSERT_TRUE((double)n <= 0.60 * (double)g_icount);
    /* Valid triangle list referencing only existing vertices. */
    assert_indices_valid(out, n, g_vcount);

    free(out);
    free_grid();
}

static void test_lod_chain_monotonic_and_valid(void)
{
    build_grid();

    const float ratios[3] = { 0.5f, 0.25f, 0.1f };
    unsigned int *lvl_idx[3]   = { NULL, NULL, NULL };
    size_t        lvl_count[3] = { 0, 0, 0 };

    size_t levels = jce_mesh_generate_lod_chain(
        g_pos, g_vcount, 3 * sizeof(float),
        g_idx, g_icount, ratios, 3,
        lvl_idx, lvl_count);

    TEST_ASSERT_EQUAL_UINT(3u, (unsigned)levels);

    size_t prev = g_icount;
    for (size_t l = 0; l < levels; ++l) {
        TEST_ASSERT_NOT_NULL(lvl_idx[l]);
        TEST_ASSERT_TRUE(lvl_count[l] <= prev);          /* non-increasing */
        TEST_ASSERT_TRUE(lvl_count[l] >= 3);             /* never empty */
        assert_indices_valid(lvl_idx[l], lvl_count[l], g_vcount);
        prev = lvl_count[l];
    }

    /* Caller owns each level — free via the module's matching allocator. */
    jce_mesh_lod_chain_free(lvl_idx, levels);

    free_grid();
}

static void test_degenerate_index_count_is_safe(void)
{
    /* Two indices: below one triangle. */
    float pos[6]        = { 0,0,0, 1,0,0 };
    unsigned int idx[2] = { 0, 1 };
    unsigned int out[2] = { 99, 99 };

    size_t n = jce_mesh_simplify(pos, 2, 3 * sizeof(float),
                                 idx, 2, 0.5f, 0.01f, out);
    /* index_count < 3 -> passthrough copy (no crash, <= input). */
    TEST_ASSERT_TRUE(n <= 2);
    if (n == 2) {
        TEST_ASSERT_EQUAL_UINT(0u, out[0]);
        TEST_ASSERT_EQUAL_UINT(1u, out[1]);
    }
}

static void test_zero_vertices_is_safe(void)
{
    unsigned int idx[3] = { 0, 1, 2 };
    unsigned int out[3] = { 0 };
    /* vcount == 0 -> passthrough (no deref of positions). */
    size_t n = jce_mesh_simplify(NULL, 0, 3 * sizeof(float),
                                 idx, 3, 0.5f, 0.01f, out);
    TEST_ASSERT_TRUE(n <= 3);
}

static void test_null_outputs_are_safe(void)
{
    float pos[9]        = { 0,0,0, 1,0,0, 0,0,1 };
    unsigned int idx[3] = { 0, 1, 2 };

    /* NULL out buffer -> 0, no crash. */
    TEST_ASSERT_EQUAL_UINT(0u,
        (unsigned)jce_mesh_simplify(pos, 3, 3 * sizeof(float),
                                    idx, 3, 0.5f, 0.01f, NULL));

    /* NULL chain outputs -> 0, no crash. */
    size_t counts[1] = { 0 };
    const float r[1] = { 0.5f };
    TEST_ASSERT_EQUAL_UINT(0u,
        (unsigned)jce_mesh_generate_lod_chain(pos, 3, 3 * sizeof(float),
                                              idx, 3, r, 1, NULL, counts));
}

static void test_passthrough_ratio_one(void)
{
    float pos[9]        = { 0,0,0, 1,0,0, 0,0,1 };
    unsigned int idx[3] = { 0, 1, 2 };
    unsigned int out[3] = { 0 };
    /* ratio == 1.0 keeps everything (passthrough). */
    size_t n = jce_mesh_simplify(pos, 3, 3 * sizeof(float),
                                 idx, 3, 1.0f, 0.01f, out);
    TEST_ASSERT_EQUAL_UINT(3u, (unsigned)n);
    assert_indices_valid(out, n, 3);
}

/* ---- COOK PERSISTENCE: auto-LOD chain is written INTO the .glb (P1 #6) ----
 *
 * Synthesize an OBJ subdivided grid, run it through the bundle mesh converter
 * (which now persists the cook-computed LOD chain as extra index accessors +
 * a JCE_lod primitive extension), then scan the produced GLB's JSON chunk to
 * assert: (1) extensionsUsed declares JCE_lod, (2) at least one primitive
 * carries a "JCE_lod":{"indices":[...]} extension.  This proves the chain is
 * no longer discarded — it ships inside the cooked asset.  (The GPU upload of
 * those accessors into JceSkinnedMesh.lod_ibh needs bgfx, so the runtime bind
 * is verified in the editor/default_main, not here.) */
static char *build_grid_obj(void)
{
    /* A 16x16 grid: enough triangles that LOD reduction is meaningful. */
    const int n = 16;
    /* Generous buffer. */
    size_t cap = 1u << 20;
    char *s = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(s);
    size_t len = 0;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            len += (size_t)snprintf(s + len, cap - len, "v %f 0 %f\n",
                                    (double)x / n, (double)z / n);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            /* OBJ is 1-based. */
            int a = z * (n + 1) + x + 1;
            int b = z * (n + 1) + x + 2;
            int c = (z + 1) * (n + 1) + x + 1;
            int d = (z + 1) * (n + 1) + x + 2;
            len += (size_t)snprintf(s + len, cap - len, "f %d %d %d\n", a, c, b);
            len += (size_t)snprintf(s + len, cap - len, "f %d %d %d\n", b, c, d);
        }
    }
    return s;
}

static void test_cook_persists_lods_into_glb(void)
{
    char *obj = build_grid_obj();
    uint8_t *glb = NULL;
    size_t   glb_sz = 0;
    int ok = jce_bundle_convert_to_glb((const uint8_t *)obj, strlen(obj),
                                       "obj", &glb, &glb_sz);
    free(obj);
    TEST_ASSERT_EQUAL_INT(1, ok);
    TEST_ASSERT_NOT_NULL(glb);
    TEST_ASSERT_TRUE(glb_sz > 12);

    /* GLB layout: 12-byte header, then chunk0 = JSON (len @ +12, tag @ +16,
     * data @ +20).  Scan the JSON text for the persisted LOD markers. */
    TEST_ASSERT_TRUE(glb_sz >= 20);
    uint32_t json_len = 0;
    memcpy(&json_len, glb + 12, 4);
    TEST_ASSERT_TRUE((size_t)json_len + 20 <= glb_sz);
    const char *json = (const char *)(glb + 20);

    /* Build a NUL-terminated copy of the JSON chunk for strstr. */
    char *js = (char *)malloc((size_t)json_len + 1);
    TEST_ASSERT_NOT_NULL(js);
    memcpy(js, json, json_len);
    js[json_len] = '\0';

    /* The cook MUST have declared and emitted the optional LOD extension. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, "JCE_lod"),
        "cooked GLB is missing the JCE_lod extension (LODs were discarded)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, "extensionsUsed"),
        "cooked GLB is missing extensionsUsed:[JCE_lod]");
    /* The extension object lists at least one reduced index accessor. */
    const char *ext = strstr(js, "\"JCE_lod\":{\"indices\":[");
    TEST_ASSERT_NOT_NULL_MESSAGE(ext,
        "JCE_lod extension has no indices array");
    /* First listed accessor id must be a number (>= 0). */
    const char *arr = strchr(ext, '[');
    TEST_ASSERT_NOT_NULL(arr);
    int first_acc = -1;
    TEST_ASSERT_EQUAL_INT(1, sscanf(arr + 1, "%d", &first_acc));
    TEST_ASSERT_TRUE(first_acc >= 0);

    free(js);
    /* The converter JCE_MALLOC'd glb; free with the MATCHING engine allocator
     * (freeing with libc free corrupts the tracked/mi heap). */
    JCE_FREE(glb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simplify_reduces_and_is_valid);
    RUN_TEST(test_lod_chain_monotonic_and_valid);
    RUN_TEST(test_degenerate_index_count_is_safe);
    RUN_TEST(test_zero_vertices_is_safe);
    RUN_TEST(test_null_outputs_are_safe);
    RUN_TEST(test_passthrough_ratio_one);
    RUN_TEST(test_cook_persists_lods_into_glb);
    return UNITY_END();
}
