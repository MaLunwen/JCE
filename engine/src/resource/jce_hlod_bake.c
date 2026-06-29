/*
 * jce_hlod_bake.c  Engine HLOD proxy bake (Direction A2).  See jce_hlod_bake.h.
 *
 * merge (transform every source mesh into world space) → simplify (meshopt via
 * jce_mesh_simplify) → compact unused vertices → write a single-mesh proxy .glb.
 */

#include <jce/resource/jce_hlod_bake.h>

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_glb_write.h>

#include "os/core/jce_memory.h"
#include "resource/jce_mesh_lod_cook.h"   /* jce_mesh_simplify */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "hlod_bake"

static void xform_point(const float *m, const float *p, float *o)
{
    o[0] = m[0]*p[0] + m[4]*p[1] + m[8] *p[2] + m[12];
    o[1] = m[1]*p[0] + m[5]*p[1] + m[9] *p[2] + m[13];
    o[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

static void xform_dir(const float *m, const float *n, float *o)
{
    o[0] = m[0]*n[0] + m[4]*n[1] + m[8] *n[2];
    o[1] = m[1]*n[0] + m[5]*n[1] + m[9] *n[2];
    o[2] = m[2]*n[0] + m[6]*n[1] + m[10]*n[2];
    float l = sqrtf(o[0]*o[0] + o[1]*o[1] + o[2]*o[2]);
    if (l > 1e-8f) { o[0]/=l; o[1]/=l; o[2]/=l; }
    else { o[0]=0.0f; o[1]=1.0f; o[2]=0.0f; }
}

bool jce_hlod_bake_proxy(const JceHlodMeshInput *inputs,
                         uint32_t                input_count,
                         float                   target_ratio,
                         const float             base_color[4],
                         const char             *out_glb_host_path,
                         JceHlodBakeStats       *out_stats)
{
    if (out_stats) memset(out_stats, 0, sizeof *out_stats);
    if (!inputs || input_count == 0 || !out_glb_host_path) return false;
    if (!(target_ratio > 0.0f)) target_ratio = 0.15f;
    if (target_ratio > 1.0f) target_ratio = 1.0f;

    /* Totals. */
    uint64_t total_v = 0, total_i = 0;
    for (uint32_t i = 0; i < input_count; ++i) {
        if (!inputs[i].positions || !inputs[i].indices) continue;
        total_v += inputs[i].vertex_count;
        total_i += inputs[i].index_count;
    }
    if (total_v == 0 || total_i < 3) {
        LOG_WARN(LOG_TAG, "no geometry to bake");
        return false;
    }

    bool ok = false;
    float        *mpos = (float *)JCE_MALLOC(total_v * 3u * sizeof(float));
    float        *mnrm = (float *)JCE_MALLOC(total_v * 3u * sizeof(float));
    unsigned int *midx = (unsigned int *)JCE_MALLOC(total_i * sizeof(unsigned int));
    unsigned int *oidx = (unsigned int *)JCE_MALLOC(total_i * sizeof(unsigned int));
    uint32_t     *remap = NULL;
    float        *cpos = NULL, *cnrm = NULL;
    if (!mpos || !mnrm || !midx || !oidx) goto done;

    /* Merge — transform each source mesh into world space. */
    uint32_t vbase = 0, ibase = 0;
    for (uint32_t s = 0; s < input_count; ++s) {
        const JceHlodMeshInput *in = &inputs[s];
        if (!in->positions || !in->indices) continue;
        const uint32_t ps = in->position_stride ? in->position_stride : 12u;
        const uint32_t ns = in->normal_stride   ? in->normal_stride   : 12u;
        const uint8_t *pbase = (const uint8_t *)in->positions;
        const uint8_t *nbase = (const uint8_t *)in->normals;
        for (uint32_t v = 0; v < in->vertex_count; ++v) {
            const float *pv = (const float *)(pbase + (size_t)v * ps);
            xform_point(in->world, pv, &mpos[(vbase+v)*3]);
            float nlocal[3] = { 0.0f, 1.0f, 0.0f };
            if (nbase) {
                const float *nv = (const float *)(nbase + (size_t)v * ns);
                nlocal[0]=nv[0]; nlocal[1]=nv[1]; nlocal[2]=nv[2];
            }
            xform_dir(in->world, nlocal, &mnrm[(vbase+v)*3]);
        }
        for (uint32_t k = 0; k < in->index_count; ++k)
            midx[ibase + k] = in->indices[k] + vbase;
        vbase += in->vertex_count;
        ibase += in->index_count;
    }
    uint32_t mv = vbase, mi = ibase;

    /* Simplify (meshopt).  Falls back to the merged buffer if simplify fails. */
    size_t n_out = jce_mesh_simplify(mpos, mv, 3u * sizeof(float),
                                     midx, mi, target_ratio, 0.02f, oidx);
    if (n_out < 3) { memcpy(oidx, midx, mi * sizeof(unsigned int)); n_out = mi; }

    /* Compact the vertices actually referenced by the simplified index set. */
    remap = (uint32_t *)JCE_MALLOC(mv * sizeof(uint32_t));
    if (!remap) goto done;
    for (uint32_t i = 0; i < mv; ++i) remap[i] = UINT32_MAX;
    uint32_t nv = 0;
    for (size_t i = 0; i < n_out; ++i) {
        unsigned int vi = oidx[i];
        if (vi < mv && remap[vi] == UINT32_MAX) remap[vi] = nv++;
    }
    cpos = (float *)JCE_MALLOC((size_t)nv * 3u * sizeof(float));
    cnrm = (float *)JCE_MALLOC((size_t)nv * 3u * sizeof(float));
    if (!cpos || !cnrm) goto done;
    for (uint32_t i = 0; i < mv; ++i) {
        if (remap[i] == UINT32_MAX) continue;
        memcpy(&cpos[remap[i]*3], &mpos[i*3], 3 * sizeof(float));
        memcpy(&cnrm[remap[i]*3], &mnrm[i*3], 3 * sizeof(float));
    }
    for (size_t i = 0; i < n_out; ++i) oidx[i] = remap[oidx[i]];

    float bc[4] = { 0.62f, 0.64f, 0.66f, 1.0f };
    if (base_color) { bc[0]=base_color[0]; bc[1]=base_color[1]; bc[2]=base_color[2]; bc[3]=base_color[3]; }

    ok = jce_glb_write_mesh(out_glb_host_path, cpos, cnrm, nv, oidx, (uint32_t)n_out, bc);
    if (ok && out_stats) {
        out_stats->in_vertices  = mv;
        out_stats->in_triangles = mi / 3u;
        out_stats->out_vertices = nv;
        out_stats->out_triangles = (uint32_t)(n_out / 3u);
    }
    if (ok)
        LOG_INFO(LOG_TAG, "proxy baked: %u->%u tris, %u->%u verts -> %s",
                 mi/3u, (unsigned)(n_out/3u), mv, nv, out_glb_host_path);

done:
    JCE_FREE(cnrm); JCE_FREE(cpos); JCE_FREE(remap);
    JCE_FREE(oidx); JCE_FREE(midx); JCE_FREE(mnrm); JCE_FREE(mpos);
    return ok;
}
