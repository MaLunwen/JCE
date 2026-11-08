/* Actual complete mesh constructor; counted GPU ports, no device or pixels. */
#include "unity.h"
#include "renderer/jce_mesh_create_internal.h"
#include <stdlib.h>
#include <string.h>
typedef struct Controls {
    unsigned copies, copied_releases, vertex_calls, index_calls;
    unsigned vertex_destroys, index_destroys, next_vertex, next_index;
    unsigned fail_copy, fail_vertex, fail_index;
    bool vertices[16], indices[16];
} Controls;
static Controls controls;
static JceMeshVertex vertices[3];
static const uint32_t indices[3] = {0,1,2};
static const bgfx_memory_t *copy_bytes(const void *bytes, uint32_t count, void *user)
{
    Controls *c=(Controls *)user;
    if (++c->copies==c->fail_copy) return NULL;
    bgfx_memory_t *memory=(bgfx_memory_t *)malloc(sizeof(*memory)+count);
    TEST_ASSERT_NOT_NULL(memory);
    memory->data=(uint8_t *)(memory+1); memory->size=count;
    memcpy(memory->data,bytes,count);
    return memory;
}
static void consume(const bgfx_memory_t *memory, Controls *c)
{ TEST_ASSERT_NOT_NULL(memory); ++c->copied_releases; free((void *)memory); }
static bgfx_vertex_buffer_handle_t create_vertex(const bgfx_memory_t *memory,
    const bgfx_vertex_layout_t *layout, void *user)
{
    Controls *c=(Controls *)user; TEST_ASSERT_NOT_NULL(layout); consume(memory,c);
    if (++c->vertex_calls==c->fail_vertex) return (bgfx_vertex_buffer_handle_t){UINT16_MAX};
    uint16_t handle=(uint16_t)c->next_vertex++;
    TEST_ASSERT_LESS_THAN_UINT16(16,handle); c->vertices[handle]=true;
    return (bgfx_vertex_buffer_handle_t){handle};
}
static bgfx_index_buffer_handle_t create_index(const bgfx_memory_t *memory, void *user)
{
    Controls *c=(Controls *)user; consume(memory,c);
    if (++c->index_calls==c->fail_index) return (bgfx_index_buffer_handle_t){UINT16_MAX};
    uint16_t handle=(uint16_t)c->next_index++;
    TEST_ASSERT_LESS_THAN_UINT16(16,handle); c->indices[handle]=true;
    return (bgfx_index_buffer_handle_t){handle};
}
static void destroy_vertex(bgfx_vertex_buffer_handle_t handle, void *user)
{
    Controls *c=(Controls *)user; TEST_ASSERT_LESS_THAN_UINT16(16,handle.idx);
    TEST_ASSERT_TRUE(c->vertices[handle.idx]); c->vertices[handle.idx]=false; ++c->vertex_destroys;
}
static void destroy_index(bgfx_index_buffer_handle_t handle, void *user)
{
    Controls *c=(Controls *)user; TEST_ASSERT_LESS_THAN_UINT16(16,handle.idx);
    TEST_ASSERT_TRUE(c->indices[handle.idx]); c->indices[handle.idx]=false; ++c->index_destroys;
}
static const JceMeshCreateProvider provider = {BGFX_RENDERER_TYPE_NOOP,copy_bytes,
    create_vertex,create_index,destroy_vertex,destroy_index,&controls};
static JceMesh *create(bool indexed)
{ return jce_mesh_create_with_provider(vertices,3,indexed?indices:NULL,indexed?3:0,&provider); }
void setUp(void)
{
    memset(&controls,0,sizeof controls); memset(vertices,0,sizeof vertices);
    vertices[1].pos[0]=1; vertices[2].pos[1]=1;
    for(unsigned i=0;i<3;++i) { vertices[i].normal[2]=1; vertices[i].tangent[0]=1; }
}
void tearDown(void)
{
    for(unsigned i=0;i<16;++i) { TEST_ASSERT_FALSE(controls.vertices[i]); TEST_ASSERT_FALSE(controls.indices[i]); }
    unsigned failed=controls.fail_copy&&controls.copies>=controls.fail_copy?1u:0u;
    TEST_ASSERT_EQUAL_UINT(controls.copies-failed,controls.copied_releases);
}
static void success_owns_valid_zero_handles(void)
{
    JceMesh *mesh=create(true); TEST_ASSERT_NOT_NULL(mesh);
    TEST_ASSERT_EQUAL_UINT16(0,jce_mesh_get_vbh(mesh)); TEST_ASSERT_EQUAL_UINT16(0,jce_mesh_get_ibh(mesh));
    TEST_ASSERT_EQUAL_UINT(3,controls.copies); jce_mesh_destroy(mesh);
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(2,controls.index_destroys);
}
static void nonindexed_mesh_owns_no_index_buffer(void)
{
    JceMesh *mesh=create(false); TEST_ASSERT_NOT_NULL(mesh);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,jce_mesh_get_ibh(mesh)); jce_mesh_destroy(mesh);
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(0,controls.index_calls);
}
static void vertex_staging_failure_never_calls_gpu(void)
{
    controls.fail_copy=1; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(0,controls.vertex_calls); TEST_ASSERT_EQUAL_UINT(0,controls.vertex_destroys);
}
static void vertex_creation_failure_owns_no_handle(void)
{
    controls.fail_vertex=1; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(0,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(0,controls.index_calls);
}
static void index_staging_failure_retires_vertex_once(void)
{
    controls.fail_copy=2; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(0,controls.index_destroys);
}
static void index_creation_failure_never_returns_pseudo_ready_mesh(void)
{
    controls.fail_index=1; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(0,controls.index_destroys);
}
static void wireframe_staging_failure_retires_both_owned_buffers(void)
{
    controls.fail_copy=3; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(1,controls.index_destroys);
}
static void wireframe_creation_failure_retires_both_owned_buffers(void)
{
    controls.fail_index=2; TEST_ASSERT_NULL(create(true));
    TEST_ASSERT_EQUAL_UINT(1,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(1,controls.index_destroys);
}
static void failed_creation_can_retry_without_double_retirement(void)
{
    controls.fail_index=1; TEST_ASSERT_NULL(create(true)); controls.fail_index=0;
    JceMesh *mesh=create(true); TEST_ASSERT_NOT_NULL(mesh); jce_mesh_destroy(mesh);
    TEST_ASSERT_EQUAL_UINT(2,controls.vertex_destroys); TEST_ASSERT_EQUAL_UINT(2,controls.index_destroys);
}
static void malformed_provider_rejected_before_allocation(void)
{
    JceMeshCreateProvider bad=provider; bad.destroy_index=NULL;
    TEST_ASSERT_NULL(jce_mesh_create_with_provider(vertices,3,indices,3,&bad));
    TEST_ASSERT_NULL(jce_mesh_create_with_provider(NULL,3,indices,3,&provider));
    TEST_ASSERT_EQUAL_UINT(0,controls.copies);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(success_owns_valid_zero_handles);
    RUN_TEST(nonindexed_mesh_owns_no_index_buffer);
    RUN_TEST(vertex_staging_failure_never_calls_gpu);
    RUN_TEST(vertex_creation_failure_owns_no_handle);
    RUN_TEST(index_staging_failure_retires_vertex_once);
    RUN_TEST(index_creation_failure_never_returns_pseudo_ready_mesh);
    RUN_TEST(wireframe_staging_failure_retires_both_owned_buffers);
    RUN_TEST(wireframe_creation_failure_retires_both_owned_buffers);
    RUN_TEST(failed_creation_can_retry_without_double_retirement);
    RUN_TEST(malformed_provider_rejected_before_allocation);
    return UNITY_END();
}
