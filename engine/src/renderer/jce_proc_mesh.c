/*
 * jce_proc_mesh.c  Procedural mesh builder.
 *
 * Channel storage uses malloc — each setter grows the channel
 * capacity geometrically.  Uploading to GPU is the caller's job
 * (B17.x wire-up); this file only owns CPU buffers + dirty bits.
 */

#include <jce/renderer/jce_proc_mesh.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

void jce_proc_mesh_init(JceProcMesh *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->topology = JCE_PROC_MESH_TOPO_TRIANGLES;
}

void jce_proc_mesh_clear(JceProcMesh *m)
{
    if (!m) return;
    m->vertex_count = 0;
    m->index_count  = 0;
    m->dirty_flags  = JCE_PROC_MESH_DIRTY_ALL;
}

void jce_proc_mesh_dispose(JceProcMesh *m)
{
    if (!m) return;
    free(m->positions); m->positions = NULL;
    free(m->normals);   m->normals   = NULL;
    free(m->uvs);       m->uvs       = NULL;
    free(m->colors);    m->colors    = NULL;
    free(m->indices);   m->indices   = NULL;
    m->vertex_count    = 0;
    m->vertex_capacity = 0;
    m->index_count     = 0;
    m->index_capacity  = 0;
    m->dirty_flags     = 0;
}

void jce_proc_mesh_set_topology(JceProcMesh *m, JceProcMeshTopology t)
{
    if (m) m->topology = t;
}

void jce_proc_mesh_mark_all_dirty(JceProcMesh *m)
{
    if (m) m->dirty_flags = JCE_PROC_MESH_DIRTY_ALL;
}

static bool ensure_vertex_capacity(JceProcMesh *m, uint32_t needed)
{
    if (m->vertex_capacity >= needed) return true;
    uint32_t cap = m->vertex_capacity ? m->vertex_capacity : 64;
    while (cap < needed) cap *= 2;
    /* Grow positions + the channels that have already been allocated.
     * Other channels stay NULL until their setter runs. */
    float    *p = (float *)realloc(m->positions, cap * 3 * sizeof(float));
    if (!p) return false;
    m->positions = p;
    if (m->normals) {
        float *q = (float *)realloc(m->normals, cap * 3 * sizeof(float));
        if (!q) return false;
        m->normals = q;
    }
    if (m->uvs) {
        float *q = (float *)realloc(m->uvs, cap * 2 * sizeof(float));
        if (!q) return false;
        m->uvs = q;
    }
    if (m->colors) {
        uint32_t *q = (uint32_t *)realloc(m->colors, cap * sizeof(uint32_t));
        if (!q) return false;
        m->colors = q;
    }
    m->vertex_capacity = cap;
    return true;
}

static bool ensure_index_capacity(JceProcMesh *m, uint32_t needed)
{
    if (m->index_capacity >= needed) return true;
    uint32_t cap = m->index_capacity ? m->index_capacity : 128;
    while (cap < needed) cap *= 2;
    uint32_t *p = (uint32_t *)realloc(m->indices, cap * sizeof(uint32_t));
    if (!p) return false;
    m->indices = p;
    m->index_capacity = cap;
    return true;
}

bool jce_proc_mesh_set_vertices(JceProcMesh *m, const float *xyz,
                                 uint32_t count)
{
    if (!m || (!xyz && count > 0)) return false;
    if (!ensure_vertex_capacity(m, count)) return false;
    if (count > 0) memcpy(m->positions, xyz, count * 3 * sizeof(float));
    m->vertex_count = count;
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_VERTICES;
    return true;
}

bool jce_proc_mesh_set_normals(JceProcMesh *m, const float *xyz,
                                uint32_t count)
{
    if (!m) return false;
    if (count > m->vertex_count) m->vertex_count = count;
    if (!ensure_vertex_capacity(m, count)) return false;
    if (!m->normals) {
        m->normals = (float *)calloc(m->vertex_capacity * 3, sizeof(float));
        if (!m->normals) return false;
    }
    if (count > 0 && xyz) memcpy(m->normals, xyz, count * 3 * sizeof(float));
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_NORMALS;
    return true;
}

bool jce_proc_mesh_set_uvs(JceProcMesh *m, const float *uv, uint32_t count)
{
    if (!m) return false;
    if (count > m->vertex_count) m->vertex_count = count;
    if (!ensure_vertex_capacity(m, count)) return false;
    if (!m->uvs) {
        m->uvs = (float *)calloc(m->vertex_capacity * 2, sizeof(float));
        if (!m->uvs) return false;
    }
    if (count > 0 && uv) memcpy(m->uvs, uv, count * 2 * sizeof(float));
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_UVS;
    return true;
}

bool jce_proc_mesh_set_colors(JceProcMesh *m, const uint32_t *rgba,
                               uint32_t count)
{
    if (!m) return false;
    if (count > m->vertex_count) m->vertex_count = count;
    if (!ensure_vertex_capacity(m, count)) return false;
    if (!m->colors) {
        m->colors = (uint32_t *)calloc(m->vertex_capacity, sizeof(uint32_t));
        if (!m->colors) return false;
    }
    if (count > 0 && rgba) memcpy(m->colors, rgba, count * sizeof(uint32_t));
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_COLORS;
    return true;
}

bool jce_proc_mesh_set_triangles(JceProcMesh *m, const uint32_t *indices,
                                  uint32_t count)
{
    if (!m || (!indices && count > 0)) return false;
    if (!ensure_index_capacity(m, count)) return false;
    if (count > 0) memcpy(m->indices, indices, count * sizeof(uint32_t));
    m->index_count = count;
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_INDICES;
    return true;
}

static void normalize3(float *v)
{
    float L = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (L > 1e-6f) { v[0] /= L; v[1] /= L; v[2] /= L; }
}

bool jce_proc_mesh_recalc_normals(JceProcMesh *m)
{
    if (!m || !m->positions || m->vertex_count == 0) return false;
    if (!m->indices  || m->index_count < 3)  return false;
    if (!m->normals) {
        m->normals = (float *)calloc(m->vertex_capacity * 3, sizeof(float));
        if (!m->normals) return false;
    } else {
        memset(m->normals, 0, m->vertex_count * 3 * sizeof(float));
    }
    /* Accumulate face normals into vertex normals. */
    for (uint32_t i = 0; i + 2 < m->index_count; i += 3) {
        uint32_t a = m->indices[i + 0];
        uint32_t b = m->indices[i + 1];
        uint32_t c = m->indices[i + 2];
        if (a >= m->vertex_count || b >= m->vertex_count ||
            c >= m->vertex_count) continue;
        const float *pa = &m->positions[a * 3];
        const float *pb = &m->positions[b * 3];
        const float *pc = &m->positions[c * 3];
        float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
        float e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
        float n[3]  = {
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        };
        for (int k = 0; k < 3; ++k) {
            m->normals[a * 3 + k] += n[k];
            m->normals[b * 3 + k] += n[k];
            m->normals[c * 3 + k] += n[k];
        }
    }
    for (uint32_t i = 0; i < m->vertex_count; ++i)
        normalize3(&m->normals[i * 3]);
    m->dirty_flags |= JCE_PROC_MESH_DIRTY_NORMALS;
    return true;
}

bool jce_proc_mesh_append_quad(JceProcMesh *m,
                                const float *p0, const float *p1,
                                const float *p2, const float *p3)
{
    if (!m || !p0 || !p1 || !p2 || !p3) return false;
    uint32_t base = m->vertex_count;
    if (!ensure_vertex_capacity(m, base + 4)) return false;
    float *pos = &m->positions[base * 3];
    memcpy(pos + 0,  p0, 3 * sizeof(float));
    memcpy(pos + 3,  p1, 3 * sizeof(float));
    memcpy(pos + 6,  p2, 3 * sizeof(float));
    memcpy(pos + 9,  p3, 3 * sizeof(float));
    m->vertex_count = base + 4;

    if (!ensure_index_capacity(m, m->index_count + 6)) return false;
    uint32_t *idx = &m->indices[m->index_count];
    idx[0] = base + 0; idx[1] = base + 1; idx[2] = base + 2;
    idx[3] = base + 0; idx[4] = base + 2; idx[5] = base + 3;
    m->index_count += 6;

    m->dirty_flags |= JCE_PROC_MESH_DIRTY_VERTICES |
                       JCE_PROC_MESH_DIRTY_INDICES;
    return true;
}
