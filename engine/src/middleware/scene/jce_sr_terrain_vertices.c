#include "jce_sr_terrain_vertices.h"
#include <string.h>

SrTerrainVertexResult sr_terrain_vertices_create(SrTerrainVertexOwner *owner,
    const JceTerrainVertex *source, uint32_t count,
    const SrTerrainVertexAllocator *allocator)
{
    /* Mesh upload length is uint32_t even on a 64-bit host. */
    if (!owner || owner->vertices || !source || !count || !allocator ||
        !allocator->allocate || !allocator->release ||
        count > UINT32_MAX / sizeof(JceMeshVertex) ||
        count > SIZE_MAX / sizeof(JceMeshVertex))
        return SR_TERRAIN_VERTICES_INVALID;
    const size_t bytes = (size_t)count * sizeof(JceMeshVertex);
    JceMeshVertex *converted = (JceMeshVertex *)allocator->allocate(bytes, allocator->user);
    if (!converted) return SR_TERRAIN_VERTICES_OOM;
    memset(converted, 0, bytes);
    for (uint32_t i = 0; i < count; ++i) {
        converted[i].pos[0] = source[i].px;
        converted[i].pos[1] = source[i].py;
        converted[i].pos[2] = source[i].pz;
        converted[i].normal[0] = source[i].nx;
        converted[i].normal[1] = source[i].ny;
        converted[i].normal[2] = source[i].nz;
        converted[i].uv[0] = source[i].u;
        converted[i].uv[1] = source[i].v;
        /* The entire stream leaves tangent.xyz zero. jce_mesh_create tests
         * that exact condition before its existing tangent generation. */
    }
    owner->allocator = *allocator;
    owner->vertices = converted;
    return SR_TERRAIN_VERTICES_OK;
}

void sr_terrain_vertices_destroy(SrTerrainVertexOwner *owner)
{
    if (!owner || !owner->vertices) return;
    JceMeshVertex *vertices = owner->vertices;
    const SrTerrainVertexAllocator allocator = owner->allocator;
    memset(owner, 0, sizeof *owner);
    allocator.release(vertices, allocator.user);
}
