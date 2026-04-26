/*
 * jce_skinned_mesh.c  Skinned GPU mesh implementation.
 */

#include <jce/middleware/animation/jce_skinned_mesh.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_renderer_internal.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "jce_skinned_mesh"

struct JceSkinnedMesh {
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
    bgfx_index_buffer_handle_t  wf_ibh;     /* wireframe line indices */
    bgfx_vertex_layout_t        layout;
    uint32_t num_verts;
    uint32_t num_indices;
    uint32_t num_wf_indices;
    bool     is_skinned;  /* true = JceSkinnedVertex, false = JcePbrVertex */
};

/* ================================================================== */
/* Vertex layout helpers                                               */
/* ================================================================== */

static void init_pbr_layout(bgfx_vertex_layout_t *layout)
{
    bgfx_vertex_layout_begin(layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_POSITION,  3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_NORMAL,    3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TANGENT,   4,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(layout);
}

static void init_skinned_layout(bgfx_vertex_layout_t *layout)
{
    bgfx_vertex_layout_begin(layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_POSITION,  3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_NORMAL,    3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TANGENT,   4,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_INDICES,   4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_WEIGHT,    4,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(layout);
}

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSkinnedMesh *jce_skinned_mesh_create(
    const JceSkinnedVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices)
{
    if (!vertices || num_verts == 0) return NULL;

    JceSkinnedMesh *m = (JceSkinnedMesh *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    init_skinned_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;
    m->is_skinned  = true;

    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JceSkinnedVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);

    if (indices && num_indices > 0) {
        const bgfx_memory_t *imem = bgfx_copy(indices,
                                               num_indices * (uint32_t)sizeof(uint32_t));
        m->ibh = bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);

        /* Build wireframe IBO: each triangle -> 3 line pairs. */
        uint32_t num_tris = num_indices / 3;
        uint32_t wf_count = num_tris * 6;
        uint32_t *wf = (uint32_t *)JCE_MALLOC(wf_count * sizeof(uint32_t));
        if (wf) {
            for (uint32_t t = 0; t < num_tris; t++) {
                uint32_t a = indices[t*3+0], b = indices[t*3+1], c = indices[t*3+2];
                wf[t*6+0]=a; wf[t*6+1]=b;
                wf[t*6+2]=b; wf[t*6+3]=c;
                wf[t*6+4]=c; wf[t*6+5]=a;
            }
            const bgfx_memory_t *wmem = bgfx_copy(wf, wf_count * (uint32_t)sizeof(uint32_t));
            m->wf_ibh = bgfx_create_index_buffer(wmem, BGFX_BUFFER_INDEX32);
            m->num_wf_indices = wf_count;
            JCE_FREE(wf);
        } else {
            m->wf_ibh.idx = UINT16_MAX;
        }
    } else {
        m->ibh.idx    = UINT16_MAX;
        m->wf_ibh.idx = UINT16_MAX;
    }

    LOG_DEBUG(LOG_TAG, "created skinned mesh: %u verts, %u indices",
              num_verts, num_indices);
    return m;
}

JceSkinnedMesh *jce_pbr_mesh_create(
    const JcePbrVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices)
{
    if (!vertices || num_verts == 0) return NULL;

    JceSkinnedMesh *m = (JceSkinnedMesh *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    init_pbr_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;
    m->is_skinned  = false;

    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JcePbrVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);

    if (indices && num_indices > 0) {
        const bgfx_memory_t *imem = bgfx_copy(indices,
                                               num_indices * (uint32_t)sizeof(uint32_t));
        m->ibh = bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);

        /* Build wireframe IBO. */
        uint32_t num_tris = num_indices / 3;
        uint32_t wf_count = num_tris * 6;
        uint32_t *wf = (uint32_t *)JCE_MALLOC(wf_count * sizeof(uint32_t));
        if (wf) {
            for (uint32_t t = 0; t < num_tris; t++) {
                uint32_t a = indices[t*3+0], b = indices[t*3+1], c = indices[t*3+2];
                wf[t*6+0]=a; wf[t*6+1]=b;
                wf[t*6+2]=b; wf[t*6+3]=c;
                wf[t*6+4]=c; wf[t*6+5]=a;
            }
            const bgfx_memory_t *wmem = bgfx_copy(wf, wf_count * (uint32_t)sizeof(uint32_t));
            m->wf_ibh = bgfx_create_index_buffer(wmem, BGFX_BUFFER_INDEX32);
            m->num_wf_indices = wf_count;
            JCE_FREE(wf);
        } else {
            m->wf_ibh.idx = UINT16_MAX;
        }
    } else {
        m->ibh.idx    = UINT16_MAX;
        m->wf_ibh.idx = UINT16_MAX;
    }

    LOG_DEBUG(LOG_TAG, "created PBR mesh: %u verts, %u indices",
              num_verts, num_indices);
    return m;
}

void jce_skinned_mesh_destroy(JceSkinnedMesh *mesh)
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

/* ================================================================== */
/* Submission                                                          */
/* ================================================================== */

void jce_skinned_mesh_submit(const JceSkinnedMesh *mesh,
                              const JceRenderer *r, uint16_t view_id)
{
    if (!mesh) return;
    (void)view_id;

    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (r && jce_renderer_get_wireframe(r) && mesh->wf_ibh.idx != UINT16_MAX) {
        /* Wireframe: line pairs from the wf_ibh, no back-face culling. */
        bgfx_set_index_buffer(mesh->wf_ibh, 0, mesh->num_wf_indices);
        bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                     | BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS
                     | BGFX_STATE_MSAA      | BGFX_STATE_PT_LINES, 0);
    } else {
        if (mesh->ibh.idx != UINT16_MAX)
            bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
        bgfx_set_state(BGFX_STATE_DEFAULT, 0);
    }
    /* Caller must call bgfx_submit after binding program via material. */
}

void jce_skinned_mesh_set_bones(const jce_mat4 *joint_matrices,
                                 uint32_t num_joints)
{
    if (!joint_matrices || num_joints == 0) return;
    bgfx_set_transform(joint_matrices->raw[0], (uint16_t)num_joints);
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

bool jce_skinned_mesh_is_skinned(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->is_skinned : false;
}

uint32_t jce_skinned_mesh_vertex_count(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->num_verts : 0;
}

uint32_t jce_skinned_mesh_index_count(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->num_indices : 0;
}
