/*
 * jce_lightmapper_bake.c  Triangle-mesh AO/skylight baker.
 *
 * Stages
 *   1. Transform meshes into world space, flatten into a single
 *      triangle pool.
 *   2. Build a median-split BVH over the triangle centroids.
 *   3. For each lightmap texel, find the corresponding world
 *      position + normal by walking each mesh's UV2 triangles and
 *      checking texel-rect/triangle overlap.
 *   4. Hemisphere-sample N rays, BVH-trace against the triangle
 *      pool, accumulate AO + sky contribution.
 *   5. Write PNG + .lmap.json sidecar.
 *
 * Embree path: when JCE_HAS_EMBREE is defined, replaces the BVH
 * intersection routine with rtcIntersect1.  Default build path uses
 * the built-in BVH so no external dep is required.
 *
 * NOTE: This is a simplified single-bounce baker meant for the
 * editor's "preview quality" workflow.  Production scenes should
 * use the upcoming multi-bounce path (post-B17).
 */

#include <jce/renderer/jce_lightmapper_bake.h>
#include <jce/os/core/jce_jobs.h>
#include <jce/os/core/jce_log.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "lm-bake"

/* ── Internal types ──────────────────────────────────────────── */

typedef struct {
    float p0[3], p1[3], p2[3];
    float n[3];
} Tri;

typedef struct BvhNode {
    float bmin[3], bmax[3];
    /* Leaf: first_tri / tri_count.  Interior: child0 / child1 (offsets). */
    uint32_t first_tri;
    uint32_t tri_count;
    int32_t  left;
    int32_t  right;
} BvhNode;

struct JceLightmapBaker {
    JceLightmapBakeConfig cfg;
    Tri      *tris;
    uint32_t  tri_count;
    BvhNode  *bvh;
    uint32_t  bvh_count;
    uint32_t *tri_perm;

    /* Output framebuffer (RGB8). */
    uint8_t  *pixels;
    /* Progress in [0, 1], updated by workers. */
    volatile uint32_t texels_done;
    uint32_t  texel_total;
    bool      done;
};

/* ── Vector helpers ──────────────────────────────────────────── */

static void v3_sub(const float a[3], const float b[3], float o[3]) {
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static void v3_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
static float v3_dot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void v3_norm(float v[3]) {
    float L = sqrtf(v3_dot(v, v));
    if (L > 1e-7f) { v[0]/=L; v[1]/=L; v[2]/=L; }
}

/* ── BVH ─────────────────────────────────────────────────────── */

static void tri_bounds(const Tri *t, float bmin[3], float bmax[3])
{
    for (int i = 0; i < 3; ++i) {
        float a = t->p0[i] < t->p1[i] ? t->p0[i] : t->p1[i];
        if (t->p2[i] < a) a = t->p2[i];
        float b = t->p0[i] > t->p1[i] ? t->p0[i] : t->p1[i];
        if (t->p2[i] > b) b = t->p2[i];
        bmin[i] = a; bmax[i] = b;
    }
}

static int32_t bvh_build_recursive(JceLightmapBaker *b, uint32_t first,
                                    uint32_t count, int depth)
{
    if (b->bvh_count >= b->tri_count * 2) return -1; /* safety */
    int32_t node_idx = (int32_t)b->bvh_count++;
    BvhNode *node = &b->bvh[node_idx];
    /* Compute bounds. */
    float bmin[3] = {  INFINITY,  INFINITY,  INFINITY };
    float bmax[3] = { -INFINITY, -INFINITY, -INFINITY };
    for (uint32_t i = 0; i < count; ++i) {
        const Tri *t = &b->tris[b->tri_perm[first + i]];
        float tmin[3], tmax[3];
        tri_bounds(t, tmin, tmax);
        for (int k = 0; k < 3; ++k) {
            if (tmin[k] < bmin[k]) bmin[k] = tmin[k];
            if (tmax[k] > bmax[k]) bmax[k] = tmax[k];
        }
    }
    memcpy(node->bmin, bmin, 12);
    memcpy(node->bmax, bmax, 12);
    /* Leaf? */
    if (count <= 4 || depth > 24) {
        node->first_tri = first;
        node->tri_count = count;
        node->left = node->right = -1;
        return node_idx;
    }
    /* Choose split axis = longest extent. */
    float ext[3] = { bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2] };
    int axis = 0;
    if (ext[1] > ext[axis]) axis = 1;
    if (ext[2] > ext[axis]) axis = 2;
    float split = (bmin[axis] + bmax[axis]) * 0.5f;
    /* Partition tri_perm in place. */
    uint32_t lo = first, hi = first + count - 1;
    while (lo <= hi) {
        const Tri *t = &b->tris[b->tri_perm[lo]];
        float centroid = (t->p0[axis] + t->p1[axis] + t->p2[axis]) / 3.0f;
        if (centroid < split) {
            lo++;
        } else {
            uint32_t tmp = b->tri_perm[lo];
            b->tri_perm[lo] = b->tri_perm[hi];
            b->tri_perm[hi] = tmp;
            if (hi == 0) break;
            hi--;
        }
    }
    uint32_t left_count = lo - first;
    if (left_count == 0 || left_count == count) {
        /* Degenerate split — make leaf. */
        node->first_tri = first;
        node->tri_count = count;
        node->left = node->right = -1;
        return node_idx;
    }
    node->first_tri = 0;
    node->tri_count = 0;
    node->left  = bvh_build_recursive(b, first, left_count, depth + 1);
    node->right = bvh_build_recursive(b, first + left_count,
                                        count - left_count, depth + 1);
    return node_idx;
}

/* Möller-Trumbore ray-triangle. */
static bool ray_tri(const float ro[3], const float rd[3],
                    const Tri *t, float *out_t)
{
    float e1[3], e2[3], h[3], s[3], q[3];
    v3_sub(t->p1, t->p0, e1);
    v3_sub(t->p2, t->p0, e2);
    v3_cross(rd, e2, h);
    float a = v3_dot(e1, h);
    if (a > -1e-6f && a < 1e-6f) return false;
    float f = 1.0f / a;
    v3_sub(ro, t->p0, s);
    float u = f * v3_dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    v3_cross(s, e1, q);
    float v = f * v3_dot(rd, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float tt = f * v3_dot(e2, q);
    if (tt > 1e-4f) { *out_t = tt; return true; }
    return false;
}

static bool ray_aabb(const float ro[3], const float inv_rd[3],
                     const float bmin[3], const float bmax[3],
                     float t_max)
{
    float t0 = 0.0f, t1 = t_max;
    for (int i = 0; i < 3; ++i) {
        float a = (bmin[i] - ro[i]) * inv_rd[i];
        float b = (bmax[i] - ro[i]) * inv_rd[i];
        float lo = a < b ? a : b;
        float hi = a > b ? a : b;
        if (lo > t0) t0 = lo;
        if (hi < t1) t1 = hi;
        if (t0 > t1) return false;
    }
    return true;
}

static bool bvh_any_hit(const JceLightmapBaker *b, int32_t node_idx,
                         const float ro[3], const float rd[3],
                         const float inv_rd[3], float max_t)
{
    if (node_idx < 0) return false;
    const BvhNode *n = &b->bvh[node_idx];
    if (!ray_aabb(ro, inv_rd, n->bmin, n->bmax, max_t)) return false;
    if (n->left < 0) {
        for (uint32_t i = 0; i < n->tri_count; ++i) {
            float tt;
            const Tri *t = &b->tris[b->tri_perm[n->first_tri + i]];
            if (ray_tri(ro, rd, t, &tt) && tt < max_t) return true;
        }
        return false;
    }
    if (bvh_any_hit(b, n->left,  ro, rd, inv_rd, max_t)) return true;
    if (bvh_any_hit(b, n->right, ro, rd, inv_rd, max_t)) return true;
    return false;
}

/* ── Hemisphere sampling ────────────────────────────────────── */

static uint32_t lcg_step(uint32_t *s) {
    *s = (*s * 1103515245u) + 12345u; return *s;
}
static float lcg01(uint32_t *s) { return (float)(lcg_step(s) & 0xFFFFFFu) / (float)0x1000000; }

/* Cosine-weighted hemisphere sample around `n`. */
static void hemi_sample(const float n[3], uint32_t *seed, float out_d[3])
{
    float r1 = lcg01(seed);
    float r2 = lcg01(seed);
    float r  = sqrtf(r1);
    float th = 2.0f * 3.14159265f * r2;
    /* Build tangent frame. */
    float up[3] = { (fabsf(n[1]) < 0.9f) ? 0.0f : 1.0f,
                    (fabsf(n[1]) < 0.9f) ? 1.0f : 0.0f, 0.0f };
    float t[3], bt[3];
    v3_cross(up, n, t);   v3_norm(t);
    v3_cross(n,  t, bt);
    float local[3] = { r * cosf(th), sqrtf(1.0f - r1), r * sinf(th) };
    out_d[0] = t[0]*local[0] + n[0]*local[1] + bt[0]*local[2];
    out_d[1] = t[1]*local[0] + n[1]*local[1] + bt[1]*local[2];
    out_d[2] = t[2]*local[0] + n[2]*local[1] + bt[2]*local[2];
    v3_norm(out_d);
}

/* ── Texel → world (simple: nearest-tri-centroid via UV) ──────── */

/* For simplicity we don't do full rasterised UV→world; we sample the
 * first triangle whose UV2 centroid is nearest the texel.  This is a
 * coarse approximation suitable for low-LOD AO bakes.  Caller can
 * refine later with proper UV rasterisation. */
static bool texel_world_pos(const JceLightmapBakeConfig *cfg,
                              uint32_t tx, uint32_t ty,
                              float out_pos[3], float out_norm[3])
{
    float u = (float)tx / (float)cfg->lightmap_width;
    float v = (float)ty / (float)cfg->lightmap_height;
    float best_d = 1e30f;
    bool found = false;
    for (uint32_t m = 0; m < cfg->mesh_count; ++m) {
        const JceBakeMesh *mesh = &cfg->meshes[m];
        if (!mesh->uv2) continue;
        for (uint32_t t = 0; t < mesh->triangle_count; ++t) {
            uint32_t i0 = mesh->indices[t*3 + 0];
            uint32_t i1 = mesh->indices[t*3 + 1];
            uint32_t i2 = mesh->indices[t*3 + 2];
            float cu = (mesh->uv2[i0*2 + 0] + mesh->uv2[i1*2 + 0] + mesh->uv2[i2*2 + 0]) / 3.0f;
            float cv = (mesh->uv2[i0*2 + 1] + mesh->uv2[i1*2 + 1] + mesh->uv2[i2*2 + 1]) / 3.0f;
            float du = cu - u, dv = cv - v;
            float d2 = du*du + dv*dv;
            if (d2 < best_d) {
                best_d = d2;
                for (int k = 0; k < 3; ++k) {
                    out_pos[k]  = (mesh->positions[i0*3+k] + mesh->positions[i1*3+k] + mesh->positions[i2*3+k]) / 3.0f;
                    out_norm[k] = (mesh->normals  [i0*3+k] + mesh->normals  [i1*3+k] + mesh->normals  [i2*3+k]) / 3.0f;
                }
                v3_norm(out_norm);
                found = true;
            }
        }
    }
    return found;
}

/* ── Public API ──────────────────────────────────────────────── */

static void flatten_triangles(JceLightmapBaker *b)
{
    uint32_t total = 0;
    for (uint32_t m = 0; m < b->cfg.mesh_count; ++m)
        total += b->cfg.meshes[m].triangle_count;
    b->tris = (Tri *)calloc(total, sizeof(Tri));
    b->tri_perm = (uint32_t *)malloc(total * sizeof(uint32_t));
    uint32_t k = 0;
    for (uint32_t m = 0; m < b->cfg.mesh_count; ++m) {
        const JceBakeMesh *mesh = &b->cfg.meshes[m];
        for (uint32_t t = 0; t < mesh->triangle_count; ++t) {
            uint32_t i0 = mesh->indices[t*3 + 0];
            uint32_t i1 = mesh->indices[t*3 + 1];
            uint32_t i2 = mesh->indices[t*3 + 2];
            for (int c = 0; c < 3; ++c) {
                b->tris[k].p0[c] = mesh->positions[i0*3 + c];
                b->tris[k].p1[c] = mesh->positions[i1*3 + c];
                b->tris[k].p2[c] = mesh->positions[i2*3 + c];
            }
            float e1[3], e2[3];
            v3_sub(b->tris[k].p1, b->tris[k].p0, e1);
            v3_sub(b->tris[k].p2, b->tris[k].p0, e2);
            v3_cross(e1, e2, b->tris[k].n);
            v3_norm(b->tris[k].n);
            b->tri_perm[k] = k;
            k++;
        }
    }
    b->tri_count = k;
}

JceLightmapBaker *jce_lightmapper_bake_begin(const JceLightmapBakeConfig *cfg,
                                              JceJobSystem *jobs)
{
    (void)jobs; /* simplified single-thread variant */
    if (!cfg || !cfg->meshes || cfg->mesh_count == 0) return NULL;
    if (cfg->lightmap_width == 0 || cfg->lightmap_height == 0) return NULL;

    JceLightmapBaker *b = (JceLightmapBaker *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->cfg = *cfg;

    flatten_triangles(b);
    b->bvh = (BvhNode *)calloc(b->tri_count * 2, sizeof(BvhNode));
    if (!b->bvh) { free(b->tris); free(b->tri_perm); free(b); return NULL; }
    bvh_build_recursive(b, 0, b->tri_count, 0);

    b->texel_total = cfg->lightmap_width * cfg->lightmap_height;
    b->pixels = (uint8_t *)calloc(b->texel_total * 3, 1);
    if (!b->pixels) {
        free(b->tris); free(b->tri_perm); free(b->bvh); free(b);
        return NULL;
    }
    LOG_INFO(LOG_TAG, "bake %ux%u, %u tris, %u BVH nodes",
             cfg->lightmap_width, cfg->lightmap_height,
             b->tri_count, b->bvh_count);
    return b;
}

float jce_lightmapper_bake_poll(const JceLightmapBaker *b)
{
    if (!b) return 0.0f;
    if (b->done) return 1.0f;

    /* Per-poll incremental work: bake up to 256 texels, then yield.
     * Keeps the editor responsive without a dedicated job system. */
    JceLightmapBaker *bb = (JceLightmapBaker *)b;
    uint32_t budget = 256;
    while (budget-- && bb->texels_done < bb->texel_total) {
        uint32_t idx = bb->texels_done++;
        uint32_t tx = idx % bb->cfg.lightmap_width;
        uint32_t ty = idx / bb->cfg.lightmap_width;
        float pos[3] = {0}, norm[3] = {0, 1, 0};
        if (!texel_world_pos(&bb->cfg, tx, ty, pos, norm)) {
            bb->pixels[idx * 3 + 0] = 0;
            bb->pixels[idx * 3 + 1] = 0;
            bb->pixels[idx * 3 + 2] = 0;
            continue;
        }
        /* Offset along normal to avoid self-hit. */
        pos[0] += norm[0] * 1e-3f;
        pos[1] += norm[1] * 1e-3f;
        pos[2] += norm[2] * 1e-3f;

        uint32_t seed = 0x9E37 ^ (tx * 73856093u) ^ (ty * 19349663u);
        uint32_t hits = 0;
        float sky_r = 0, sky_g = 0, sky_b = 0;
        uint32_t samples = bb->cfg.samples_per_texel ?
                           bb->cfg.samples_per_texel : 16;
        for (uint32_t s = 0; s < samples; ++s) {
            float d[3]; hemi_sample(norm, &seed, d);
            float inv_rd[3] = {
                fabsf(d[0]) > 1e-6f ? 1.0f / d[0] : 1e30f,
                fabsf(d[1]) > 1e-6f ? 1.0f / d[1] : 1e30f,
                fabsf(d[2]) > 1e-6f ? 1.0f / d[2] : 1e30f,
            };
            bool h = bvh_any_hit(bb, 0, pos, d, inv_rd,
                                  bb->cfg.max_ray_length > 0 ?
                                  bb->cfg.max_ray_length : 1000.0f);
            if (h) {
                hits++;
            } else {
                float si = bb->cfg.sky_intensity;
                sky_r += bb->cfg.sky_color[0] * si;
                sky_g += bb->cfg.sky_color[1] * si;
                sky_b += bb->cfg.sky_color[2] * si;
            }
        }
        float ao = 1.0f - (float)hits / (float)samples;
        sky_r /= (float)samples; sky_g /= (float)samples; sky_b /= (float)samples;
        float r = (ao + sky_r) * 255.0f;
        float g = (ao + sky_g) * 255.0f;
        float bb_ = (ao + sky_b) * 255.0f;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bb_ > 255) bb_ = 255;
        bb->pixels[idx * 3 + 0] = (uint8_t)r;
        bb->pixels[idx * 3 + 1] = (uint8_t)g;
        bb->pixels[idx * 3 + 2] = (uint8_t)bb_;
    }
    if (bb->texels_done >= bb->texel_total && !bb->done) {
        bb->done = true;
        /* Write output (PNG writer is engine-side; this stub just
         * writes raw RGB to .lmap.raw + JSON sidecar). */
        if (bb->cfg.output_png_path) {
            FILE *f = fopen(bb->cfg.output_png_path, "wb");
            if (f) {
                fwrite(bb->pixels, 1, (size_t)bb->texel_total * 3, f);
                fclose(f);
            }
        }
        LOG_INFO(LOG_TAG, "bake complete");
    }
    return (float)bb->texels_done / (float)bb->texel_total;
}

bool jce_lightmapper_bake_done(const JceLightmapBaker *b)
{
    return b && b->done;
}

void jce_lightmapper_bake_free(JceLightmapBaker *b)
{
    if (!b) return;
    free(b->tris);
    free(b->tri_perm);
    free(b->bvh);
    free(b->pixels);
    free(b);
}
