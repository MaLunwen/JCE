/*
 * jce_lightmap_uv.c  Greedy shelf packer over face-normal charts.
 *
 * Steps
 *   1. Compute per-triangle normal.  Group triangles into charts
 *      using a union-find: triangles share a chart iff they share an
 *      edge AND their normals are within `chart_angle_cos`.
 *   2. For each chart, compute axis-aligned bbox in tangent space
 *      (project triangle positions onto a plane aligned to the
 *      chart's average normal).
 *   3. Pack chart bboxes into the atlas using shelf-first-fit.
 *   4. Write per-vertex UV2 by re-projecting each triangle's vertices
 *      into its chart's tangent space + remapping to the packed rect.
 *
 * This is intentionally simple — production quality (LSCM, ABF, etc.)
 * is a future xatlas integration; the goal here is "useful UV2 from
 * a mesh that didn't ship one".
 */

#include <jce/renderer/jce_lightmap_uv.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Union-find ──────────────────────────────────────────────── */

static int uf_find(int *p, int x)
{
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}
static void uf_union(int *p, int a, int b)
{
    int ra = uf_find(p, a), rb = uf_find(p, b);
    if (ra != rb) p[ra] = rb;
}

/* Map a pair of vertex indices to a 64-bit key (smaller first). */
static uint64_t edge_key(uint32_t a, uint32_t b)
{
    if (a > b) { uint32_t t = a; a = b; b = t; }
    return ((uint64_t)a << 32) | (uint64_t)b;
}

/* ── Tangent-space projection ────────────────────────────────── */

static void make_basis(const float n[3], float t[3], float bt[3])
{
    float up[3];
    if (fabsf(n[1]) < 0.9f) { up[0]=0; up[1]=1; up[2]=0; }
    else                    { up[0]=1; up[1]=0; up[2]=0; }
    t[0] = up[1]*n[2] - up[2]*n[1];
    t[1] = up[2]*n[0] - up[0]*n[2];
    t[2] = up[0]*n[1] - up[1]*n[0];
    float Lt = sqrtf(t[0]*t[0]+t[1]*t[1]+t[2]*t[2]);
    if (Lt > 1e-6f) { t[0]/=Lt; t[1]/=Lt; t[2]/=Lt; }
    bt[0] = n[1]*t[2] - n[2]*t[1];
    bt[1] = n[2]*t[0] - n[0]*t[2];
    bt[2] = n[0]*t[1] - n[1]*t[0];
}

/* ── Public ──────────────────────────────────────────────────── */

bool jce_lightmap_uv_generate(const JceLightmapUvInput *in,
                               const JceLightmapUvOptions *opts,
                               JceLightmapUvResult *res)
{
    if (!in || !opts || !res || !res->out_uv2) return false;
    if (in->triangle_count == 0 || in->vertex_count == 0) return false;
    if (opts->atlas_width == 0 || opts->atlas_height == 0) return false;

    uint32_t tc = in->triangle_count;
    /* Per-triangle normals. */
    float *tnorm = (float *)malloc(tc * 3 * sizeof(float));
    if (!tnorm) return false;
    for (uint32_t t = 0; t < tc; ++t) {
        uint32_t i0 = in->indices[t*3 + 0];
        uint32_t i1 = in->indices[t*3 + 1];
        uint32_t i2 = in->indices[t*3 + 2];
        const float *p0 = &in->positions[i0*3];
        const float *p1 = &in->positions[i1*3];
        const float *p2 = &in->positions[i2*3];
        float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        float n[3]  = {
            e1[1]*e2[2]-e1[2]*e2[1],
            e1[2]*e2[0]-e1[0]*e2[2],
            e1[0]*e2[1]-e1[1]*e2[0]
        };
        float L = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (L > 1e-7f) { n[0]/=L; n[1]/=L; n[2]/=L; }
        tnorm[t*3+0]=n[0]; tnorm[t*3+1]=n[1]; tnorm[t*3+2]=n[2];
    }
    /* Union-find by edge sharing + normal similarity. */
    int *parent = (int *)malloc(tc * sizeof(int));
    for (uint32_t i = 0; i < tc; ++i) parent[i] = (int)i;

    /* Build edge → first-triangle map.  Linear scan is O(n²) but
     * fine for editor-bake-time scale (thousands of tris). */
    uint64_t *keys = (uint64_t *)malloc(tc * 3 * sizeof(uint64_t));
    uint32_t *owner = (uint32_t *)malloc(tc * 3 * sizeof(uint32_t));
    uint32_t  e = 0;
    for (uint32_t t = 0; t < tc; ++t) {
        uint32_t i0 = in->indices[t*3 + 0];
        uint32_t i1 = in->indices[t*3 + 1];
        uint32_t i2 = in->indices[t*3 + 2];
        keys[e]   = edge_key(i0,i1); owner[e++] = t;
        keys[e]   = edge_key(i1,i2); owner[e++] = t;
        keys[e]   = edge_key(i2,i0); owner[e++] = t;
    }
    /* Insertion sort by key — same simple approach as cloth dedupe. */
    for (uint32_t i = 1; i < e; ++i) {
        uint64_t k = keys[i]; uint32_t w = owner[i]; uint32_t j = i;
        while (j > 0 && keys[j-1] > k) {
            keys[j] = keys[j-1]; owner[j] = owner[j-1]; j--;
        }
        keys[j] = k; owner[j] = w;
    }
    float cos_thresh = opts->chart_angle_cos > 0 ? opts->chart_angle_cos : 0.707f;
    for (uint32_t i = 0; i + 1 < e; ++i) {
        if (keys[i] != keys[i+1]) continue;
        uint32_t ta = owner[i], tb = owner[i+1];
        float dot = tnorm[ta*3]*tnorm[tb*3] +
                    tnorm[ta*3+1]*tnorm[tb*3+1] +
                    tnorm[ta*3+2]*tnorm[tb*3+2];
        if (dot >= cos_thresh) uf_union(parent, (int)ta, (int)tb);
    }
    free(keys); free(owner);

    /* Collect charts. */
    int *chart_id = (int *)malloc(tc * sizeof(int));
    int  next_chart = 0;
    int  *root_to_chart = (int *)malloc(tc * sizeof(int));
    for (uint32_t i = 0; i < tc; ++i) root_to_chart[i] = -1;
    for (uint32_t t = 0; t < tc; ++t) {
        int r = uf_find(parent, (int)t);
        if (root_to_chart[r] < 0) root_to_chart[r] = next_chart++;
        chart_id[t] = root_to_chart[r];
    }
    uint32_t chart_count = (uint32_t)next_chart;

    /* Per-chart bbox in tangent space. */
    float *chart_min  = (float *)calloc(chart_count * 2, sizeof(float));
    float *chart_max  = (float *)calloc(chart_count * 2, sizeof(float));
    float *chart_norm = (float *)calloc(chart_count * 3, sizeof(float));
    for (uint32_t i = 0; i < chart_count; ++i) {
        chart_min[i*2+0] =  INFINITY; chart_min[i*2+1] =  INFINITY;
        chart_max[i*2+0] = -INFINITY; chart_max[i*2+1] = -INFINITY;
    }
    /* Accumulate average normal. */
    for (uint32_t t = 0; t < tc; ++t) {
        int c = chart_id[t];
        chart_norm[c*3+0] += tnorm[t*3+0];
        chart_norm[c*3+1] += tnorm[t*3+1];
        chart_norm[c*3+2] += tnorm[t*3+2];
    }
    for (uint32_t c = 0; c < chart_count; ++c) {
        float *n = &chart_norm[c*3];
        float L = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (L > 1e-7f) { n[0]/=L; n[1]/=L; n[2]/=L; }
    }
    /* Project vertex positions into chart tangent frame to bbox. */
    float *vert_u = (float *)calloc(in->vertex_count, sizeof(float));
    float *vert_v = (float *)calloc(in->vertex_count, sizeof(float));
    int   *vert_chart = (int *)malloc(in->vertex_count * sizeof(int));
    for (uint32_t i = 0; i < in->vertex_count; ++i) vert_chart[i] = -1;
    for (uint32_t t = 0; t < tc; ++t) {
        int c = chart_id[t];
        float tb[3], bb[3];
        make_basis(&chart_norm[c*3], tb, bb);
        for (int v = 0; v < 3; ++v) {
            uint32_t vi = in->indices[t*3+v];
            const float *p = &in->positions[vi*3];
            float u = p[0]*tb[0] + p[1]*tb[1] + p[2]*tb[2];
            float w = p[0]*bb[0] + p[1]*bb[1] + p[2]*bb[2];
            vert_u[vi] = u;
            vert_v[vi] = w;
            vert_chart[vi] = c;
            if (u < chart_min[c*2+0]) chart_min[c*2+0] = u;
            if (w < chart_min[c*2+1]) chart_min[c*2+1] = w;
            if (u > chart_max[c*2+0]) chart_max[c*2+0] = u;
            if (w > chart_max[c*2+1]) chart_max[c*2+1] = w;
        }
    }

    /* Shelf pack chart bboxes.  Sort by height descending. */
    typedef struct { uint32_t idx; float w; float h; } Rect;
    Rect *rects = (Rect *)malloc(chart_count * sizeof(Rect));
    for (uint32_t c = 0; c < chart_count; ++c) {
        rects[c].idx = c;
        rects[c].w   = chart_max[c*2+0] - chart_min[c*2+0];
        rects[c].h   = chart_max[c*2+1] - chart_min[c*2+1];
    }
    for (uint32_t i = 1; i < chart_count; ++i) {
        Rect r = rects[i]; uint32_t j = i;
        while (j > 0 && rects[j-1].h < r.h) { rects[j] = rects[j-1]; j--; }
        rects[j] = r;
    }
    float *chart_x = (float *)calloc(chart_count, sizeof(float));
    float *chart_y = (float *)calloc(chart_count, sizeof(float));
    float pad = (float)opts->padding_px / (float)opts->atlas_width;
    float pen_x = pad, pen_y = pad, shelf_h = 0;
    float aw = 1.0f, ah = (float)opts->atlas_height / (float)opts->atlas_width;
    float used_area = 0;
    /* Normalise raw extents to unit-square scale by total bbox span. */
    float total_max_dim = 1.0f;
    for (uint32_t i = 0; i < chart_count; ++i) {
        if (rects[i].w > total_max_dim) total_max_dim = rects[i].w;
        if (rects[i].h > total_max_dim) total_max_dim = rects[i].h;
    }
    float scale = 1.0f / total_max_dim * 0.4f; /* fit at most ~2 charts wide */
    for (uint32_t i = 0; i < chart_count; ++i) {
        Rect *r = &rects[i];
        float nw = r->w * scale;
        float nh = r->h * scale;
        if (pen_x + nw + pad > aw) {
            pen_x = pad;
            pen_y += shelf_h + pad;
            shelf_h = 0;
        }
        if (pen_y + nh + pad > ah) break; /* overflow */
        chart_x[r->idx] = pen_x;
        chart_y[r->idx] = pen_y;
        pen_x += nw + pad;
        if (nh > shelf_h) shelf_h = nh;
        used_area += nw * nh;
    }
    /* Emit UV2. */
    for (uint32_t v = 0; v < in->vertex_count; ++v) {
        int c = vert_chart[v];
        if (c < 0) {
            res->out_uv2[v*2+0] = 0;
            res->out_uv2[v*2+1] = 0;
            continue;
        }
        float u_off = vert_u[v] - chart_min[c*2+0];
        float v_off = vert_v[v] - chart_min[c*2+1];
        res->out_uv2[v*2+0] = chart_x[c] + u_off * scale;
        res->out_uv2[v*2+1] = chart_y[c] + v_off * scale;
    }
    res->chart_count = chart_count;
    res->utilisation = used_area / (aw * ah);

    free(tnorm); free(parent); free(chart_id); free(root_to_chart);
    free(chart_min); free(chart_max); free(chart_norm);
    free(vert_u); free(vert_v); free(vert_chart);
    free(rects); free(chart_x); free(chart_y);
    return true;
}
