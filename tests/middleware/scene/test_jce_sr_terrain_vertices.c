/* Production typed terrain conversion + real public procedural chunk owner.
 * Counted allocator controls prove CPU ownership/stride, never GPU output. */
#include "unity.h"
#include "middleware/scene/jce_sr_terrain_vertices.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static struct {
    unsigned calls, allocations, releases;
    size_t bytes;
    unsigned char *live;
    bool fail;
} f;
static void *allocate_vertices(size_t bytes, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user);
    ++f.calls;
    if (f.fail) return NULL;
    TEST_ASSERT_NULL(f.live);
    f.live = (unsigned char *)malloc(bytes + 16u);
    TEST_ASSERT_NOT_NULL(f.live);
    f.bytes = bytes;
    memset(f.live, 0xa5, bytes + 16u);
    ++f.allocations;
    return f.live;
}
static void release_vertices(void *ptr, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user);
    TEST_ASSERT_EQUAL_PTR(f.live, ptr);
    for (size_t i = f.bytes; i < f.bytes + 16u; ++i)
        TEST_ASSERT_EQUAL_HEX8(0xa5u, f.live[i]);
    free(ptr);
    f.live = NULL;
    ++f.releases;
}
static const SrTerrainVertexAllocator allocator = {
    allocate_vertices, release_vertices, &f
};
void setUp(void) { memset(&f, 0, sizeof f); }
void tearDown(void) {
    TEST_ASSERT_NULL(f.live);
    TEST_ASSERT_EQUAL_UINT(f.allocations, f.releases);
}
static void check_vertex(const JceTerrainVertex *src, const JceMeshVertex *dst)
{
    TEST_ASSERT_EQUAL_FLOAT(src->px, dst->pos[0]);
    TEST_ASSERT_EQUAL_FLOAT(src->py, dst->pos[1]);
    TEST_ASSERT_EQUAL_FLOAT(src->pz, dst->pos[2]);
    TEST_ASSERT_EQUAL_FLOAT(src->nx, dst->normal[0]);
    TEST_ASSERT_EQUAL_FLOAT(src->ny, dst->normal[1]);
    TEST_ASSERT_EQUAL_FLOAT(src->nz, dst->normal[2]);
    TEST_ASSERT_EQUAL_FLOAT(src->u, dst->uv[0]);
    TEST_ASSERT_EQUAL_FLOAT(src->v, dst->uv[1]);
    for (int j = 0; j < 4; ++j) TEST_ASSERT_EQUAL_FLOAT(0.0f, dst->tangent[j]);
}
static void test_compact_vertex_and_skirt_convert_without_reinterpreting_stride(void)
{
    TEST_ASSERT_EQUAL_UINT(8u * sizeof(float), sizeof(JceTerrainVertex));
    TEST_ASSERT_EQUAL_UINT(12u * sizeof(float), sizeof(JceMeshVertex));
    const JceTerrainVertex src[] = {
        {1, 2, 3, 0, 1, 0, .25f, .75f},
        {4, -.01f, 6, 1, 0, 0, .5f, 1}
    };
    unsigned char saved[sizeof src]; memcpy(saved, src, sizeof src);
    SrTerrainVertexOwner owner = {0};
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_OK,
        sr_terrain_vertices_create(&owner, src, 2u, &allocator));
    TEST_ASSERT_EQUAL_UINT(2u * sizeof(JceMeshVertex), f.bytes);
    check_vertex(&src[0], &owner.vertices[0]);
    check_vertex(&src[1], &owner.vertices[1]);
    TEST_ASSERT_EQUAL_MEMORY(saved, src, sizeof src);
    sr_terrain_vertices_destroy(&owner);
    TEST_ASSERT_NULL(owner.vertices);
    sr_terrain_vertices_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(1u, f.releases);
}
static void test_real_procedural_chunk_keeps_positions_heights_and_tangent_contract(void)
{
    JceTerrain *terrain = jce_terrain_create_procedural(17, 17, 20, 20,
        .05f, 16, 4, 4040u, .02f);
    TEST_ASSERT_NOT_NULL(terrain);
    int nv = 0, ni = 0;
    jce_terrain_chunk_mesh_size(terrain, 0, 0, 0, &nv, &ni);
    TEST_ASSERT_EQUAL_INT(17 * 17, nv);
    TEST_ASSERT_TRUE(ni > 0);
    JceTerrainVertex *src = (JceTerrainVertex *)malloc((size_t)nv * sizeof *src);
    uint32_t *indices = (uint32_t *)malloc((size_t)ni * sizeof *indices);
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(indices);
    int wrote_v = 0, wrote_i = 0;
    jce_terrain_chunk_build_mesh(terrain, 0, 0, 0, src, nv, indices, ni,
        &wrote_v, &wrote_i);
    TEST_ASSERT_EQUAL_INT(nv, wrote_v); TEST_ASSERT_EQUAL_INT(ni, wrote_i);
    SrTerrainVertexOwner owner = {0};
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_OK,
        sr_terrain_vertices_create(&owner, src, (uint32_t)nv, &allocator));
    for (int i = 0; i < nv; ++i) {
        check_vertex(&src[i], &owner.vertices[i]);
        TEST_ASSERT_TRUE(isfinite(owner.vertices[i].pos[1]));
        TEST_ASSERT_TRUE(owner.vertices[i].pos[1] >= 0.0f &&
                         owner.vertices[i].pos[1] <= .05f);
    }
    for (int i = 0; i < ni; ++i) TEST_ASSERT_TRUE(indices[i] < (uint32_t)nv);
    sr_terrain_vertices_destroy(&owner);
    free(src); free(indices); jce_terrain_free(terrain);
}
static void test_oom_does_not_publish_and_retry_releases_exactly_once(void)
{
    const JceTerrainVertex src = {0};
    SrTerrainVertexOwner owner = {0}; f.fail = true;
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_OOM,
        sr_terrain_vertices_create(&owner, &src, 1u, &allocator));
    TEST_ASSERT_NULL(owner.vertices);
    sr_terrain_vertices_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(0u, f.releases);
    f.fail = false;
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_OK,
        sr_terrain_vertices_create(&owner, &src, 1u, &allocator));
    sr_terrain_vertices_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(2u, f.calls);
    TEST_ASSERT_EQUAL_UINT(1u, f.allocations);
    TEST_ASSERT_EQUAL_UINT(1u, f.releases);
}
static void test_invalid_upload_length_rejects_before_allocation_or_source_read(void)
{
    const JceTerrainVertex src = {0}; SrTerrainVertexOwner owner = {0};
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_INVALID,
        sr_terrain_vertices_create(&owner, &src, UINT32_MAX, &allocator));
    TEST_ASSERT_EQUAL_UINT(0u, f.calls); TEST_ASSERT_NULL(owner.vertices);
}
static void test_invalid_source_and_incomplete_allocator_do_not_allocate(void)
{
    const JceTerrainVertex src = {0}; SrTerrainVertexOwner owner = {0};
    SrTerrainVertexAllocator incomplete = allocator; incomplete.release = NULL;
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_INVALID,
        sr_terrain_vertices_create(&owner, NULL, 1u, &allocator));
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_INVALID,
        sr_terrain_vertices_create(&owner, &src, 0u, &allocator));
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_INVALID,
        sr_terrain_vertices_create(&owner, &src, 1u, &incomplete));
    TEST_ASSERT_EQUAL_UINT(0u, f.calls);
}
static void test_live_owner_cannot_be_overwritten_or_released_by_other_provider(void)
{
    const JceTerrainVertex src = {0}; SrTerrainVertexOwner owner = {0};
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_OK,
        sr_terrain_vertices_create(&owner, &src, 1u, &allocator));
    JceMeshVertex *first = owner.vertices;
    SrTerrainVertexAllocator other = allocator; other.user = NULL;
    TEST_ASSERT_EQUAL_INT(SR_TERRAIN_VERTICES_INVALID,
        sr_terrain_vertices_create(&owner, &src, 1u, &other));
    TEST_ASSERT_EQUAL_PTR(first, owner.vertices);
    TEST_ASSERT_EQUAL_UINT(1u, f.calls);
    sr_terrain_vertices_destroy(&owner);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_compact_vertex_and_skirt_convert_without_reinterpreting_stride);
    RUN_TEST(test_real_procedural_chunk_keeps_positions_heights_and_tangent_contract);
    RUN_TEST(test_oom_does_not_publish_and_retry_releases_exactly_once);
    RUN_TEST(test_invalid_upload_length_rejects_before_allocation_or_source_read);
    RUN_TEST(test_invalid_source_and_incomplete_allocator_do_not_allocate);
    RUN_TEST(test_live_owner_cannot_be_overwritten_or_released_by_other_provider);
    return UNITY_END();
}
