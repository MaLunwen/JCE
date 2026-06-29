/*
 * jce_skinned_mesh.c  Skinned GPU mesh implementation.
 */

#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_renderer_internal.h"

#include <bgfx/c99/bgfx.h>
#include <stddef.h>
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

    /* In-asset auto-LOD index buffers (large-world-opt P1 #6).  Each level
     * shares THIS mesh's vertex buffer (vbh) and binds a reduced index set so a
     * distant entity draws fewer triangles without a separate VB or model file.
     * lod_count == 0 (the default) keeps the legacy single-IBO behaviour with
     * zero extra GPU memory.  Levels are stored 0-based here (lod_ibh[0] is the
     * FIRST reduced level, i.e. glTF JCE_lod indices[0] / "LOD1"). */
    bgfx_index_buffer_handle_t  lod_ibh[JCE_SM_MAX_LOD];
    uint32_t                    lod_num_indices[JCE_SM_MAX_LOD];
    uint32_t                    lod_count;

    /* Morph deform support (FEATURE 3.1, opt-in via retain_cpu).  When
     * non-NULL, holds an undeformed CPU copy of the source vertex array
     * (num_verts * stride bytes) so a per-instance morph can produce a
     * deformed copy without touching the shared static VB.  NULL (the default)
     * = zero extra RAM, byte-identical legacy behavior. */
    void    *base_cpu;     /* owned; NULL unless retain_cpu requested */
    uint32_t stride;       /* byte stride of the retained vertex format */
    uint32_t pos_offset;   /* byte offset of POSITION float[3] */
    uint32_t normal_offset;/* byte offset of NORMAL float[3] */
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
    const uint32_t *indices, uint32_t num_indices,
    bool retain_cpu)
{
    if (!vertices || num_verts == 0) return NULL;

    JceSkinnedMesh *m = (JceSkinnedMesh *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    init_skinned_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;
    m->is_skinned  = true;

    /* Morph deform: optionally retain an undeformed CPU copy of the source
     * verts (FEATURE 3.1).  Gated to morph-bearing prims by the caller, so the
     * default path allocates nothing.  pos@0 / normal@12 match the layout. */
    if (retain_cpu) {
        uint32_t bytes = num_verts * (uint32_t)sizeof(JceSkinnedVertex);
        m->base_cpu = JCE_MALLOC(bytes);
        if (m->base_cpu) {
            memcpy(m->base_cpu, vertices, bytes);
            m->stride        = (uint32_t)sizeof(JceSkinnedVertex);
            m->pos_offset    = (uint32_t)offsetof(JceSkinnedVertex, pos);
            m->normal_offset = (uint32_t)offsetof(JceSkinnedVertex, normal);
        }
    }

    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JceSkinnedVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);
    if (m->vbh.idx == UINT16_MAX) {
        /* bgfx static vertex-buffer pool exhausted (or upload failed): fail the
         * whole mesh instead of returning a zombie with an invalid vbh that
         * would forever submit nothing. Degrades gracefully under handle
         * pressure rather than feeding a broken handle into the draw list. */
        LOG_WARN(LOG_TAG, "skinned mesh: vertex buffer allocation failed");
        if (m->base_cpu) JCE_FREE(m->base_cpu);
        JCE_FREE(m);
        return NULL;
    }

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
    const uint32_t *indices, uint32_t num_indices,
    bool retain_cpu)
{
    if (!vertices || num_verts == 0) return NULL;

    JceSkinnedMesh *m = (JceSkinnedMesh *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    init_pbr_layout(&m->layout);
    m->num_verts   = num_verts;
    m->num_indices = num_indices;
    m->is_skinned  = false;

    /* Morph deform: optionally retain an undeformed CPU copy (FEATURE 3.1).
     * JcePbrVertex has the same pos@0 / normal@12 offsets as the skinned
     * vertex, but a different (smaller) stride. */
    if (retain_cpu) {
        uint32_t bytes = num_verts * (uint32_t)sizeof(JcePbrVertex);
        m->base_cpu = JCE_MALLOC(bytes);
        if (m->base_cpu) {
            memcpy(m->base_cpu, vertices, bytes);
            m->stride        = (uint32_t)sizeof(JcePbrVertex);
            m->pos_offset    = (uint32_t)offsetof(JcePbrVertex, pos);
            m->normal_offset = (uint32_t)offsetof(JcePbrVertex, normal);
        }
    }

    const bgfx_memory_t *vmem = bgfx_copy(vertices,
                                           num_verts * (uint32_t)sizeof(JcePbrVertex));
    m->vbh = bgfx_create_vertex_buffer(vmem, &m->layout, BGFX_BUFFER_NONE);
    if (m->vbh.idx == UINT16_MAX) {
        /* See jce_skinned_mesh_create: fail cleanly on pool exhaustion rather
         * than return a zombie mesh with an invalid vertex buffer. */
        LOG_WARN(LOG_TAG, "PBR mesh: vertex buffer allocation failed");
        if (m->base_cpu) JCE_FREE(m->base_cpu);
        JCE_FREE(m);
        return NULL;
    }

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
    for (uint32_t l = 0; l < mesh->lod_count; ++l)
        if (mesh->lod_ibh[l].idx != UINT16_MAX)
            bgfx_destroy_index_buffer(mesh->lod_ibh[l]);
    if (mesh->base_cpu)
        JCE_FREE(mesh->base_cpu);   /* retained morph base (NULL unless retained) */
    JCE_FREE(mesh);
}

/* ================================================================== */
/* Submission                                                          */
/* ================================================================== */

/* Per-submit double-sided override (render-thread only, mirrors jce_model.c's
 * s_material_override pattern).  jce_model_draw* sets it from the bound
 * material's double_sided flag before each color submit so a two-sided material
 * actually disables back-face culling here — without it, this path hard-coded
 * BGFX_STATE_DEFAULT (CULL_CW) and silently ignored double_sided, so any model
 * whose visible faces are wound as back-faces (e.g. the gen_hlod far-proxy
 * boxes) rendered only ambient-lit underside/back-faces => a flat, dark,
 * colourless mass.  Default false => byte-identical to the legacy single-sided
 * behaviour for every model that doesn't author double_sided. */
static bool s_submit_double_sided = false;

void jce_skinned_mesh_set_submit_double_sided(bool on)
{
    s_submit_double_sided = on;
}

void jce_skinned_mesh_submit(const JceSkinnedMesh *mesh,
                              const JceRenderer *r, uint16_t view_id)
{
    if (!mesh) return;
    JCE_PROFILE_ZONE_N("SkinnedMesh::Submit");
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
        /* Two-sided materials drop the CULL_CW bit so both winding orders draw. */
        uint64_t state = BGFX_STATE_DEFAULT;
        if (s_submit_double_sided) state &= ~BGFX_STATE_CULL_MASK;
        bgfx_set_state(state, 0);
    }
    /* Caller must call bgfx_submit after binding program via material. */
    JCE_PROFILE_ZONE_END;
}

/* Morph color submit: byte-identical to jce_skinned_mesh_submit except the
 * vertex source is the per-instance dynamic VB (dyn_vb_idx).  Caller (the
 * model draw) submits the program afterward, exactly as for the static path. */
void jce_skinned_mesh_submit_morphed(const JceSkinnedMesh *mesh,
                                     const JceRenderer *r, uint16_t view_id,
                                     uint16_t dyn_vb_idx)
{
    if (!mesh) return;
    if (dyn_vb_idx == UINT16_MAX) {
        /* No live morph VB -> literal fall-through to the static submit. */
        jce_skinned_mesh_submit(mesh, r, view_id);
        return;
    }
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitMorphed");
    (void)view_id;

    bgfx_dynamic_vertex_buffer_handle_t dvb = { dyn_vb_idx };
    bgfx_set_dynamic_vertex_buffer(0, dvb, 0, mesh->num_verts);

    if (r && jce_renderer_get_wireframe(r) && mesh->wf_ibh.idx != UINT16_MAX) {
        bgfx_set_index_buffer(mesh->wf_ibh, 0, mesh->num_wf_indices);
        bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                     | BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS
                     | BGFX_STATE_MSAA      | BGFX_STATE_PT_LINES, 0);
    } else {
        if (mesh->ibh.idx != UINT16_MAX)
            bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
        uint64_t state = BGFX_STATE_DEFAULT;
        if (s_submit_double_sided) state &= ~BGFX_STATE_CULL_MASK;
        bgfx_set_state(state, 0);
    }
    /* Caller submits the program (matches jce_skinned_mesh_submit contract). */
    JCE_PROFILE_ZONE_END;
}

void jce_skinned_mesh_set_bones(const jce_mat4 *joint_matrices,
                                 uint32_t num_joints)
{
    if (!joint_matrices || num_joints == 0) return;
    bgfx_set_transform(joint_matrices->raw[0], (uint16_t)num_joints);
}

void jce_skinned_mesh_submit_shadow(const JceSkinnedMesh *mesh,
                                    const JceRenderer *r, uint16_t view_id,
                                    JceShaderHandle program)
{
    if (!mesh || !r || program.idx == UINT16_MAX) return;
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitShadow");
    (void)view_id;

    /* Caller must have uploaded the bone palette via
     * jce_skinned_mesh_set_bones() (skinned program) or a single
     * bgfx_set_transform() (static-PBR fallback) before this call. */
    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    /* Depth-only: write Z, cull front faces to reduce peter-panning —
     * identical state to jce_mesh_submit_shadow() for the static path. */
    bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);

    bgfx_program_handle_t prog = { program.idx };
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

/* Morph shadow submit: mirror of jce_skinned_mesh_submit_shadow that binds the
 * per-instance dynamic VB so the cast silhouette matches the morphed mesh.
 * UINT16_MAX => fall through to the static shadow submit. */
void jce_skinned_mesh_submit_shadow_morphed(const JceSkinnedMesh *mesh,
                                            const JceRenderer *r, uint16_t view_id,
                                            JceShaderHandle program,
                                            uint16_t dyn_vb_idx)
{
    if (!mesh || !r || program.idx == UINT16_MAX) return;
    if (dyn_vb_idx == UINT16_MAX) {
        jce_skinned_mesh_submit_shadow(mesh, r, view_id, program);
        return;
    }
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitShadowMorphed");
    (void)view_id;

    /* Caller must have uploaded the bone palette (skinned program) or a single
     * bgfx_set_transform() (static-PBR fallback) before this call. */
    bgfx_dynamic_vertex_buffer_handle_t dvb = { dyn_vb_idx };
    bgfx_set_dynamic_vertex_buffer(0, dvb, 0, mesh->num_verts);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);

    bgfx_program_handle_t prog = { program.idx };
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

/* Wireframe overlay: line-topology submit using the per-mesh wf_ibh.
 * For skinned variants the caller must have uploaded the bone palette
 * via jce_skinned_mesh_set_bones() (or bgfx_set_transform for static
 * PBR variants).  Uses the PBR shader matching the layout so the
 * vertex stage produces correctly skinned positions; fragment output
 * is overwritten by the line rasterizer, so colour is irrelevant. */
void jce_skinned_mesh_submit_wireframe_overlay(const JceSkinnedMesh *mesh,
                                                const JceRenderer *r,
                                                uint16_t view_id)
{
    if (!mesh || !r) return;
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitWireframeOverlay");

    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->wf_ibh.idx != UINT16_MAX) {
        bgfx_set_index_buffer(mesh->wf_ibh, 0, mesh->num_wf_indices);
    } else if (mesh->ibh.idx != UINT16_MAX) {
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
    }

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LEQUAL
                   | BGFX_STATE_MSAA | BGFX_STATE_PT_LINES;
    if (mesh->num_wf_indices < 1500000u)
        state |= BGFX_STATE_LINEAA;

    bgfx_set_state(state, 0);

    JceShaderHandle sh = mesh->is_skinned
        ? jce_renderer_get_program_pbr_skinned(r)
        : jce_renderer_get_program_pbr(r);
    bgfx_program_handle_t prog = { sh.idx };
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

    JCE_PROFILE_ZONE_END;
}

void jce_skinned_mesh_submit_pick_id(const JceSkinnedMesh *mesh,
                                      const JceRenderer *r,
                                      uint16_t view_id,
                                      JceShaderHandle program,
                                      bool double_sided)
{
    if (!mesh || !r || program.idx == UINT16_MAX)
        return;
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitPickID");

    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);

    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                     BGFX_STATE_MSAA;
    if (!double_sided)
        state |= BGFX_STATE_CULL_CW;
    bgfx_set_state(state, 0);

    bgfx_program_handle_t prog = { program.idx };
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

    JCE_PROFILE_ZONE_END;
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

/* ================================================================== */
/* In-asset auto-LOD (large-world-opt P1 #6)                            */
/* ================================================================== */

uint32_t jce_skinned_mesh_add_lod(JceSkinnedMesh *mesh,
                                  const uint32_t *indices,
                                  uint32_t num_indices)
{
    if (!mesh) return 0;
    if (!indices || num_indices < 3 || (num_indices % 3) != 0)
        return mesh->lod_count;
    if (mesh->lod_count >= JCE_SM_MAX_LOD)
        return mesh->lod_count;
    /* Every LOD index must address the shared base vertex buffer. */
    for (uint32_t i = 0; i < num_indices; ++i)
        if (indices[i] >= mesh->num_verts)
            return mesh->lod_count;

    const bgfx_memory_t *imem =
        bgfx_copy(indices, num_indices * (uint32_t)sizeof(uint32_t));
    bgfx_index_buffer_handle_t ibh =
        bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);
    if (ibh.idx == UINT16_MAX) return mesh->lod_count;  /* pool exhausted: skip */

    uint32_t l = mesh->lod_count;
    mesh->lod_ibh[l]         = ibh;
    mesh->lod_num_indices[l] = num_indices;
    mesh->lod_count          = l + 1;
    return mesh->lod_count;
}

uint32_t jce_skinned_mesh_lod_count(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->lod_count : 0;
}

uint32_t jce_skinned_mesh_lod_index_count(const JceSkinnedMesh *mesh,
                                          uint32_t level)
{
    if (!mesh) return 0;
    if (level < mesh->lod_count) return mesh->lod_num_indices[level];
    return mesh->num_indices;   /* out of range → quote the base count */
}

void jce_skinned_mesh_submit_lod(const JceSkinnedMesh *mesh,
                                 const JceRenderer *r, uint16_t view_id,
                                 uint32_t level)
{
    if (!mesh) return;
    /* No LOD for this level, or wireframe view (must keep its base wf_ibh):
     * fall through to the base submit so behaviour is byte-identical. */
    if (level >= mesh->lod_count || mesh->lod_ibh[level].idx == UINT16_MAX ||
        (r && jce_renderer_get_wireframe(r))) {
        jce_skinned_mesh_submit(mesh, r, view_id);
        return;
    }
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitLod");
    (void)view_id;

    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);
    bgfx_set_index_buffer(mesh->lod_ibh[level], 0, mesh->lod_num_indices[level]);
    uint64_t state = BGFX_STATE_DEFAULT;
    if (s_submit_double_sided) state &= ~BGFX_STATE_CULL_MASK;
    bgfx_set_state(state, 0);
    /* Caller submits the program after binding the material (same contract as
     * jce_skinned_mesh_submit). */
    JCE_PROFILE_ZONE_END;
}

void jce_skinned_mesh_submit_shadow_lod(const JceSkinnedMesh *mesh,
                                        const JceRenderer *r, uint16_t view_id,
                                        JceShaderHandle program, uint32_t level)
{
    if (!mesh || !r || program.idx == UINT16_MAX) return;
    if (level >= mesh->lod_count || mesh->lod_ibh[level].idx == UINT16_MAX) {
        jce_skinned_mesh_submit_shadow(mesh, r, view_id, program);
        return;
    }
    JCE_PROFILE_ZONE_N("SkinnedMesh::SubmitShadowLod");
    (void)view_id;

    /* Caller uploaded the bone palette / transform before this call. */
    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);
    bgfx_set_index_buffer(mesh->lod_ibh[level], 0, mesh->lod_num_indices[level]);
    bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
    bgfx_program_handle_t prog = { program.idx };
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Morph deform accessors (FEATURE 3.1)                                */
/* ================================================================== */

const void *jce_skinned_mesh_base_verts(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->base_cpu : NULL;
}

uint32_t jce_skinned_mesh_stride(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->stride : 0;
}

uint32_t jce_skinned_mesh_pos_offset(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->pos_offset : 0;
}

uint32_t jce_skinned_mesh_normal_offset(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->normal_offset : 0;
}

const void *jce_skinned_mesh_layout(const JceSkinnedMesh *mesh)
{
    /* Only meaningful when a CPU copy was retained (caller uses the layout to
     * create the matching dynamic VB); return NULL otherwise to mirror the
     * other morph accessors' "not retained" contract. */
    return (mesh && mesh->base_cpu) ? &mesh->layout : NULL;
}
