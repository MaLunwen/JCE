/* Exercise the compatibility deformer without a graphics device. */
#include "unity.h"
#include "renderer/jce_skin_deform_internal.h"

void setUp(void) {}
void tearDown(void) {}

static JceSkinnedVertex fixture_vertex(void)
{
    JceSkinnedVertex vertex = {
        {2.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.2f, 0.8f},
        {1.0f, 0.0f, 0.0f, -1.0f},
        {0, JCE_MAX_BONES - 1, 0, 0},
        {0.25f, 0.75f, 0.0f, 0.0f},
    };
    return vertex;
}

static void blends_full_palette_with_current_vertex_shape(void)
{
    JceSkinnedVertex vertex = fixture_vertex();
    jce_mat4 palette[JCE_MAX_BONES] = {0};
    palette[0] = jce_m4_identity();
    jce_mat4 *joint = &palette[JCE_MAX_BONES - 1];
    *joint = jce_m4_identity();
    joint->col[0] = jce_v4(0.0f, 1.0f, 0.0f, 0.0f);
    joint->col[1] = jce_v4(-1.0f, 0.0f, 0.0f, 0.0f);
    joint->col[3] = jce_v4(4.0f, -2.0f, 0.0f, 1.0f);

    /* The y=1 vertex can be a morph output; rotation must consume that value. */
    jce_skin_deform_vertices(&vertex, 1, palette, JCE_MAX_BONES);

    const float expected_position[] = {2.75f, 0.25f, 0.0f};
    const float expected_normal[] = {-0.9486833f, 0.3162278f, 0.0f};
    const float expected_tangent[] = {0.3162278f, 0.9486833f, 0.0f, -1.0f};
    TEST_ASSERT_FLOAT_ARRAY_WITHIN(1e-6f, expected_position, vertex.pos, 3);
    TEST_ASSERT_FLOAT_ARRAY_WITHIN(1e-6f, expected_normal, vertex.normal, 3);
    TEST_ASSERT_FLOAT_ARRAY_WITHIN(1e-6f, expected_tangent, vertex.tangent, 4);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, vertex.uv[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, vertex.uv[1]);
    TEST_ASSERT_EQUAL_UINT8(JCE_MAX_BONES - 1, vertex.joints[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, vertex.weights[1]);
}

static void ignores_bad_influences_and_preserves_unweighted_vertices(void)
{
    JceSkinnedVertex vertices[] = {fixture_vertex(), fixture_vertex()};
    jce_mat4 palette = jce_m4_identity();
    palette.col[3].x = 5.0f;
    vertices[0].joints[1] = UINT8_MAX;
    vertices[0].weights[2] = NAN;
    vertices[0].weights[3] = -1.0f;
    vertices[1].weights[0] = 0.0f;
    vertices[1].weights[3] = 2.0f;
    const JceSkinnedVertex untouched = vertices[1];

    jce_skin_deform_vertices(vertices, 2, &palette, 1);

    TEST_ASSERT_EQUAL_FLOAT(7.0f, vertices[0].pos[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, vertices[0].pos[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, vertices[0].normal[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, vertices[0].tangent[0]);
    TEST_ASSERT_EQUAL_MEMORY(untouched.pos, vertices[1].pos,
                             sizeof(untouched.pos));
    TEST_ASSERT_EQUAL_MEMORY(untouched.normal, vertices[1].normal,
                             sizeof(untouched.normal));
    TEST_ASSERT_EQUAL_MEMORY(untouched.tangent, vertices[1].tangent,
                             sizeof(untouched.tangent));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(blends_full_palette_with_current_vertex_shape);
    RUN_TEST(ignores_bad_influences_and_preserves_unweighted_vertices);
    return UNITY_END();
}
