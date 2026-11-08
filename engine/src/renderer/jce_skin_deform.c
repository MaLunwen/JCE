/* Linear blend skinning for backends without a usable GPU bone palette. */
#include "jce_skin_deform_internal.h"

static void store_direction(float dst[3], jce_vec4 value)
{
    jce_vec3 xyz = jce_v3(value.x, value.y, value.z);
    float length = jce_v3_len(xyz);
    if (!isfinite(length) || length < 1e-8f)
        return;

    jce_vec3 unit = jce_v3_scale(xyz, 1.0f / length);
    dst[0] = unit.x;
    dst[1] = unit.y;
    dst[2] = unit.z;
}

void jce_skin_deform_vertices(JceSkinnedVertex *vertices, uint32_t count,
                              const jce_mat4 *palette, uint32_t joints)
{
    if (!vertices || !palette || joints == 0)
        return;

    for (uint32_t v = 0; v < count; ++v) {
        JceSkinnedVertex *vertex = &vertices[v];
        jce_mat4 blend = {0};
        float weight_sum = 0.0f;
        for (uint32_t i = 0; i < JCE_MAX_BONE_INFLUENCES; ++i) {
            uint32_t joint = vertex->joints[i];
            float weight = vertex->weights[i];
            if (joint >= joints || !(weight > 0.0f && weight <= 1.0f))
                continue;

            /* Only the first three rows affect xyz in the shader. */
            for (uint32_t col = 0; col < 4; ++col)
                for (uint32_t row = 0; row < 3; ++row)
                    blend.raw[col][row] += weight * palette[joint].raw[col][row];
            weight_sum += weight;
        }
        /* Invalid influences must not collapse otherwise drawable vertices. */
        if (weight_sum < 1e-8f)
            continue;

        jce_vec4 position = jce_m4_mul_v4(&blend,
            jce_v4(vertex->pos[0], vertex->pos[1], vertex->pos[2], 1.0f));
        jce_vec3 xyz = jce_v3_scale(jce_v3(position.x, position.y, position.z),
                                   1.0f / weight_sum);
        if (!isfinite(xyz.x) || !isfinite(xyz.y) || !isfinite(xyz.z))
            continue;

        vertex->pos[0] = xyz.x;
        vertex->pos[1] = xyz.y;
        vertex->pos[2] = xyz.z;
        store_direction(vertex->normal, jce_m4_mul_v4(&blend,
            jce_v4(vertex->normal[0], vertex->normal[1], vertex->normal[2], 0.0f)));
        store_direction(vertex->tangent, jce_m4_mul_v4(&blend,
            jce_v4(vertex->tangent[0], vertex->tangent[1], vertex->tangent[2], 0.0f)));
    }
}
