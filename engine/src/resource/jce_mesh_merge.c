/*
 * jce_mesh_merge.c  The one world-space mesh merge.  See jce_mesh_merge.h.
 */

#include <jce/resource/jce_mesh_merge.h>

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "mesh_merge"

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

void jce_mesh_merge_free(JceMeshMergeResult *r)
{
    if (!r) return;
    JCE_FREE(r->positions);
    JCE_FREE(r->normals);
    JCE_FREE(r->uvs);
    JCE_FREE(r->indices);
    memset(r, 0, sizeof *r);
}

bool jce_mesh_merge(const JceMeshMergeInput *inputs,
                    uint32_t                 input_count,
                    JceMeshMergeResult      *out)
{
    /* Unchanged behaviour, one caller of the spans version with no
     * spans asked for -- so there is ONE merge loop and not two that
     * could drift about what 'skipped' means. */
    return jce_mesh_merge_spans(inputs, input_count, out, NULL);
}

bool jce_mesh_merge_spans(const JceMeshMergeInput *inputs,
                          uint32_t                 input_count,
                          JceMeshMergeResult      *out,
                          JceMeshMergeSpan        *out_spans)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!inputs || input_count == 0) return false;

    /* Totals, and whether ANY input carries UVs.  A merge of untextured meshes
     * must not grow a texture coordinate: an all-zero UV set and no UV set are
     * different things to the .glb writer and to whatever samples it. */
    uint64_t total_v = 0, total_i = 0;
    bool any_uv = false;
    for (uint32_t i = 0; i < input_count; ++i) {
        if (!inputs[i].positions || !inputs[i].indices) continue;
        total_v += inputs[i].vertex_count;
        total_i += inputs[i].index_count;
        if (inputs[i].uvs) any_uv = true;
    }
    if (total_v == 0 || total_i < 3) {
        LOG_WARN(LOG_TAG, "nothing to merge (%u input(s))", input_count);
        return false;
    }
    /* One merged mesh addresses its vertices with 32-bit indices, so the merged
     * vertex count has to fit in one.  Said rather than wrapped: a caller
     * grouping a whole world by material can reach this, and a wrapped index
     * produces a mesh made of triangles between unrelated objects. */
    if (total_v > 0xFFFFFFFFull) {
        LOG_ERROR(LOG_TAG, "merged vertex count %llu exceeds the 32-bit index "
                           "space; split the group", (unsigned long long)total_v);
        return false;
    }

    float    *mpos = (float *)JCE_MALLOC((size_t)total_v * 3u * sizeof(float));
    float    *mnrm = (float *)JCE_MALLOC((size_t)total_v * 3u * sizeof(float));
    float    *muv  = any_uv
                   ? (float *)JCE_MALLOC((size_t)total_v * 2u * sizeof(float))
                   : NULL;
    uint32_t *midx = (uint32_t *)JCE_MALLOC((size_t)total_i * sizeof(uint32_t));
    if (!mpos || !mnrm || !midx || (any_uv && !muv)) {
        JCE_FREE(mpos); JCE_FREE(mnrm); JCE_FREE(muv); JCE_FREE(midx);
        return false;
    }

    if (out_spans)
        memset(out_spans, 0, (size_t)input_count * sizeof(*out_spans));

    uint32_t vbase = 0, ibase = 0;
    for (uint32_t s = 0; s < input_count; ++s) {
        const JceMeshMergeInput *in = &inputs[s];
        if (!in->positions || !in->indices) continue;
        const uint32_t ps = in->position_stride ? in->position_stride : 12u;
        const uint32_t ns = in->normal_stride   ? in->normal_stride   : 12u;
        const uint32_t us = in->uv_stride       ? in->uv_stride       : 8u;
        const uint8_t *pbase = (const uint8_t *)in->positions;
        const uint8_t *nbase = (const uint8_t *)in->normals;
        const uint8_t *ubase = (const uint8_t *)in->uvs;

        float bmin[3] = {  3.402823466e38f,  3.402823466e38f,  3.402823466e38f };
        float bmax[3] = { -3.402823466e38f, -3.402823466e38f, -3.402823466e38f };

        for (uint32_t v = 0; v < in->vertex_count; ++v) {
            const float *pv = (const float *)(pbase + (size_t)v * ps);
            float *wp = &mpos[(size_t)(vbase + v) * 3];
            xform_point(in->world, pv, wp);
            /* FROM THE VERTICES THIS MERGE WROTE, not from a local bound
             * pushed through the matrix: the two differ for a sheared or
             * mirrored instance, and only one of them is where the triangles
             * actually are. */
            for (int a = 0; a < 3; ++a) {
                if (wp[a] < bmin[a]) bmin[a] = wp[a];
                if (wp[a] > bmax[a]) bmax[a] = wp[a];
            }

            float nlocal[3] = { 0.0f, 1.0f, 0.0f };
            if (nbase) {
                const float *nv = (const float *)(nbase + (size_t)v * ns);
                nlocal[0] = nv[0]; nlocal[1] = nv[1]; nlocal[2] = nv[2];
            }
            xform_dir(in->world, nlocal, &mnrm[(size_t)(vbase + v) * 3]);

            if (muv) {
                float uv[2] = { 0.0f, 0.0f };
                if (ubase) {
                    const float *uvv = (const float *)(ubase + (size_t)v * us);
                    uv[0] = uvv[0]; uv[1] = uvv[1];
                }
                muv[(size_t)(vbase + v) * 2 + 0] = uv[0];
                muv[(size_t)(vbase + v) * 2 + 1] = uv[1];
            }
        }
        for (uint32_t k = 0; k < in->index_count; ++k)
            midx[ibase + k] = in->indices[k] + vbase;

        if (out_spans) {
            JceMeshMergeSpan *sp = &out_spans[s];
            sp->first_index  = ibase;
            sp->index_count  = in->index_count;
            sp->first_vertex = vbase;
            sp->vertex_count = in->vertex_count;
            sp->merged       = true;
            if (in->vertex_count > 0) {
                for (int a = 0; a < 3; ++a) {
                    sp->aabb_min[a] = bmin[a];
                    sp->aabb_max[a] = bmax[a];
                }
            }
        }

        vbase += in->vertex_count;
        ibase += in->index_count;
    }

    out->positions    = mpos;
    out->normals      = mnrm;
    out->uvs          = muv;
    out->vertex_count = vbase;
    out->indices      = midx;
    out->index_count  = ibase;
    return true;
}
