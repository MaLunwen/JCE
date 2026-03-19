/*
 * jce_mesh.c  GPU mesh implementation.
 */

#include "jce_mesh.h"
#include "jce_renderer_internal.h"
#include "resource/jce_model_loader.h"
#include "core/jce_log.h"
#include "core/jce_math.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <string.h>

#define LOG_TAG "jce_mesh"

struct JceMesh {
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
    bgfx_vertex_layout_t        layout;
    uint32_t                    num_verts;
    uint32_t                    num_indices;
};

/* Shared mesh vertex layout (position float3 + normal float3 + texcoord float2). */
static void init_mesh_layout(bgfx_vertex_layout_t *layout)
{
    bgfx_vertex_layout_begin(layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_NORMAL, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(layout);
}

JceMesh *jce_mesh_create(const JceMeshVertex *vertices, uint32_t num_verts,
                          const uint32_t *indices, uint32_t num_indices)
{
    if (!vertices || num_verts == 0) return NULL;

    JceMesh *m = (JceMesh *)SDL_calloc(1, sizeof(*m));
    if (!m) return NULL;

    init_mesh_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;

    /* Create vertex buffer. */
    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JceMeshVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);

    /* Create index buffer (optional). */
    if (indices && num_indices > 0) {
        const bgfx_memory_t *imem = bgfx_copy(indices,
                                               num_indices * (uint32_t)sizeof(uint32_t));
        m->ibh = bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);
    } else {
        m->ibh.idx = UINT16_MAX;
    }

    return m;
}

JceMesh *jce_mesh_load(PakArchive *pak, const char *asset_path)
{
    return jce_model_load(pak, asset_path);
}

void jce_mesh_destroy(JceMesh *mesh)
{
    if (!mesh) return;
    if (mesh->vbh.idx != UINT16_MAX)
        bgfx_destroy_vertex_buffer(mesh->vbh);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_destroy_index_buffer(mesh->ibh);
    SDL_free(mesh);
}

void jce_mesh_submit(const JceMesh *mesh, JceRenderer *r, uint16_t view_id)
{
    if (!mesh || !r) return;

    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    bgfx_set_state(BGFX_STATE_DEFAULT, 0);

    bgfx_program_handle_t prog = jce_renderer_get_program_mesh(r);
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

uint32_t jce_mesh_vertex_count(const JceMesh *mesh)
{
    return mesh ? mesh->num_verts : 0;
}

uint32_t jce_mesh_index_count(const JceMesh *mesh)
{
    return mesh ? mesh->num_indices : 0;
}

/* -- Built-in procedural meshes ------------------------------------ */

JceMesh *jce_mesh_create_cube(float size)
{
    float hs = size * 0.5f;

    /* 24 vertices (4 per face, unique normals). */
    JceMeshVertex verts[24] = {
        /* Front face (+Z) */
        {{ -hs, -hs,  hs }, {  0,  0,  1 }, { 0, 1 }},
        {{  hs, -hs,  hs }, {  0,  0,  1 }, { 1, 1 }},
        {{  hs,  hs,  hs }, {  0,  0,  1 }, { 1, 0 }},
        {{ -hs,  hs,  hs }, {  0,  0,  1 }, { 0, 0 }},
        /* Back face (-Z) */
        {{  hs, -hs, -hs }, {  0,  0, -1 }, { 0, 1 }},
        {{ -hs, -hs, -hs }, {  0,  0, -1 }, { 1, 1 }},
        {{ -hs,  hs, -hs }, {  0,  0, -1 }, { 1, 0 }},
        {{  hs,  hs, -hs }, {  0,  0, -1 }, { 0, 0 }},
        /* Top face (+Y) */
        {{ -hs,  hs,  hs }, {  0,  1,  0 }, { 0, 1 }},
        {{  hs,  hs,  hs }, {  0,  1,  0 }, { 1, 1 }},
        {{  hs,  hs, -hs }, {  0,  1,  0 }, { 1, 0 }},
        {{ -hs,  hs, -hs }, {  0,  1,  0 }, { 0, 0 }},
        /* Bottom face (-Y) */
        {{ -hs, -hs, -hs }, {  0, -1,  0 }, { 0, 1 }},
        {{  hs, -hs, -hs }, {  0, -1,  0 }, { 1, 1 }},
        {{  hs, -hs,  hs }, {  0, -1,  0 }, { 1, 0 }},
        {{ -hs, -hs,  hs }, {  0, -1,  0 }, { 0, 0 }},
        /* Right face (+X) */
        {{  hs, -hs,  hs }, {  1,  0,  0 }, { 0, 1 }},
        {{  hs, -hs, -hs }, {  1,  0,  0 }, { 1, 1 }},
        {{  hs,  hs, -hs }, {  1,  0,  0 }, { 1, 0 }},
        {{  hs,  hs,  hs }, {  1,  0,  0 }, { 0, 0 }},
        /* Left face (-X) */
        {{ -hs, -hs, -hs }, { -1,  0,  0 }, { 0, 1 }},
        {{ -hs, -hs,  hs }, { -1,  0,  0 }, { 1, 1 }},
        {{ -hs,  hs,  hs }, { -1,  0,  0 }, { 1, 0 }},
        {{ -hs,  hs, -hs }, { -1,  0,  0 }, { 0, 0 }},
    };

    uint32_t indices[36];
    for (int face = 0; face < 6; face++) {
        int b = face * 4;
        int i = face * 6;
        indices[i + 0] = (uint32_t)(b + 0);
        indices[i + 1] = (uint32_t)(b + 1);
        indices[i + 2] = (uint32_t)(b + 2);
        indices[i + 3] = (uint32_t)(b + 0);
        indices[i + 4] = (uint32_t)(b + 2);
        indices[i + 5] = (uint32_t)(b + 3);
    }

    return jce_mesh_create(verts, 24, indices, 36);
}

JceMesh *jce_mesh_create_plane(float width, float depth, uint32_t subdivs)
{
    return jce_mesh_create_plane_ex(width, depth, subdivs, 1.0f);
}

JceMesh *jce_mesh_create_plane_ex(float width, float depth,
                                   uint32_t subdivs, float uv_scale)
{
    if (subdivs == 0) subdivs = 1;

    uint32_t nx = subdivs + 1;
    uint32_t nz = subdivs + 1;
    uint32_t num_verts = nx * nz;
    uint32_t num_quads = subdivs * subdivs;
    uint32_t num_indices = num_quads * 6;

    JceMeshVertex *verts = (JceMeshVertex *)SDL_malloc(num_verts * sizeof(JceMeshVertex));
    uint32_t *indices = (uint32_t *)SDL_malloc(num_indices * sizeof(uint32_t));
    if (!verts || !indices) {
        SDL_free(verts);
        SDL_free(indices);
        return NULL;
    }

    float hw = width * 0.5f;
    float hd = depth * 0.5f;

    for (uint32_t z = 0; z < nz; z++) {
        for (uint32_t x = 0; x < nx; x++) {
            uint32_t idx = z * nx + x;
            float fx = (float)x / (float)subdivs;
            float fz = (float)z / (float)subdivs;
            verts[idx].pos[0] = -hw + fx * width;
            verts[idx].pos[1] = 0.0f;
            verts[idx].pos[2] = -hd + fz * depth;
            verts[idx].normal[0] = 0.0f;
            verts[idx].normal[1] = 1.0f;
            verts[idx].normal[2] = 0.0f;
            verts[idx].uv[0] = fx * uv_scale;
            verts[idx].uv[1] = fz * uv_scale;
        }
    }

    uint32_t ii = 0;
    for (uint32_t z = 0; z < subdivs; z++) {
        for (uint32_t x = 0; x < subdivs; x++) {
            uint32_t tl = z * nx + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * nx + x;
            uint32_t br = bl + 1;
            indices[ii++] = tl;
            indices[ii++] = bl;
            indices[ii++] = tr;
            indices[ii++] = tr;
            indices[ii++] = bl;
            indices[ii++] = br;
        }
    }

    JceMesh *mesh = jce_mesh_create(verts, num_verts, indices, num_indices);
    SDL_free(verts);
    SDL_free(indices);
    return mesh;
}
