#include "jce_model_pick_internal.h"

#include "jce_model_internal.h"
#include "jce_pick_geometry_internal.h"
#include "jce_skin_deform_internal.h"
#include "os/core/jce_memory.h"

#include <jce/os/core/jce_frustum.h>
#include <string.h>

#define JCE_MODEL_PICK_TRIANGLE_BUDGET (512u * 1024u)

static bool model_pick_bounds_missed(const JceModel *model, const jce_mat4 *root,
                                     jce_vec3 origin, jce_vec3 direction,
                                     float max_t)
{
    if (!model->has_aabb || (direction.x != 0.0f && fabsf(direction.x) < 1e-8f) ||
        (direction.y != 0.0f && fabsf(direction.y) < 1e-8f) ||
        (direction.z != 0.0f && fabsf(direction.z) < 1e-8f))
        return false;
    const float *local_min = model->aabb_min;
    const float *local_max = model->aabb_max;
    jce_vec3 min, max;
    jce_transform_aabb(root, jce_v3(local_min[0], local_min[1], local_min[2]),
        jce_v3(local_max[0], local_max[1], local_max[2]), &min, &max);
    float near;
    return !jce_ray_aabb_intersect(origin, direction, min, max, &near) ||
           near >= max_t;
}

/* Reuse one temporary vertex buffer across this query's deformed primitives.
 * Base vertices and posed GPU buffers remain owned by their render paths. */
static bool prepare_primitive(JcePickGeometry *geometry,
                              const JceModelPrimitive *primitive,
                              const jce_mat4 *palette, uint32_t joints,
                              const float *weights, uint32_t weight_count,
                              void **scratch, size_t *capacity,
                              uint32_t *triangle_budget)
{
    const JceSkinnedMesh *mesh = primitive->skinned_mesh;
    if (!jce_skinned_mesh_pick_geometry(mesh, geometry))
        return false;

    const uint32_t count = geometry->index_count
        ? geometry->index_count : geometry->vertex_count;
    if (count / 3u > *triangle_budget)
        return false;
    *triangle_budget -= count / 3u;

    const bool morph = primitive->morph && weights && weight_count &&
        jce_morph_vertex_count(primitive->morph) == geometry->vertex_count;
    const bool skin = jce_skinned_mesh_is_skinned(mesh) && palette && joints;
    if (!morph && !skin)
        return true;

    const void *base = jce_skinned_mesh_base_verts(mesh);
    const uint32_t stride = jce_skinned_mesh_stride(mesh);
    const uint32_t position = jce_skinned_mesh_pos_offset(mesh);
    if (!base || !stride || geometry->vertex_count > SIZE_MAX / stride)
        return false;

    const size_t bytes = (size_t)geometry->vertex_count * stride;
    if (*capacity < bytes) {
        void *grown = JCE_REALLOC(*scratch, bytes);
        if (!grown)
            return false;
        *scratch = grown;
        *capacity = bytes;
    }

    memcpy(*scratch, base, bytes);
    if (morph && !jce_morph_apply(primitive->morph, weights, weight_count,
            *scratch, *scratch, geometry->vertex_count, stride, (int32_t)position, -1))
        return false;
    if (skin)
        jce_skin_deform_vertices(*scratch, geometry->vertex_count, palette, joints);

    geometry->positions = (const unsigned char *)*scratch + position;
    geometry->stride = stride;
    return true;
}

JceModelRayResult jce_model_pick_ray(const JceModel *model,
    const jce_mat4 *world, const jce_mat4 *palette, uint32_t joints,
    const float *weights, uint32_t weight_count,
    jce_vec3 origin, jce_vec3 direction, float max_t, float *out_t)
{
    if (!model || !model->nodes || !isfinite(max_t) || !(max_t > 0.0f) ||
        !isfinite(origin.x) || !isfinite(origin.y) || !isfinite(origin.z) ||
        !isfinite(direction.x) || !isfinite(direction.y) || !isfinite(direction.z) ||
        (direction.x == 0.0f && direction.y == 0.0f && direction.z == 0.0f))
        return JCE_MODEL_RAY_UNAVAILABLE;

    const jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = world ? world : &identity;
    if (!model->skeleton && !joints && !weight_count &&
        model_pick_bounds_missed(model, root, origin, direction, max_t))
        return JCE_MODEL_RAY_MISS;

    const jce_mat4 *skin_palette = palette;
    uint32_t skin_joints = palette ? joints : 0;
    jce_mat4 rest[JCE_MAX_BONES];
    if (!skin_joints && model->skeleton) {
        skin_joints = jce_skeleton_joint_count(model->skeleton);
        if (skin_joints > JCE_MAX_BONES)
            skin_joints = JCE_MAX_BONES;
        jce_skeleton_evaluate(model->skeleton, NULL, rest, skin_joints);
        skin_palette = rest;
    }

    void *scratch = NULL;
    size_t capacity = 0;
    float best = max_t;
    bool hit = false;
    bool incomplete = false;
    uint32_t triangle_budget = JCE_MODEL_PICK_TRIANGLE_BUDGET;
    for (uint32_t n = 0; n < model->num_nodes && !incomplete; ++n) {
        const JceModelNode *node = &model->nodes[n];
        if (!node->primitives)
            continue;
        const jce_mat4 node_world = jce_m4_multiply(root, &node->local_transform);
        const jce_mat4 rigid_world = jce_model_static_node_world(
            model, node, root, &node_world, palette, joints);
        for (uint32_t p = 0; p < node->num_primitives; ++p) {
            const JceModelPrimitive *primitive = &node->primitives[p];
            if (!primitive->skinned_mesh && !primitive->static_mesh)
                continue;

            JcePickGeometry geometry;
            if (!prepare_primitive(&geometry, primitive, skin_palette,
                    skin_joints, weights, weight_count, &scratch, &capacity,
                    &triangle_budget)) {
                incomplete = true;
                break;
            }

            const bool skinned = jce_skinned_mesh_is_skinned(primitive->skinned_mesh);
            const jce_mat4 *transform = skinned
                ? (skin_joints ? root : &node_world) : &rigid_world;
            const bool double_sided = primitive->material_index < model->num_materials &&
                model->materials[primitive->material_index].double_sided;
            if (jce_pick_geometry_ray(&geometry, transform, double_sided,
                                      origin, direction, best, &best))
                hit = true;
        }
    }
    JCE_FREE(scratch);

    if (incomplete)
        return JCE_MODEL_RAY_UNAVAILABLE;
    if (!hit)
        return JCE_MODEL_RAY_MISS;
    if (out_t)
        *out_t = best;
    return JCE_MODEL_RAY_HIT;
}
