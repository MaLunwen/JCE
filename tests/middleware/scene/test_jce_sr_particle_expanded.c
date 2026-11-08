/* Actual particle batch owner and bgfx OpenGL vertex layout, with counted
 * transient ports. No graphics device, shader execution or pixels here. */
#include "unity.h"
#include "middleware/scene/jce_sr_particle_batch.h"

#include <stdlib.h>
#include <string.h>

enum {
    VERTEX_FLOATS = 13,
    VERTEX_BYTES = 52,
    VERTEX_RESERVE = 12,
    INDEX_RESERVE = 32768,
    MAX_QUADS = 4096
};

static struct {
    bgfx_caps_t caps;
    JceInstanceUploadOwner owner;
    void *vertex_bytes;
    void *index_bytes;
    size_t vertex_size;
    size_t index_size;
    uint32_t vertex_available;
    uint32_t index_available;
    uint32_t vertex_query;
    uint32_t index_query;
    uint32_t vertex_allocated;
    uint32_t index_allocated;
    uint32_t vertex_bound;
    uint32_t index_bound;
    unsigned allocations;
    unsigned vertex_binds;
    unsigned index_binds;
    bool absent_caps;
    bool fail_allocation;
} fixture;

static const bgfx_caps_t *controlled_caps(void)
{
    return fixture.absent_caps ? NULL : &fixture.caps;
}

static bgfx_renderer_type_t controlled_backend(void)
{
    return BGFX_RENDERER_TYPE_OPENGL;
}

static JceInstanceUploadOwner *controlled_instance_owner(const JceRenderer *renderer)
{
    TEST_ASSERT_NULL(renderer);
    return &fixture.owner;
}

static uint32_t controlled_instance_epoch(void)
{
    TEST_FAIL_MESSAGE("expanded quads must not reserve instance uploads");
    return 0;
}

static bool controlled_instance_begin(JceInstanceUploadOwner *owner, uint32_t epoch,
    uint32_t requested, uint16_t stride, uint32_t budget, JceInstanceUpload *upload)
{
    (void)owner;
    (void)epoch;
    (void)requested;
    (void)stride;
    (void)budget;
    (void)upload;
    TEST_FAIL_MESSAGE("expanded quads must not reserve instance uploads");
    return false;
}

static bool controlled_instance_bind(JceInstanceUploadOwner *owner,
                                     JceInstanceUpload *upload, uint32_t actual)
{
    (void)owner;
    (void)upload;
    (void)actual;
    TEST_FAIL_MESSAGE("expanded quads must not bind instance uploads");
    return false;
}

static uint32_t controlled_vertex_available(uint32_t requested,
                                            const bgfx_vertex_layout_t *layout)
{
    TEST_ASSERT_EQUAL_UINT16(VERTEX_BYTES, layout->stride);
    TEST_ASSERT_EQUAL_UINT16(0, layout->offset[BGFX_ATTRIB_POSITION]);
    TEST_ASSERT_EQUAL_UINT16(12, layout->offset[BGFX_ATTRIB_TEXCOORD0]);
    TEST_ASSERT_EQUAL_UINT16(20, layout->offset[BGFX_ATTRIB_TEXCOORD1]);
    TEST_ASSERT_EQUAL_UINT16(36, layout->offset[BGFX_ATTRIB_COLOR0]);
    fixture.vertex_query = requested;
    return requested < fixture.vertex_available ? requested : fixture.vertex_available;
}

static uint32_t controlled_index_available(uint32_t requested, bool index32)
{
    TEST_ASSERT_FALSE(index32);
    fixture.index_query = requested;
    return requested < fixture.index_available ? requested : fixture.index_available;
}

static bool controlled_allocate(bgfx_transient_vertex_buffer_t *vertices,
    const bgfx_vertex_layout_t *layout, uint32_t vertex_count,
    bgfx_transient_index_buffer_t *indices, uint32_t index_count, bool index32)
{
    TEST_ASSERT_FALSE(index32);
    TEST_ASSERT_NULL(fixture.vertex_bytes);
    TEST_ASSERT_NULL(fixture.index_bytes);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_QUADS * 4u, vertex_count);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MAX_QUADS * 6u, index_count);
    ++fixture.allocations;
    fixture.vertex_allocated = vertex_count;
    fixture.index_allocated = index_count;
    if (fixture.fail_allocation) return false;

    fixture.vertex_size = (size_t)vertex_count * layout->stride;
    fixture.index_size = (size_t)index_count * sizeof(uint16_t);
    fixture.vertex_bytes = malloc(fixture.vertex_size + 1u);
    fixture.index_bytes = malloc(fixture.index_size + 1u);
    TEST_ASSERT_NOT_NULL(fixture.vertex_bytes);
    TEST_ASSERT_NOT_NULL(fixture.index_bytes);
    memset(fixture.vertex_bytes, 0xcd, fixture.vertex_size + 1u);
    memset(fixture.index_bytes, 0xcd, fixture.index_size + 1u);
    *vertices = (bgfx_transient_vertex_buffer_t) {
        .data = fixture.vertex_bytes,
        .size = (uint32_t)fixture.vertex_size,
        .startVertex = 31u,
        .stride = layout->stride,
    };
    *indices = (bgfx_transient_index_buffer_t) {
        .data = fixture.index_bytes,
        .size = (uint32_t)fixture.index_size,
        .startIndex = 37u,
        .isIndex16 = true,
    };
    return true;
}

static void controlled_vertex_bind(uint8_t stream, const bgfx_transient_vertex_buffer_t *buffer,
                                    uint32_t first, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT8(0, stream);
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(31, buffer->startVertex);
    TEST_ASSERT_EQUAL_PTR(fixture.vertex_bytes, buffer->data);
    fixture.vertex_bound = count;
    ++fixture.vertex_binds;
}

static void controlled_index_bind(const bgfx_transient_index_buffer_t *buffer,
                                   uint32_t first, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(37, buffer->startIndex);
    TEST_ASSERT_EQUAL_PTR(fixture.index_bytes, buffer->data);
    TEST_ASSERT_TRUE(buffer->isIndex16);
    fixture.index_bound = count;
    ++fixture.index_binds;
}

#define bgfx_get_caps controlled_caps
#define bgfx_get_renderer_type controlled_backend
#define bgfx_get_avail_transient_vertex_buffer controlled_vertex_available
#define bgfx_get_avail_transient_index_buffer controlled_index_available
#define bgfx_alloc_transient_buffers controlled_allocate
#define bgfx_set_transient_vertex_buffer controlled_vertex_bind
#define bgfx_set_transient_index_buffer controlled_index_bind
#define jce_renderer_instance_upload_owner controlled_instance_owner
#define jce_renderer_instance_upload_epoch controlled_instance_epoch
#define jce_instance_upload_begin controlled_instance_begin
#define jce_instance_upload_bind controlled_instance_bind
#ifndef JCE_TEST_PARTICLE_EXPANDED_OWNER
#define JCE_TEST_PARTICLE_EXPANDED_OWNER "middleware/scene/jce_sr_particle_batch.c"
#endif
#include JCE_TEST_PARTICLE_EXPANDED_OWNER
#undef jce_instance_upload_bind
#undef jce_instance_upload_begin
#undef jce_renderer_instance_upload_epoch
#undef jce_renderer_instance_upload_owner
#undef bgfx_set_transient_index_buffer
#undef bgfx_set_transient_vertex_buffer
#undef bgfx_alloc_transient_buffers
#undef bgfx_get_avail_transient_index_buffer
#undef bgfx_get_avail_transient_vertex_buffer
#undef bgfx_get_renderer_type
#undef bgfx_get_caps

void setUp(void)
{
    memset(&fixture, 0, sizeof fixture);
    fixture.caps.limits.maxTransientVbSize = 6u * 1024u * 1024u;
    fixture.caps.limits.maxTransientIbSize = 2u * 1024u * 1024u;
    fixture.owner.transient_reserve_bytes = VERTEX_BYTES * VERTEX_RESERVE;
    fixture.vertex_available = UINT32_MAX;
    fixture.index_available = UINT32_MAX;
}

void tearDown(void)
{
    free(fixture.vertex_bytes);
    free(fixture.index_bytes);
}

static void test_zero_request_caps_and_pool_headroom_skip_allocation(void)
{
    SrParticleExpandedUpload upload = {0};
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 0, &upload));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.vertex_query);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.index_query);

    fixture.absent_caps = true;
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 1, &upload));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.vertex_query);
    fixture.absent_caps = false;

    fixture.vertex_available = VERTEX_RESERVE;
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 1, &upload));
    fixture.vertex_available = UINT32_MAX;
    fixture.index_available = INDEX_RESERVE + 5u;
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 1, &upload));
    TEST_ASSERT_EQUAL_UINT32(VERTEX_RESERVE + 4u, fixture.vertex_query);
    TEST_ASSERT_EQUAL_UINT32(INDEX_RESERVE + 6u, fixture.index_query);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.allocations);
    TEST_ASSERT_FALSE(sr_particle_expanded_bind(&upload));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.vertex_binds);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.index_binds);
}

static void test_vertex_and_index_admission_are_independently_bounded(void)
{
    SrParticleExpandedUpload upload = {0};
    fixture.fail_allocation = true;
    fixture.vertex_available = VERTEX_RESERVE + 11u;
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 5, &upload));
    TEST_ASSERT_EQUAL_UINT32(8, fixture.vertex_allocated);
    TEST_ASSERT_EQUAL_UINT32(12, fixture.index_allocated);

    fixture.vertex_available = UINT32_MAX;
    fixture.index_available = INDEX_RESERVE + 13u;
    TEST_ASSERT_FALSE(sr_particle_expanded_begin(NULL, 5, &upload));
    TEST_ASSERT_EQUAL_UINT32(8, fixture.vertex_allocated);
    TEST_ASSERT_EQUAL_UINT32(12, fixture.index_allocated);
    TEST_ASSERT_EQUAL_UINT32(2, fixture.allocations);
    TEST_ASSERT_NULL(fixture.vertex_bytes);
    TEST_ASSERT_NULL(fixture.index_bytes);
}

static void test_small_pools_cap_reserves_at_half_the_actual_capacity(void)
{
    SrParticleExpandedUpload upload;
    fixture.caps.limits.maxTransientVbSize = VERTEX_BYTES * 10u;
    fixture.caps.limits.maxTransientIbSize = 40u;
    fixture.vertex_available = 10u;
    fixture.index_available = 20u;

    TEST_ASSERT_TRUE(sr_particle_expanded_begin(NULL, 1, &upload));
    TEST_ASSERT_EQUAL_UINT32(9, fixture.vertex_query);
    TEST_ASSERT_EQUAL_UINT32(16, fixture.index_query);
    TEST_ASSERT_EQUAL_UINT32(4, fixture.vertex_allocated);
    TEST_ASSERT_EQUAL_UINT32(6, fixture.index_allocated);
}

static void check_vertex(uint32_t index, const float expected[VERTEX_FLOATS])
{
    const unsigned char *bytes = fixture.vertex_bytes;
    float values[VERTEX_FLOATS];
    memcpy(values, bytes + index * VERTEX_BYTES, sizeof values);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, values, VERTEX_FLOATS);
}

static void test_consecutive_quads_preserve_vertices_and_bind_only_filled_range(void)
{
    SrParticleExpandedUpload upload;
    TEST_ASSERT_TRUE(sr_particle_expanded_begin(NULL, 3, &upload));
    TEST_ASSERT_EQUAL_UINT32(3, upload.capacity);
    TEST_ASSERT_FALSE(sr_particle_expanded_bind(&upload));

    const JceParticleView first = {
        .position = {10.0f, -20.0f, 30.0f},
        .color = {2.0f, 0.25f, 0.5f, 0.75f},
        .size = 0.75f,
        .velocity = {2.0f, -4.0f, 8.0f},
        .stretch = 0.25f,
    };
    const JceParticleView second = {
        .position = {50.0f, 60.0f, -70.0f},
        .color = {0.125f, 0.25f, 0.5f, 1.0f},
        .size = -1.0f,
        .velocity = {11.0f, 13.0f, 17.0f},
        .stretch = 0.0f,
    };
    sr_particle_expanded_visit(&first, &upload);
    sr_particle_expanded_visit(&second, &upload);

    const float expected_corners[4][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
    };
    for (uint32_t i = 0; i < 4; i++) {
        const float u = expected_corners[i][0];
        const float v = expected_corners[i][1];
        const float a[VERTEX_FLOATS] = {
            10.0f, -20.0f, 30.0f, u, v, 0.5f, -1.0f, 2.0f, 0.75f,
            2.0f, 0.25f, 0.5f, 0.75f,
        };
        const float b[VERTEX_FLOATS] = {
            50.0f, 60.0f, -70.0f, u, v, 0.0f, 0.0f, 0.0f, 0.0001f,
            0.125f, 0.25f, 0.5f, 1.0f,
        };
        check_vertex(i, a);
        check_vertex(4u + i, b);
    }
    const uint16_t expected_indices[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected_indices, fixture.index_bytes, 12);
    TEST_ASSERT_EQUAL_HEX8(0xcd, ((unsigned char *)fixture.vertex_bytes)[8u * VERTEX_BYTES]);
    TEST_ASSERT_EQUAL_HEX8(0xcd, ((unsigned char *)fixture.index_bytes)[12u * sizeof(uint16_t)]);

    TEST_ASSERT_TRUE(sr_particle_expanded_bind(&upload));
    TEST_ASSERT_EQUAL_UINT32(8, fixture.vertex_bound);
    TEST_ASSERT_EQUAL_UINT32(12, fixture.index_bound);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.vertex_binds);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.index_binds);
}

static void test_maximum_slice_keeps_indices_local_and_ignores_overflow_visit(void)
{
    SrParticleExpandedUpload upload;
    TEST_ASSERT_TRUE(sr_particle_expanded_begin(NULL, UINT32_MAX, &upload));
    TEST_ASSERT_EQUAL_UINT32(MAX_QUADS, upload.capacity);
    TEST_ASSERT_EQUAL_UINT32(MAX_QUADS * 4u + VERTEX_RESERVE, fixture.vertex_query);
    TEST_ASSERT_EQUAL_UINT32(MAX_QUADS * 6u + INDEX_RESERVE, fixture.index_query);

    const JceParticleView particle = {.size = 1.0f};
    for (uint32_t i = 0; i < MAX_QUADS + 1u; i++)
        sr_particle_expanded_visit(&particle, &upload);
    TEST_ASSERT_EQUAL_UINT32(MAX_QUADS, upload.count);

    const uint16_t expected_last[] = {16380, 16381, 16382, 16380, 16382, 16383};
    const uint16_t *indices = fixture.index_bytes;
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected_last, indices + (MAX_QUADS - 1u) * 6u, 6);
    TEST_ASSERT_EQUAL_HEX8(0xcd, ((unsigned char *)fixture.vertex_bytes)[fixture.vertex_size]);
    TEST_ASSERT_EQUAL_HEX8(0xcd, ((unsigned char *)fixture.index_bytes)[fixture.index_size]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_request_caps_and_pool_headroom_skip_allocation);
    RUN_TEST(test_vertex_and_index_admission_are_independently_bounded);
    RUN_TEST(test_small_pools_cap_reserves_at_half_the_actual_capacity);
    RUN_TEST(test_consecutive_quads_preserve_vertices_and_bind_only_filled_range);
    RUN_TEST(test_maximum_slice_keeps_indices_local_and_ignores_overflow_visit);
    return UNITY_END();
}
