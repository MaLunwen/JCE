/* Actual NOOP-device resource calls, not a mirrored counter implementation. */
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_texture.h>
#include "unity.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}

static void test_real_static_mesh_and_texture_submission(void)
{
    JceRenderer *renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    JceSubmittedPayloadStats before, after, unchanged, reset;
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &before));
    JceMeshVertex vertices[3];
    memset(vertices, 0, sizeof vertices);
    vertices[1].pos[0] = 1.0f;
    vertices[2].pos[1] = 1.0f;
    const uint32_t indices[3] = {0, 1, 2};
    JceMesh *mesh = jce_mesh_create(vertices, 3, indices, 3);
    TEST_ASSERT_NOT_NULL(mesh);
    const unsigned char pixels[16] = {0, 64, 128, 255};
    JceTexture texture = jce_texture_from_rgba(pixels, 2, 2);
    TEST_ASSERT_TRUE(jce_texture_valid(texture));
    TEST_ASSERT_TRUE(jce_texture_update_rgba(texture, pixels, 2, 2));
    JceTexture mip_texture = jce_texture_from_rgba_ex(pixels, 2, 2, 0);
    TEST_ASSERT_TRUE(jce_texture_valid(mip_texture));
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &after));
    TEST_ASSERT_EQUAL_UINT64(sizeof vertices, after.static_vertex_bytes - before.static_vertex_bytes);
    /* Triangle indices plus its three unique wireframe edges. */
    TEST_ASSERT_EQUAL_UINT64(9 * sizeof(uint32_t), after.static_index_bytes - before.static_index_bytes);
    TEST_ASSERT_EQUAL_UINT64(20, after.texture_create_bytes - before.texture_create_bytes);
    TEST_ASSERT_EQUAL_UINT64(2 * sizeof pixels, after.texture_update_bytes - before.texture_update_bytes);
    TEST_ASSERT_NULL(jce_mesh_create(NULL, 0, NULL, 0));
    TEST_ASSERT_FALSE(jce_texture_update_rgba(JCE_TEXTURE_INVALID, pixels, 2, 2));
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &unchanged));
    TEST_ASSERT_EQUAL_UINT64(after.static_vertex_bytes, unchanged.static_vertex_bytes);
    TEST_ASSERT_EQUAL_UINT64(after.texture_update_bytes, unchanged.texture_update_bytes);
    jce_texture_destroy(mip_texture);
    jce_texture_destroy(texture);
    jce_mesh_destroy(mesh);
    uint64_t generation = after.context_generation;
    jce_renderer_destroy(renderer);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_TRUE(jce_renderer_get_submitted_payload_stats(renderer, &reset));
    TEST_ASSERT_TRUE(reset.context_generation > generation);
    TEST_ASSERT_EQUAL_UINT64(0, reset.static_vertex_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, reset.texture_update_bytes);
    jce_renderer_destroy(renderer);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_static_mesh_and_texture_submission);
    return UNITY_END();
}
