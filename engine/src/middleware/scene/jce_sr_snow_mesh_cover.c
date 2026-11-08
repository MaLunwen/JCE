#include "jce_sr_snow_mesh_cover.h"
#include "jce_sr_internal.h"
#include "renderer/jce_mesh_create_internal.h"
#include "renderer/jce_model_internal.h"
#include "renderer/jce_pick_geometry_internal.h"

#include <math.h>
#include <string.h>

enum { SNOW_SOURCE_TRIANGLE_BUDGET = 64 };

typedef struct SrSnowMeshPack {
    float (*rows)[3][4];
    uint16_t count;
    uint16_t capacity;
    uint32_t triangles_left;
} SrSnowMeshPack;

static bool snow_mesh_positive_transform(const jce_mat4 *world)
{
    const jce_vec3 x = jce_v3(world->col[0].x, world->col[0].y, world->col[0].z);
    const jce_vec3 y = jce_v3(world->col[1].x, world->col[1].y, world->col[1].z);
    const jce_vec3 z = jce_v3(world->col[2].x, world->col[2].y, world->col[2].z);
    const float determinant = jce_v3_dot(x, jce_v3_cross(y, z));
    return isfinite(determinant) && determinant > 1e-8f;
}

static jce_vec3 snow_mesh_world_vertex(const JcePickGeometry *mesh,
    const jce_mat4 *world, uint32_t index)
{
    float pos[3];
    const unsigned char *base = mesh->positions;
    memcpy(pos, base + (size_t)index * mesh->stride, sizeof pos);
    const jce_vec4 p = jce_m4_mul_v4(world, (jce_vec4){pos[0], pos[1], pos[2], 1.0f});
    return jce_v3(p.x, p.y, p.z);
}

static bool snow_mesh_pack_geometry(SrSnowMeshPack *pack, const JcePickGeometry *mesh,
    const jce_mat4 *world, bool double_sided)
{
    const uint32_t index_count = mesh->indices ? mesh->index_count : mesh->vertex_count;
    if (!mesh->positions || mesh->stride < 3u * sizeof(float) || index_count % 3u != 0u ||
        index_count / 3u > pack->triangles_left || !snow_mesh_positive_transform(world))
        return false;
    pack->triangles_left -= index_count / 3u;

    for (uint32_t i = 0; i < index_count; i += 3u) {
        const uint32_t ia = mesh->indices ? mesh->indices[i] : i;
        const uint32_t ib = mesh->indices ? mesh->indices[i + 1u] : i + 1u;
        const uint32_t ic = mesh->indices ? mesh->indices[i + 2u] : i + 2u;
        if (ia >= mesh->vertex_count || ib >= mesh->vertex_count || ic >= mesh->vertex_count)
            return false;
        const jce_vec3 origin = snow_mesh_world_vertex(mesh, world, ia);
        const jce_vec3 u = jce_v3_sub(snow_mesh_world_vertex(mesh, world, ib), origin);
        const jce_vec3 v = jce_v3_sub(snow_mesh_world_vertex(mesh, world, ic), origin);
        const float area = u.x * v.z - u.z * v.x;
        if (!isfinite(area)) return false;
        /* The sign is the negated world normal Y. Ignore vertical faces,
         * and backfaces of one-sided opaque materials, as seen from above. */
        if (fabsf(area) < 1e-4f || (!double_sided && area > 0.0f)) continue;
        const float reach_x = fmaxf(fabsf(u.x), fabsf(v.x));
        const float reach_z = fmaxf(fabsf(u.z), fabsf(v.z));
        if (!(fabsf(origin.x) <= 32768.0f && fabsf(origin.z) <= 32768.0f &&
              isfinite(origin.y) && reach_x <= 48.0f && reach_z <= 48.0f &&
              fabsf(u.y) + fabsf(v.y) <= 48.0f) || pack->count == pack->capacity)
            return false;

        const float inverse = 1.0f / area;
        const float data[3][4] = {
            {origin.x, origin.y, origin.z, -2.0f},
            {v.z * inverse, -v.x * inverse, -u.z * inverse, u.x * inverse},
            {u.y, v.y, reach_x + 0.001f, reach_z + 0.001f},
        };
        memcpy(pack->rows[pack->count++], data, sizeof data);
    }
    return true;
}

static bool snow_mesh_pack_model(SrSnowMeshPack *pack, const JceModel *asset,
    const jce_mat4 *world)
{
    if (!asset || !asset->nodes || asset->skeleton || asset->num_anims)
        return false;

    for (uint32_t n = 0; n < asset->num_nodes; ++n) {
        const JceModelNode *node = &asset->nodes[n];
        if (!node->primitives) continue;
        const jce_mat4 node_world = jce_m4_multiply(world, &node->local_transform);
        for (uint32_t p = 0; p < node->num_primitives; ++p) {
            const JceModelPrimitive *primitive = &node->primitives[p];
            const JceSkinnedMesh *mesh = primitive->skinned_mesh;
            if (!mesh && !primitive->static_mesh) continue;
            if (!mesh || primitive->morph || jce_skinned_mesh_is_skinned(mesh) ||
                jce_skinned_mesh_lod_count(mesh) != 0u)
                return false;

            const JcePbrMaterial *material = primitive->material_index < asset->num_materials
                ? &asset->materials[primitive->material_index] : NULL;
            if (material && material->alpha_mode != JCE_ALPHA_OPAQUE)
                return false;
            JcePickGeometry geom;
            if (!jce_skinned_mesh_pick_geometry(mesh, &geom) ||
                !snow_mesh_pack_geometry(pack, &geom, &node_world,
                                         material && material->double_sided))
                return false;
        }
    }
    return true;
}

uint16_t sr_snow_mesh_cover(JceSceneRenderer *sr, JceScene *scene, JceEntity entity,
    const jce_mat4 *world, float out[][3][4], uint16_t capacity)
{
    const JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, entity);
    if (!mr || !mr->visible || !capacity || mr->alpha_mode != JCE_ALPHA_OPAQUE ||
        !jce_scene_component_enabled(scene, entity, JCE_COMP_FLAG_MESH_RENDERER) ||
        jce_scene_has_skeletal_animator(scene, entity) || jce_scene_has_lod_group(scene, entity))
        return 0u;

    SrSnowMeshPack pack = {out, 0u, capacity, SNOW_SOURCE_TRIANGLE_BUDGET};
    if (mr->mesh_path[0]) {
        const char *path = sr_mesh_renderer_model_path(scene, entity);
        if (!path) return 0u;
        const SrModelCache *asset = sr_get_model(sr, path, (uint32_t)entity);
        if (!asset || !snow_mesh_pack_model(&pack, asset->model, world)) return 0u;
    } else {
        if (mr->mesh_shape != 0 || !sr->cube_mesh) return 0u;
        JceMeshVertex vertices[24];
        uint32_t indices[36];
        jce_mesh_cube_geometry(1.0f, vertices, indices);
        const JcePickGeometry geom = {vertices[0].pos, sizeof vertices[0],
                                      24u, indices, 36u};
        if (!snow_mesh_pack_geometry(&pack, &geom, world, false)) return 0u;
    }
    return pack.count;
}
