#include "jce_pick_geometry_internal.h"

#include <float.h>
#include <string.h>

static jce_vec3 world_position(const JcePickGeometry *geometry,
                              const jce_mat4 *world, uint32_t index)
{
    float position[3];
    const unsigned char *bytes = geometry->positions;
    memcpy(position, bytes + (size_t)index * geometry->stride, sizeof(position));
    const jce_vec4 point = jce_m4_mul_v4(world,
        jce_v4(position[0], position[1], position[2], 1.0f));
    return jce_v3(point.x, point.y, point.z);
}

static bool intersect_triangle(jce_vec3 origin, jce_vec3 direction,
                               jce_vec3 a, jce_vec3 b, jce_vec3 c,
                               bool double_sided, float *best)
{
    const jce_vec3 edge_ab = jce_v3_sub(b, a);
    const jce_vec3 edge_ac = jce_v3_sub(c, a);
    const jce_vec3 perpendicular = jce_v3_cross(direction, edge_ac);
    const float determinant = jce_v3_dot(edge_ab, perpendicular);
    const float facing = double_sided ? fabsf(determinant) : determinant;
    if (!(facing >= FLT_MIN) || !isfinite(facing))
        return false;

    const jce_vec3 relative = jce_v3_sub(origin, a);
    const float u = jce_v3_dot(relative, perpendicular) / determinant;
    if (!(u >= 0.0f && u <= 1.0f))
        return false;

    const jce_vec3 cross = jce_v3_cross(relative, edge_ab);
    const float v = jce_v3_dot(direction, cross) / determinant;
    if (!(v >= 0.0f && u + v <= 1.0f))
        return false;

    const float distance = jce_v3_dot(edge_ac, cross) / determinant;
    if (!(distance >= 0.0f && distance < *best))
        return false;

    *best = distance;
    return true;
}

bool jce_pick_geometry_ray(const JcePickGeometry *geometry,
                           const jce_mat4 *world, bool double_sided,
                           jce_vec3 origin, jce_vec3 direction,
                           float max_t, float *out_t)
{
    if (!geometry || !geometry->positions || !world || !(max_t > 0.0f) ||
        geometry->stride < 3u * sizeof(float) ||
        geometry->vertex_count > SIZE_MAX / geometry->stride ||
        (geometry->index_count && !geometry->indices))
        return false;

    const uint32_t *indices = geometry->index_count ? geometry->indices : NULL;
    const uint32_t count = geometry->index_count
        ? geometry->index_count : geometry->vertex_count;
    const uint32_t triangles = count / 3u;
    float best = max_t;
    bool hit = false;
    for (uint32_t triangle = 0; triangle < triangles; ++triangle) {
        const uint32_t first = triangle * 3u;
        const uint32_t a = indices ? indices[first] : first;
        const uint32_t b = indices ? indices[first + 1u] : first + 1u;
        const uint32_t c = indices ? indices[first + 2u] : first + 2u;
        if (a >= geometry->vertex_count || b >= geometry->vertex_count ||
            c >= geometry->vertex_count)
            continue;

        if (intersect_triangle(origin, direction,
                world_position(geometry, world, a),
                world_position(geometry, world, b),
                world_position(geometry, world, c),
                double_sided, &best))
            hit = true;
    }

    if (hit && out_t)
        *out_t = best;
    return hit;
}
