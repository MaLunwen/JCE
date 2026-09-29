/*
 * jce_hlod_bake.c  Engine HLOD proxy bake (Direction A2).  See jce_hlod_bake.h.
 *
 * merge (transform every source mesh into world space) → simplify (meshopt via
 * jce_mesh_simplify) → compact unused vertices → write a single-mesh proxy .glb.
 */

#include <jce/resource/jce_hlod_bake.h>

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_glb_write.h>
#include <jce/resource/jce_mesh_merge.h>

#include "os/core/jce_memory.h"
#include "resource/jce_mesh_lod_cook.h"   /* jce_mesh_simplify */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "hlod_bake"

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

    /* MERGE -- the shared one (jce_mesh_merge.h).  A proxy carries no UVs, so
     * none are supplied and none come back: this is a distant flat-tinted
     * stand-in and texture coordinates for it would be dead bytes in every
     * streamed cell. */
    bool ok = false;
    JceMeshMergeResult merged;
    memset(&merged, 0, sizeof merged);
    JceMeshMergeInput *mi_in = (JceMeshMergeInput *)
        JCE_MALLOC((size_t)input_count * sizeof *mi_in);
    if (!mi_in) return false;
    for (uint32_t i = 0; i < input_count; ++i) {
        memset(&mi_in[i], 0, sizeof mi_in[i]);
        mi_in[i].positions       = inputs[i].positions;
        mi_in[i].position_stride = inputs[i].position_stride;
        mi_in[i].normals         = inputs[i].normals;
        mi_in[i].normal_stride   = inputs[i].normal_stride;
        mi_in[i].vertex_count    = inputs[i].vertex_count;
        mi_in[i].indices         = inputs[i].indices;
        mi_in[i].index_count     = inputs[i].index_count;
        memcpy(mi_in[i].world, inputs[i].world, sizeof mi_in[i].world);
    }
    const bool merged_ok = jce_mesh_merge(mi_in, input_count, &merged);
    JCE_FREE(mi_in);
    if (!merged_ok) {
        LOG_WARN(LOG_TAG, "no geometry to bake");
        return false;
    }

    float        *mpos = merged.positions;   /* owned by `merged` */
    float        *mnrm = merged.normals;
    unsigned int *midx = merged.indices;
    unsigned int *oidx = (unsigned int *)
        JCE_MALLOC((size_t)merged.index_count * sizeof(unsigned int));
    uint32_t     *remap = NULL;
    float        *cpos = NULL, *cnrm = NULL;
    if (!oidx) goto done;

    uint32_t mv = merged.vertex_count, mi = merged.index_count;

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
    JCE_FREE(oidx);
    jce_mesh_merge_free(&merged);   /* owns mpos / mnrm / midx */
    return ok;
}
