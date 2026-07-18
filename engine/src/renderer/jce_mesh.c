/*
 * jce_mesh.c  GPU mesh implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_mesh.h>

#include "jce_renderer_internal.h"
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/jce_render_encoder.h"

#define LOG_TAG "jce_mesh"

struct JceMesh {
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
    bgfx_index_buffer_handle_t  wf_ibh;     /* wireframe line indices */
    bgfx_vertex_layout_t        layout;
    uint32_t                    num_verts;
    uint32_t                    num_indices;
    uint32_t                    num_wf_indices;
    float                       aabb_min[3];
    float                       aabb_max[3];
};

typedef struct JceEdgeKey {
    uint32_t a;
    uint32_t b;
} JceEdgeKey;

static int edge_key_compare(const void *lhs, const void *rhs)
{
    const JceEdgeKey *l = (const JceEdgeKey *)lhs;
    const JceEdgeKey *r = (const JceEdgeKey *)rhs;
    if (l->a < r->a) return -1;
    if (l->a > r->a) return 1;
    if (l->b < r->b) return -1;
    if (l->b > r->b) return 1;
    return 0;
}

static bool build_unique_wireframe_indices(const uint32_t *indices,
                                           uint32_t num_indices,
                                           uint32_t **out_wf,
                                           uint32_t *out_wf_count)
{
    if (!indices || num_indices < 3 || !out_wf || !out_wf_count)
        return false;

    const uint32_t num_tris = num_indices / 3;
    const uint32_t edge_capacity = num_tris * 3;
    if (edge_capacity == 0)
        return false;

    JceEdgeKey *edges = (JceEdgeKey *)JCE_MALLOC((size_t)edge_capacity * sizeof(JceEdgeKey));
    if (!edges)
        return false;

    uint32_t edge_count = 0;
    for (uint32_t t = 0; t < num_tris; t++) {
        const uint32_t tri[3] = {
            indices[t * 3 + 0],
            indices[t * 3 + 1],
            indices[t * 3 + 2]
        };

        for (int e = 0; e < 3; e++) {
            uint32_t a = tri[e];
            uint32_t b = tri[(e + 1) % 3];
            if (a == b)
                continue;
            if (a > b) {
                uint32_t tmp = a;
                a = b;
                b = tmp;
            }
            edges[edge_count].a = a;
            edges[edge_count].b = b;
            edge_count++;
        }
    }

    if (edge_count == 0) {
        JCE_FREE(edges);
        return false;
    }

    qsort(edges, edge_count, sizeof(JceEdgeKey), edge_key_compare);

    uint32_t unique_count = 1;
    for (uint32_t i = 1; i < edge_count; i++) {
        if (edges[i].a != edges[i - 1].a || edges[i].b != edges[i - 1].b)
            unique_count++;
    }

    uint32_t *wf = (uint32_t *)JCE_MALLOC((size_t)unique_count * 2u * sizeof(uint32_t));
    if (!wf) {
        JCE_FREE(edges);
        return false;
    }

    uint32_t write = 0;
    wf[write++] = edges[0].a;
    wf[write++] = edges[0].b;
    for (uint32_t i = 1; i < edge_count; i++) {
        if (edges[i].a == edges[i - 1].a && edges[i].b == edges[i - 1].b)
            continue;
        wf[write++] = edges[i].a;
        wf[write++] = edges[i].b;
    }

    JCE_FREE(edges);

    *out_wf = wf;
    *out_wf_count = write;
    return true;
}

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

    JceMesh *m = (JceMesh *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    init_mesh_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;

    /* Compute local-space AABB from the vertex stream. */
    {
        float mn[3] = { vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2] };
        float mx[3] = { vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2] };
        for (uint32_t i = 1; i < num_verts; i++) {
            const float *p = vertices[i].pos;
            if (p[0] < mn[0]) mn[0] = p[0]; if (p[0] > mx[0]) mx[0] = p[0];
            if (p[1] < mn[1]) mn[1] = p[1]; if (p[1] > mx[1]) mx[1] = p[1];
            if (p[2] < mn[2]) mn[2] = p[2]; if (p[2] > mx[2]) mx[2] = p[2];
        }
        m->aabb_min[0] = mn[0]; m->aabb_min[1] = mn[1]; m->aabb_min[2] = mn[2];
        m->aabb_max[0] = mx[0]; m->aabb_max[1] = mx[1]; m->aabb_max[2] = mx[2];
    }

    /* Create vertex buffer. */
    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JceMeshVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);
    if (m->vbh.idx == UINT16_MAX) {
        /* bgfx static vertex-buffer pool exhausted (or upload failed): fail the
         * whole mesh instead of returning a zombie with an invalid vbh that
         * would forever submit nothing. Degrades gracefully under handle
         * pressure rather than feeding a broken handle into the draw list. */
        LOG_WARN(LOG_TAG, "mesh: vertex buffer allocation failed");
        JCE_FREE(m);
        return NULL;
    }

    /* Create index buffer (optional). */
    if (indices && num_indices > 0) {
        const bgfx_memory_t *imem = bgfx_copy(indices,
                                               num_indices * (uint32_t)sizeof(uint32_t));
        m->ibh = bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);

        /* Build wireframe index buffer using UNIQUE undirected edges.
         * This preserves full wireframe topology while avoiding duplicate
         * interior edges from adjacent triangles. */
        uint32_t *wf = NULL;
        uint32_t wf_count = 0;
        if (build_unique_wireframe_indices(indices, num_indices,
                                           &wf, &wf_count)
            && wf && wf_count > 0) {
            const bgfx_memory_t *wmem = bgfx_copy(wf,
                                                    wf_count * (uint32_t)sizeof(uint32_t));
            m->wf_ibh = bgfx_create_index_buffer(wmem, BGFX_BUFFER_INDEX32);
            m->num_wf_indices = wf_count;
            JCE_FREE(wf);
        } else {
            m->wf_ibh.idx = UINT16_MAX;
        }
    } else {
        m->ibh.idx = UINT16_MAX;
        m->wf_ibh.idx = UINT16_MAX;
    }

    return m;
}

JceMesh *jce_mesh_load(const JcePakArchive *pak, const char *asset_path)
{
    (void)pak; (void)asset_path;
    /* Stub: glTF assets are loaded via jce_model_load_gltf().  Returning
     * NULL lets callers fall through to primitive-shape fallback without
     * spamming the log every frame. */
    return NULL;
}

void jce_mesh_destroy(JceMesh *mesh)
{
    if (!mesh) return;
    if (mesh->vbh.idx != UINT16_MAX)
        bgfx_destroy_vertex_buffer(mesh->vbh);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_destroy_index_buffer(mesh->ibh);
    if (mesh->wf_ibh.idx != UINT16_MAX)
        bgfx_destroy_index_buffer(mesh->wf_ibh);
    JCE_FREE(mesh);
}

void jce_mesh_submit(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (jce_renderer_get_wireframe(r) && mesh->wf_ibh.idx != UINT16_MAX) {
        /* Wireframe: use line index buffer, no face culling. */
        jce_enc_set_index_buffer(mesh->wf_ibh, 0, mesh->num_wf_indices);
        jce_enc_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                     | BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS
                     | BGFX_STATE_MSAA      | BGFX_STATE_PT_LINES, 0);
    } else {
        if (mesh->ibh.idx != UINT16_MAX)
            jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
        jce_enc_set_state(BGFX_STATE_DEFAULT, 0);
    }

    JceShaderHandle sh = jce_renderer_get_program_mesh(r);
    bgfx_program_handle_t prog = { sh.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_wireframe_overlay(const JceMesh *mesh, const JceRenderer *r,
                                       uint16_t view_id)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->wf_ibh.idx != UINT16_MAX) {
        jce_enc_set_index_buffer(mesh->wf_ibh, 0, mesh->num_wf_indices);
    } else if (mesh->ibh.idx != UINT16_MAX) {
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
    }

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LEQUAL
                   | BGFX_STATE_MSAA | BGFX_STATE_PT_LINES;

    /* LINEAA is visually nice but very expensive on dense meshes. */
    if (mesh->num_wf_indices < 1500000u)
        state |= BGFX_STATE_LINEAA;

    /* LEQUAL depth test so wireframe overlay renders on top of solid geometry
     * at the same depth.  LINEAA for smooth anti-aliased lines. */
    jce_enc_set_state(state, 0);

    JceShaderHandle sh = jce_renderer_get_program_mesh(r);
    bgfx_program_handle_t prog = { sh.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_pbr(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id)
{
    jce_mesh_submit_pbr_state(mesh, r, view_id, BGFX_STATE_DEFAULT);
}

void jce_mesh_submit_pbr_state(const JceMesh *mesh, const JceRenderer *r,
                               uint16_t view_id, uint64_t state)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    /* state==0 keeps the historical opaque behaviour.  A non-zero state
     * carries the material's blend / cull (double_sided drops CULL_CW) /
     * depth-write flags, built by jce_pbr_material_render_state(). */
    jce_enc_set_state(state ? state : BGFX_STATE_DEFAULT, 0);

    JceShaderHandle sh = jce_renderer_get_program_pbr(r);
    bgfx_program_handle_t prog = { sh.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_pbr_with_program(const JceMesh        *mesh,
                                       const JceRenderer    *r,
                                       uint16_t              view_id,
                                       JceShaderHandle       program)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    jce_enc_set_state(BGFX_STATE_DEFAULT, 0);

    /* Fall back to the renderer's default PBR program when the
     * override handle is invalid — keeps callers simple (they can
     * always pass the material's custom_program even if unset). */
    uint16_t idx = program.idx;
    if (idx == UINT16_MAX)
        idx = jce_renderer_get_program_pbr(r).idx;

    bgfx_program_handle_t prog = { idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_pick_id(const JceMesh        *mesh,
                             const JceRenderer    *r,
                             uint16_t              view_id,
                             JceShaderHandle       program,
                             bool                  double_sided)
{
    if (!mesh || !r || program.idx == UINT16_MAX)
        return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                     BGFX_STATE_MSAA;
    if (!double_sided)
        state |= BGFX_STATE_CULL_CW;
    jce_enc_set_state(state, 0);

    bgfx_program_handle_t prog = { program.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_terrain(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    jce_enc_set_state(BGFX_STATE_DEFAULT, 0);

    JceShaderHandle sh = jce_renderer_get_program_terrain(r);
    bgfx_program_handle_t prog = { sh.idx };
    if (prog.idx == UINT16_MAX) {
        /* Fall back to plain PBR program if terrain shader failed to load. */
        sh = jce_renderer_get_program_pbr(r);
        prog.idx = sh.idx;
    }
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_shadow(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    /* Depth-only: write Z, cull front faces to reduce peter-panning. */
    jce_enc_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);

    JceShaderHandle sh = jce_renderer_get_program_shadow(r);
    bgfx_program_handle_t prog = { sh.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_mesh_submit_overlay(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id)
{
    if (!mesh || !r) return;

    jce_enc_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    /* State is NOT set here — caller controls blend mode etc. */
    JceShaderHandle sh = jce_renderer_get_program_mesh(r);
    bgfx_program_handle_t prog = { sh.idx };
    jce_enc_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

uint32_t jce_mesh_vertex_count(const JceMesh *mesh)
{
    return mesh ? mesh->num_verts : 0;
}

uint32_t jce_mesh_index_count(const JceMesh *mesh)
{
    return mesh ? mesh->num_indices : 0;
}

uint32_t jce_mesh_get_vbh(const JceMesh *mesh)
{
    return mesh ? (uint32_t)mesh->vbh.idx : (uint32_t)UINT16_MAX;
}

uint32_t jce_mesh_get_ibh(const JceMesh *mesh)
{
    return mesh ? (uint32_t)mesh->ibh.idx : (uint32_t)UINT16_MAX;
}

void jce_mesh_get_aabb(const JceMesh *mesh, float out_min[3], float out_max[3])
{
    if (!mesh) {
        if (out_min) { out_min[0] = out_min[1] = out_min[2] = 0.0f; }
        if (out_max) { out_max[0] = out_max[1] = out_max[2] = 0.0f; }
        return;
    }
    if (out_min) {
        out_min[0] = mesh->aabb_min[0];
        out_min[1] = mesh->aabb_min[1];
        out_min[2] = mesh->aabb_min[2];
    }
    if (out_max) {
        out_max[0] = mesh->aabb_max[0];
        out_max[1] = mesh->aabb_max[1];
        out_max[2] = mesh->aabb_max[2];
    }
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

    JceMeshVertex *verts = (JceMeshVertex *)JCE_MALLOC(num_verts * sizeof(JceMeshVertex));
    uint32_t *indices = (uint32_t *)JCE_MALLOC(num_indices * sizeof(uint32_t));
    if (!verts || !indices) {
        JCE_FREE(verts);
        JCE_FREE(indices);
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
    JCE_FREE(verts);
    JCE_FREE(indices);
    return mesh;
}

/* -- UV sphere ---------------------------------------------------- */

JceMesh *jce_mesh_create_sphere(float radius)
{
    const uint32_t rings  = 16;
    const uint32_t slices = 32;
    uint32_t num_verts   = (rings + 1) * (slices + 1);
    uint32_t num_indices = rings * slices * 6;

    JceMeshVertex *verts = (JceMeshVertex *)JCE_MALLOC(num_verts * sizeof(JceMeshVertex));
    uint32_t *indices    = (uint32_t *)JCE_MALLOC(num_indices * sizeof(uint32_t));
    if (!verts || !indices) { JCE_FREE(verts); JCE_FREE(indices); return NULL; }

    uint32_t vi = 0;
    for (uint32_t r = 0; r <= rings; r++) {
        float theta = JCE_PI * (float)r / (float)rings;
        float st    = sinf(theta);
        float ct    = cosf(theta);
        for (uint32_t s = 0; s <= slices; s++) {
            float phi = 2.0f * JCE_PI * (float)s / (float)slices;
            float sp  = sinf(phi);
            float cp  = cosf(phi);
            float nx  = st * cp;
            float ny  = ct;
            float nz  = st * sp;
            verts[vi].pos[0]    = radius * nx;
            verts[vi].pos[1]    = radius * ny;
            verts[vi].pos[2]    = radius * nz;
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            verts[vi].uv[0]     = (float)s / (float)slices;
            verts[vi].uv[1]     = (float)r / (float)rings;
            vi++;
        }
    }

    /* Winding: CCW seen from OUTSIDE (the engine-wide front-face
       convention — matches jce_mesh_create_cube and glTF). phi grows
       toward -screen-x on the camera-facing side, so the quad must be
       emitted (a, a+1, b) / (a+1, b+1, b); the previous (a, b, a+1)
       order was clockwise from outside — with the default CULL_CW
       state the sphere rendered INSIDE-OUT (front faces culled,
       interior visible) on every backend. */
    uint32_t ii = 0;
    for (uint32_t r = 0; r < rings; r++) {
        for (uint32_t s = 0; s < slices; s++) {
            uint32_t a = r * (slices + 1) + s;
            uint32_t b = a + slices + 1;
            indices[ii++] = a;
            indices[ii++] = a + 1;
            indices[ii++] = b;
            indices[ii++] = a + 1;
            indices[ii++] = b + 1;
            indices[ii++] = b;
        }
    }

    JceMesh *mesh = jce_mesh_create(verts, num_verts, indices, num_indices);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return mesh;
}

/* -- Capsule (hemisphere + cylinder + hemisphere) ----------------- */

JceMesh *jce_mesh_create_capsule(float radius, float height)
{
    const uint32_t hemi_rings = 8;
    const uint32_t slices     = 32;
    float half_h = (height - 2.0f * radius) * 0.5f;
    if (half_h < 0.0f) half_h = 0.0f;

    /* Top hemi (hemi_rings+1 rows) + bottom hemi (hemi_rings+1 rows)
       + 2 cylinder rows = total rows: 2*(hemi_rings+1) + 2.
       Simplified: top hemi rows 0..hemi_rings, bottom rows 0..hemi_rings,
       but they share the equator.  Total unique rows = 2*hemi_rings + 1. */
    uint32_t rows      = 2 * hemi_rings + 1;
    uint32_t num_verts = (rows + 1) * (slices + 1);
    uint32_t num_idx   = rows * slices * 6;

    JceMeshVertex *verts = (JceMeshVertex *)JCE_MALLOC(num_verts * sizeof(JceMeshVertex));
    uint32_t *indices    = (uint32_t *)JCE_MALLOC(num_idx * sizeof(uint32_t));
    if (!verts || !indices) { JCE_FREE(verts); JCE_FREE(indices); return NULL; }

    uint32_t vi = 0;
    for (uint32_t r = 0; r <= rows; r++) {
        float theta, y_off;
        if (r <= hemi_rings) {
            /* Top hemisphere: theta from 0 (top) to PI/2 (equator). */
            theta = (JCE_PI * 0.5f) * (float)r / (float)hemi_rings;
            y_off = half_h;
        } else {
            /* Bottom hemisphere: theta from PI/2 to PI (bottom). */
            uint32_t br = r - hemi_rings;
            theta = (JCE_PI * 0.5f) + (JCE_PI * 0.5f) * (float)br / (float)hemi_rings;
            y_off = -half_h;
        }
        float st = sinf(theta);
        float ct = cosf(theta);
        for (uint32_t s = 0; s <= slices; s++) {
            float phi = 2.0f * JCE_PI * (float)s / (float)slices;
            float nx  = st * cosf(phi);
            float ny  = ct;
            float nz  = st * sinf(phi);
            verts[vi].pos[0]    = radius * nx;
            verts[vi].pos[1]    = radius * ny + y_off;
            verts[vi].pos[2]    = radius * nz;
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            verts[vi].uv[0]     = (float)s / (float)slices;
            verts[vi].uv[1]     = (float)r / (float)rows;
            vi++;
        }
    }

    /* Winding: CCW from outside — same fix/rationale as
       jce_mesh_create_sphere above (the shared ring-grid pattern was
       emitted clockwise, rendering the capsule inside-out under the
       default CULL_CW state). */
    uint32_t ii = 0;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t s = 0; s < slices; s++) {
            uint32_t a = r * (slices + 1) + s;
            uint32_t b = a + slices + 1;
            indices[ii++] = a;
            indices[ii++] = a + 1;
            indices[ii++] = b;
            indices[ii++] = a + 1;
            indices[ii++] = b + 1;
            indices[ii++] = b;
        }
    }

    JceMesh *mesh = jce_mesh_create(verts, num_verts, indices, num_idx);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return mesh;
}

/* -- Cylinder ----------------------------------------------------- */

JceMesh *jce_mesh_create_cylinder(float radius, float height)
{
    const uint32_t slices = 32;
    const uint32_t rows   = 1;
    /* Body: 2 rows.  Top cap: center + rim.  Bottom cap: center + rim.
       Total verts: 2*(slices+1) + 2*(slices+1) + 2 = 4*(slices+1) + 2. */
    uint32_t body_verts = 2 * (slices + 1);
    uint32_t cap_verts  = slices + 1 + 1;  /* rim + center */
    uint32_t num_verts  = body_verts + 2 * cap_verts;
    uint32_t body_idx   = slices * 6;
    uint32_t cap_idx    = slices * 3;
    uint32_t num_idx    = body_idx + 2 * cap_idx;
    float hh = height * 0.5f;

    JceMeshVertex *verts = (JceMeshVertex *)JCE_MALLOC(num_verts * sizeof(JceMeshVertex));
    uint32_t *indices    = (uint32_t *)JCE_MALLOC(num_idx * sizeof(uint32_t));
    if (!verts || !indices) { JCE_FREE(verts); JCE_FREE(indices); return NULL; }

    uint32_t vi = 0, ii = 0;

    /* Body (tube). */
    for (uint32_t row = 0; row <= rows; row++) {
        float y = hh - height * (float)row / (float)rows;
        for (uint32_t s = 0; s <= slices; s++) {
            float phi = 2.0f * JCE_PI * (float)s / (float)slices;
            float nx  = cosf(phi);
            float nz  = sinf(phi);
            verts[vi].pos[0] = radius * nx;
            verts[vi].pos[1] = y;
            verts[vi].pos[2] = radius * nz;
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = 0.0f;
            verts[vi].normal[2] = nz;
            verts[vi].uv[0] = (float)s / (float)slices;
            verts[vi].uv[1] = (float)row / (float)rows;
            vi++;
        }
    }
    /* Winding: CCW from outside — same ring-grid fix as the sphere
       (tube faces were clockwise → culled, inner wall showed). */
    for (uint32_t s = 0; s < slices; s++) {
        uint32_t a = s;
        uint32_t b = a + slices + 1;
        indices[ii++] = a;
        indices[ii++] = a + 1;
        indices[ii++] = b;
        indices[ii++] = a + 1;
        indices[ii++] = b + 1;
        indices[ii++] = b;
    }

    /* Top cap. */
    uint32_t top_center = vi;
    verts[vi].pos[0] = 0; verts[vi].pos[1] = hh; verts[vi].pos[2] = 0;
    verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0;
    verts[vi].uv[0] = 0.5f; verts[vi].uv[1] = 0.5f;
    vi++;
    uint32_t top_rim = vi;
    for (uint32_t s = 0; s <= slices; s++) {
        float phi = 2.0f * JCE_PI * (float)s / (float)slices;
        verts[vi].pos[0] = radius * cosf(phi);
        verts[vi].pos[1] = hh;
        verts[vi].pos[2] = radius * sinf(phi);
        verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0;
        verts[vi].uv[0] = 0.5f + 0.5f * cosf(phi);
        verts[vi].uv[1] = 0.5f + 0.5f * sinf(phi);
        vi++;
    }
    /* Top cap winding: rim runs +X->+Z with growing phi, so CCW seen
       from ABOVE (+Y outward) is (center, rim+s+1, rim+s) — the old
       order faced the cap normal DOWN into the tube (cap invisible
       from above, its interior visible from below). */
    for (uint32_t s = 0; s < slices; s++) {
        indices[ii++] = top_center;
        indices[ii++] = top_rim + s + 1;
        indices[ii++] = top_rim + s;
    }

    /* Bottom cap. */
    uint32_t bot_center = vi;
    verts[vi].pos[0] = 0; verts[vi].pos[1] = -hh; verts[vi].pos[2] = 0;
    verts[vi].normal[0] = 0; verts[vi].normal[1] = -1; verts[vi].normal[2] = 0;
    verts[vi].uv[0] = 0.5f; verts[vi].uv[1] = 0.5f;
    vi++;
    uint32_t bot_rim = vi;
    for (uint32_t s = 0; s <= slices; s++) {
        float phi = 2.0f * JCE_PI * (float)s / (float)slices;
        verts[vi].pos[0] = radius * cosf(phi);
        verts[vi].pos[1] = -hh;
        verts[vi].pos[2] = radius * sinf(phi);
        verts[vi].normal[0] = 0; verts[vi].normal[1] = -1; verts[vi].normal[2] = 0;
        verts[vi].uv[0] = 0.5f + 0.5f * cosf(phi);
        verts[vi].uv[1] = 0.5f + 0.5f * sinf(phi);
        vi++;
    }
    /* Bottom cap: CCW seen from BELOW (-Y outward) is the mirror of
       the top cap — (center, rim+s, rim+s+1). */
    for (uint32_t s = 0; s < slices; s++) {
        indices[ii++] = bot_center;
        indices[ii++] = bot_rim + s;
        indices[ii++] = bot_rim + s + 1;
    }

    JceMesh *mesh = jce_mesh_create(verts, vi, indices, ii);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return mesh;
}
