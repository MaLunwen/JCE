/*
 * jce_skinned_mesh.c  Skinned GPU mesh implementation.
 */

#include "renderer/jce_vertex_layout.h"
#include <jce/renderer/jce_skinned_mesh.h>
#include "renderer/jce_render_encoder.h"   /* jce_dbg_xform_matrices */
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

    /* Nanite-lite V1 meshlet sidecar: a meshlet-GROUPED alternate index buffer
     * (each cluster's triangles contiguous; global vertex ids share vbh) plus
     * the per-meshlet cull data as a static COMPUTE_READ buffer — 3 vec4 per
     * meshlet: {index_offset(bits), index_count(bits), 0, 0},
     * {sphere cx,cy,cz,r}, {cone ax,ay,az,cutoff}.  ml_count == 0 (default) =
     * no sidecar, zero extra GPU memory. */
    bgfx_index_buffer_handle_t  ml_ibh;
    uint32_t                    ml_num_indices;
    bgfx_vertex_buffer_handle_t ml_data;
    uint32_t                    ml_count;

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
    jce_vertex_layout_add_pbr_base(layout);
    bgfx_vertex_layout_end(layout);
}

static void init_skinned_layout(bgfx_vertex_layout_t *layout)
{
    bgfx_vertex_layout_begin(layout, bgfx_get_renderer_type());
    jce_vertex_layout_add_pbr_base(layout);
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
    if (mesh->ml_count) {   /* Nanite-lite V1 meshlet sidecar */
        bgfx_destroy_index_buffer(mesh->ml_ibh);
        bgfx_destroy_vertex_buffer(mesh->ml_data);
    }
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
/* WIDENED from a bool to the whole state, because double_sided was only the
 * first bit this path dropped.  A glTF model's ALPHA MODE and BLEND EQUATION
 * were dropped the same way and for the same reason: the material's factors
 * reached the draw through jce_pbr_material_bind while its render state
 * reached nothing, so a model could not be transparent no matter what was
 * authored.  Measured 2026-09-06: four pine models with alphaMode BLEND and
 * alpha 0.6 rendered fully opaque and correctly TINTED by their base colours
 * -- factors arriving while the state did not is exactly that picture.
 *
 * 0 means "the caller said nothing", not "no state": that is what keeps a
 * model nobody has authored a material for byte-identical. */
/* ONE record, not a global apiece: the two are set together, applied
 * together, and cleared together, and splitting them would let a caller set
 * half of what the next submit uses. */
static struct { uint64_t state; uint32_t stencil;     /* Static-batch member culling; see the header. */
    uint32_t range_first;
    uint32_t range_count;
} s_submit = { 0, 0 };

/* Apply both.  bgfx resets the stencil after each submit, so this runs per
 * submit, not once; skipped when zero because zero is exactly what that reset
 * leaves behind. */
static void sm_apply_submit_state(void)
{
    bgfx_set_state(s_submit.state ? s_submit.state : BGFX_STATE_DEFAULT, 0);
    if (s_submit.stencil) bgfx_set_stencil(s_submit.stencil, BGFX_STENCIL_NONE);
}

void jce_skinned_mesh_set_submit_double_sided(bool on)
{
    /* Kept because it is public and because it is the narrow question most
     * callers ask.  Expressed through the same global, so there is exactly
     * one thing to be stale. */
    s_submit.state = on ? (BGFX_STATE_DEFAULT & ~BGFX_STATE_CULL_MASK)
                        : BGFX_STATE_DEFAULT;
    s_submit.stencil = 0;
}

void jce_skinned_mesh_set_submit_state(uint64_t state)
{
    s_submit.state = state;
    s_submit.stencil = 0;   /* see the header: state-only callers get no stencil */
}

void jce_skinned_mesh_set_submit_stencil(uint32_t stencil)
{
    s_submit.stencil = stencil;
}

void jce_skinned_mesh_set_submit_index_range(uint32_t first, uint32_t count)
{
    s_submit.range_first = first;
    s_submit.range_count = count;
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
        if (mesh->ibh.idx != UINT16_MAX) {
            /* An armed range draws that slice; no range draws the whole
             * buffer, which is what every caller but static-batch member
             * culling does and is byte-identical to the historical submit.
             * Clamped rather than trusted: a member table from a sidecar that
             * no longer matches its .glb would otherwise index past the end,
             * and bgfx's answer to that is not a message anyone can act on. */
            uint32_t first = 0, count = mesh->num_indices;
            if (s_submit.range_count > 0 &&
                s_submit.range_first < mesh->num_indices) {
                first = s_submit.range_first;
                count = s_submit.range_count;
                if (first + count > mesh->num_indices)
                    count = mesh->num_indices - first;
            }
            bgfx_set_index_buffer(mesh->ibh, first, count);
        }
        /* The caller's state when it stated one (two-sided, alpha blend,
         * additive, ...), the legacy default when it did not. */
        sm_apply_submit_state();
    }
    /* DISARMED BY THE SUBMIT, on both branches: a range left armed would
     * truncate the next unrelated mesh, and that failure is a hole in the
     * world one draw later with nothing pointing back here. */
    s_submit.range_first = 0;
    s_submit.range_count = 0;
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
        sm_apply_submit_state();
    }
    /* Caller submits the program (matches jce_skinned_mesh_submit contract). */
    JCE_PROFILE_ZONE_END;
}

void jce_skinned_mesh_set_bones(const jce_mat4 *joint_matrices,
                                 uint32_t num_joints)
{
    if (!joint_matrices || num_joints == 0) return;
    /* Matrix-cache pressure: bone palettes are the whale consumer of bgfx's
     * fixed per-frame matrix cache (~24-128 matrices per skinned draw); the
     * counter feeds the silent-saturation warning in jce_renderer_end_frame. */
    jce_dbg_xform_matrices += num_joints;
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

void jce_skinned_mesh_bind_shadow(const JceSkinnedMesh *mesh)
{
    if (!mesh) return;
    /* Depth-only bind (no submit) for an INSTANCED shadow draw: the caller
     * follows with bgfx_set_instance_data_buffer + bgfx_submit(shadow program).
     * Unlike jce_skinned_mesh_submit (the color binder) this has NO wireframe
     * branch, so an instanced depth submit never inherits the line index buffer
     * + PT_LINES under the editor's wireframe view — it always binds the
     * TRIANGLE index buffer, matching jce_skinned_mesh_submit_shadow. */
    bgfx_set_vertex_buffer(0, mesh->vbh, 0, mesh->num_verts);
    if (mesh->ibh.idx != UINT16_MAX)
        bgfx_set_index_buffer(mesh->ibh, 0, mesh->num_indices);
    bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
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

uint32_t jce_skinned_mesh_get_vbh(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->vbh.idx : UINT16_MAX;
}

uint32_t jce_skinned_mesh_get_ibh(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->ibh.idx : UINT16_MAX;
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

/* Nanite-lite V1/V3: install the meshlet sidecar — the meshlet-grouped
 * alternate index buffer + the per-meshlet cull data packed as 6 vec4 per
 * meshlet into a static COMPUTE_READ buffer the cluster-cull compute reads:
 *   vec4[0] = {asfloat(index_offset), asfloat(index_count), 0, 0}
 *   vec4[1] = {sphere cx, cy, cz, r}          (mesh-local space)
 *   vec4[2] = {cone axis x, y, z, cutoff}     (meshopt convention)
 *   vec4[3] = {own_error, parent_error, 0, 0} (V3 LOD DAG cut; object units)
 *   vec4[4] = {own-group sphere cx,cy,cz,r}   (own-error test distance)
 *   vec4[5] = {parent-group sphere cx,cy,cz,r}(parent-error test distance)
 * `errors` (10 f32/meshlet: {own, parent, own-group xyzr, parent-group
 * xyzr}) may be NULL: clusters then carry {0, +BIG, cluster sphere x2} —
 * "leaf with no parent", which the cut test always draws (== V2 behaviour).
 * A child's parent-test and its parent's own-test share the IDENTICAL
 * (error, sphere) pair, making the runtime cut exactly complementary.
 * Returns the installed meshlet count (0 = rejected/failed). */
uint32_t jce_skinned_mesh_set_meshlets(JceSkinnedMesh *mesh,
                                       const uint32_t *indices,
                                       uint32_t num_indices,
                                       const uint32_t *desc,
                                       const float *bounds,
                                       const float *errors,
                                       uint32_t count)
{
    if (!mesh || !indices || !desc || !bounds || count == 0u ||
        num_indices < 3u || mesh->ml_count != 0u)
        return 0;
    for (uint32_t i = 0; i < num_indices; ++i)
        if (indices[i] >= mesh->num_verts) return 0;

    const bgfx_memory_t *imem =
        bgfx_copy(indices, num_indices * (uint32_t)sizeof(uint32_t));
    bgfx_index_buffer_handle_t ibh =
        bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);
    if (ibh.idx == UINT16_MAX) return 0;

    /* Pack the GPU cull records (24 floats per meshlet). */
    float *rec = (float *)JCE_MALLOC((size_t)count * 24u * sizeof(float));
    if (!rec) { bgfx_destroy_index_buffer(ibh); return 0; }
    for (uint32_t m = 0; m < count; ++m) {
        union { uint32_t u; float f; } off, cnt;
        off.u = desc[m * 2u];
        cnt.u = desc[m * 2u + 1u];
        float *d = rec + (size_t)m * 24u;
        d[0] = off.f; d[1] = cnt.f; d[2] = 0.0f; d[3] = 0.0f;
        memcpy(d + 4, bounds + (size_t)m * 8u, 8u * sizeof(float));
        if (errors) {
            d[12] = errors[m * 10u];
            d[13] = errors[m * 10u + 1u];
            d[14] = 0.0f; d[15] = 0.0f;
            memcpy(d + 16, errors + (size_t)m * 10u + 2u, 8u * sizeof(float));
        } else {
            d[12] = 0.0f; d[13] = 1.0e30f; d[14] = 0.0f; d[15] = 0.0f;
            memcpy(d + 16, bounds + (size_t)m * 8u, 4u * sizeof(float));
            memcpy(d + 20, bounds + (size_t)m * 8u, 4u * sizeof(float));
        }
    }
    bgfx_vertex_layout_t vl;
    bgfx_vertex_layout_begin(&vl, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD1, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD2, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD3, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&vl);
    const bgfx_memory_t *dmem =
        bgfx_copy(rec, count * 24u * (uint32_t)sizeof(float));
    JCE_FREE(rec);
    bgfx_vertex_buffer_handle_t data = bgfx_create_vertex_buffer(dmem, &vl,
        BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X4 |
        BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (data.idx == UINT16_MAX) { bgfx_destroy_index_buffer(ibh); return 0; }

    mesh->ml_ibh         = ibh;
    mesh->ml_num_indices = num_indices;
    mesh->ml_data        = data;
    mesh->ml_count       = count;
    return count;
}

uint32_t jce_skinned_mesh_meshlet_count(const JceSkinnedMesh *mesh)
{
    return mesh ? mesh->ml_count : 0;
}

uint32_t jce_skinned_mesh_meshlet_ibh(const JceSkinnedMesh *mesh)
{
    return (mesh && mesh->ml_count) ? mesh->ml_ibh.idx : UINT16_MAX;
}

uint32_t jce_skinned_mesh_meshlet_data_vb(const JceSkinnedMesh *mesh)
{
    return (mesh && mesh->ml_count) ? mesh->ml_data.idx : UINT16_MAX;
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

uint32_t jce_skinned_mesh_lod_ibh(const JceSkinnedMesh *mesh, uint32_t level)
{
    if (!mesh) return UINT16_MAX;
    if (level < mesh->lod_count) return mesh->lod_ibh[level].idx;
    return mesh->ibh.idx;       /* out of range → base index buffer (LOD0) */
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
    sm_apply_submit_state();
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
