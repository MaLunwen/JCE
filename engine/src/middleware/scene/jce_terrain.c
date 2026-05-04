/*
 * jce_terrain.c -- Terrain runtime impl.
 */

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERRAIN_MAGIC 0x52544A43u  /* 'JCTR' little-endian */
#define TERRAIN_BIN_VERSION 1u

struct JceTerrain {
    int       w;
    int       h;
    int       chunk_size;       /* cells per chunk side (vertex count = chunk_size+1) */
    float     world_size_x;
    float     world_size_z;
    float     max_height;
    float    *heights;          /* w*h, normalized 0..1 */
    uint32_t *splat;            /* w*h, packed RGBA8 layer weights */
};

/* ───── Internal helpers ──────────────────────────────────────── */

static int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void world_to_uv(const JceTerrain *t, float wx, float wz,
                        float *out_fx, float *out_fz)
{
    /* Normalized 0..(w-1) / 0..(h-1) "vertex coords". */
    float u = wx / t->world_size_x;
    float v = wz / t->world_size_z;
    *out_fx = u * (float)(t->w - 1);
    *out_fz = v * (float)(t->h - 1);
}

static float sample_h_norm(const JceTerrain *t, float wx, float wz)
{
    float fx, fz;
    world_to_uv(t, wx, wz, &fx, &fz);
    if (fx < 0.0f || fz < 0.0f || fx > (float)(t->w - 1) || fz > (float)(t->h - 1))
        return 0.0f;
    int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
    int x1 = clampi(x0 + 1, 0, t->w - 1);
    int z1 = clampi(z0 + 1, 0, t->h - 1);
    float u = fx - (float)x0;
    float v = fz - (float)z0;
    float h00 = t->heights[(size_t)z0 * t->w + x0];
    float h10 = t->heights[(size_t)z0 * t->w + x1];
    float h01 = t->heights[(size_t)z1 * t->w + x0];
    float h11 = t->heights[(size_t)z1 * t->w + x1];
    float a = h00 * (1.0f - u) + h10 * u;
    float b = h01 * (1.0f - u) + h11 * u;
    return a * (1.0f - v) + b * v;
}

/* ───── Lifecycle ────────────────────────────────────────────── */

JceTerrain *jce_terrain_create(int width, int height,
                               float world_size_x, float world_size_z,
                               float max_height, int chunk_size)
{
    if (width < 2 || height < 2 || world_size_x <= 0.0f || world_size_z <= 0.0f)
        return NULL;
    if (chunk_size < 2) chunk_size = 32;
    JceTerrain *t = JCE_NEW(JceTerrain);
    if (!t) return NULL;
    t->w = width;
    t->h = height;
    t->chunk_size   = chunk_size;
    t->world_size_x = world_size_x;
    t->world_size_z = world_size_z;
    t->max_height   = max_height;
    size_t n = (size_t)width * (size_t)height;
    t->heights = (float    *)JCE_CALLOC(n, sizeof(float));
    t->splat   = (uint32_t *)JCE_CALLOC(n, sizeof(uint32_t));
    if (!t->heights || !t->splat) {
        JCE_FREE(t->heights); JCE_FREE(t->splat); JCE_FREE(t);
        return NULL;
    }
    /* Default: layer 0 fully opaque, others zero. */
    for (size_t i = 0; i < n; ++i)
        t->splat[i] = 0x000000FFu;
    return t;
}

void jce_terrain_free(JceTerrain *t)
{
    if (!t) return;
    JCE_FREE(t->heights);
    JCE_FREE(t->splat);
    JCE_FREE(t);
}

/* ───── IO (JSON meta + .bin side-car) ───────────────────────── */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + (n - m), suffix) == 0;
}

static void make_bin_path(const char *meta_json_path, char *out, size_t cap)
{
    if (ends_with(meta_json_path, ".json")) {
        size_t n = strlen(meta_json_path) - 5;
        if (n + 5 >= cap) n = cap - 6;
        memcpy(out, meta_json_path, n);
        memcpy(out + n, ".bin", 5);
    } else {
        snprintf(out, cap, "%s.bin", meta_json_path);
    }
}

bool jce_terrain_save_file(const JceTerrain *t, const char *meta_json_path)
{
    if (!t || !meta_json_path) return false;
    char bin_path[1024];
    make_bin_path(meta_json_path, bin_path, sizeof(bin_path));

    /* Binary blob first: serialise to one buffer, then write atomically. */
    size_t   n          = (size_t)t->w * (size_t)t->h;
    size_t   header_sz  = sizeof(uint32_t) * 2 + sizeof(int32_t) * 2;
    size_t   payload_sz = n * (sizeof(float) + sizeof(uint32_t));
    size_t   total      = header_sz + payload_sz;
    uint8_t *buf        = (uint8_t *)JCE_MALLOC(total);
    if (!buf) {
        LOG_WARN("terrain", "OOM serialising %s", bin_path);
        return false;
    }
    uint32_t magic   = TERRAIN_MAGIC;
    uint32_t version = TERRAIN_BIN_VERSION;
    int32_t  iw      = (int32_t)t->w;
    int32_t  ih      = (int32_t)t->h;
    size_t   off     = 0;
    memcpy(buf + off, &magic,   sizeof(magic));   off += sizeof(magic);
    memcpy(buf + off, &version, sizeof(version)); off += sizeof(version);
    memcpy(buf + off, &iw,      sizeof(iw));      off += sizeof(iw);
    memcpy(buf + off, &ih,      sizeof(ih));      off += sizeof(ih);
    memcpy(buf + off, t->heights, sizeof(float)    * n); off += sizeof(float)    * n;
    memcpy(buf + off, t->splat,   sizeof(uint32_t) * n); off += sizeof(uint32_t) * n;

    bool wrote = jce_fs_host_write_all(bin_path, buf, total);
    JCE_FREE(buf);
    if (!wrote) {
        LOG_WARN("terrain", "failed to write %s", bin_path);
        return false;
    }

    /* JSON meta. */
    JceJson *root = jce_json_object();
    jce_json_set_int   (root, "version",       1);
    jce_json_set_int   (root, "width",         t->w);
    jce_json_set_int   (root, "height",        t->h);
    jce_json_set_number(root, "world_size_x",  (double)t->world_size_x);
    jce_json_set_number(root, "world_size_z",  (double)t->world_size_z);
    jce_json_set_number(root, "max_height",    (double)t->max_height);
    jce_json_set_int   (root, "chunk_size",    t->chunk_size);
    /* The bin path is implied by side-car convention; record only the
     * leaf filename so the project can move directories. */
    const char *leaf = bin_path;
    for (const char *p = bin_path; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    jce_json_set_string(root, "bin", leaf);
    bool ok = jce_json_write_file(meta_json_path, root, true, false);
    jce_json_free(root);
    return ok;
}

JceTerrain *jce_terrain_load_file(const char *meta_json_path)
{
    if (!meta_json_path) return NULL;
    JceJson *root = jce_json_parse_file(meta_json_path);
    if (!root) {
        LOG_WARN("terrain", "failed to parse %s", meta_json_path);
        return NULL;
    }
    int   w  = jce_json_get_int   (root, "width",  0);
    int   h  = jce_json_get_int   (root, "height", 0);
    float wx = (float)jce_json_get_number(root, "world_size_x", 100.0);
    float wz = (float)jce_json_get_number(root, "world_size_z", 100.0);
    float mh = (float)jce_json_get_number(root, "max_height",    20.0);
    int   cs = jce_json_get_int   (root, "chunk_size", 32);
    const char *bin_leaf = jce_json_get_string(root, "bin", "");
    char bin_path[1024];
    if (bin_leaf && bin_leaf[0]) {
        /* Resolve next to the meta file. */
        size_t plen = strlen(meta_json_path);
        size_t cut  = plen;
        for (size_t i = plen; i > 0; --i) {
            char c = meta_json_path[i - 1];
            if (c == '/' || c == '\\') { cut = i; break; }
            if (i == 1) cut = 0;
        }
        if (cut > sizeof(bin_path) - 1) cut = sizeof(bin_path) - 1;
        memcpy(bin_path, meta_json_path, cut);
        snprintf(bin_path + cut, sizeof(bin_path) - cut, "%s", bin_leaf);
    } else {
        make_bin_path(meta_json_path, bin_path, sizeof(bin_path));
    }
    jce_json_free(root);

    if (w < 2 || h < 2) {
        LOG_WARN("terrain", "invalid dims %dx%d", w, h);
        return NULL;
    }
    JceTerrain *t = jce_terrain_create(w, h, wx, wz, mh, cs);
    if (!t) return NULL;

    size_t  expected = sizeof(uint32_t) * 2 + sizeof(int32_t) * 2
                       + (size_t)w * (size_t)h * (sizeof(float) + sizeof(uint32_t));
    uint64_t got     = 0;
    uint8_t *buf     = (uint8_t *)jce_fs_host_read_all(bin_path, &got);
    if (!buf) {
        LOG_WARN("terrain", "no side-car bin: %s", bin_path);
        return t;
    }
    if (got < expected) {
        LOG_WARN("terrain", "bin truncated: %s (got %zu, want %zu)",
                 bin_path, got, expected);
        JCE_FREE(buf);
        return t;
    }
    uint32_t magic = 0, version = 0;
    int32_t  iw = 0, ih = 0;
    size_t   off = 0;
    memcpy(&magic,   buf + off, sizeof(magic));   off += sizeof(magic);
    memcpy(&version, buf + off, sizeof(version)); off += sizeof(version);
    memcpy(&iw,      buf + off, sizeof(iw));      off += sizeof(iw);
    memcpy(&ih,      buf + off, sizeof(ih));      off += sizeof(ih);
    if (magic != TERRAIN_MAGIC || version != TERRAIN_BIN_VERSION
        || iw != w || ih != h) {
        LOG_WARN("terrain", "bin header mismatch: %s", bin_path);
        JCE_FREE(buf);
        return t;
    }
    size_t n = (size_t)w * (size_t)h;
    memcpy(t->heights, buf + off, sizeof(float)    * n); off += sizeof(float)    * n;
    memcpy(t->splat,   buf + off, sizeof(uint32_t) * n);
    JCE_FREE(buf);
    return t;
}

/* ───── Introspection ─────────────────────────────────────────── */

int   jce_terrain_width      (const JceTerrain *t) { return t ? t->w : 0; }
int   jce_terrain_height     (const JceTerrain *t) { return t ? t->h : 0; }
float jce_terrain_world_size_x(const JceTerrain *t){ return t ? t->world_size_x : 0.0f; }
float jce_terrain_world_size_z(const JceTerrain *t){ return t ? t->world_size_z : 0.0f; }
float jce_terrain_max_height (const JceTerrain *t) { return t ? t->max_height : 0.0f; }
int   jce_terrain_chunk_size (const JceTerrain *t) { return t ? t->chunk_size : 0; }

int jce_terrain_chunk_count_x(const JceTerrain *t)
{
    if (!t || t->chunk_size <= 0) return 0;
    return (t->w - 1 + t->chunk_size - 1) / t->chunk_size;
}

int jce_terrain_chunk_count_z(const JceTerrain *t)
{
    if (!t || t->chunk_size <= 0) return 0;
    return (t->h - 1 + t->chunk_size - 1) / t->chunk_size;
}

const float    *jce_terrain_heights(const JceTerrain *t) { return t ? t->heights : NULL; }
const uint32_t *jce_terrain_splat  (const JceTerrain *t) { return t ? t->splat   : NULL; }

/* ───── Sampling ─────────────────────────────────────────────── */

float jce_terrain_sample_height(const JceTerrain *t, float wx, float wz)
{
    if (!t) return 0.0f;
    return sample_h_norm(t, wx, wz) * t->max_height;
}

void jce_terrain_sample_splat(const JceTerrain *t, float wx, float wz,
                              float out_w[4])
{
    if (!out_w) return;
    out_w[0] = 1.0f; out_w[1] = out_w[2] = out_w[3] = 0.0f;
    if (!t) return;
    float fx, fz;
    world_to_uv(t, wx, wz, &fx, &fz);
    if (fx < 0.0f || fz < 0.0f || fx > (float)(t->w - 1) || fz > (float)(t->h - 1))
        return;
    int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
    int x1 = clampi(x0 + 1, 0, t->w - 1);
    int z1 = clampi(z0 + 1, 0, t->h - 1);
    float u = fx - (float)x0;
    float v = fz - (float)z0;
    uint32_t s00 = t->splat[(size_t)z0 * t->w + x0];
    uint32_t s10 = t->splat[(size_t)z0 * t->w + x1];
    uint32_t s01 = t->splat[(size_t)z1 * t->w + x0];
    uint32_t s11 = t->splat[(size_t)z1 * t->w + x1];
    float total = 0.0f;
    for (int c = 0; c < 4; ++c) {
        float a = (float)((s00 >> (c * 8)) & 0xFFu);
        float b = (float)((s10 >> (c * 8)) & 0xFFu);
        float cc= (float)((s01 >> (c * 8)) & 0xFFu);
        float d = (float)((s11 >> (c * 8)) & 0xFFu);
        float top = a * (1.0f - u) + b * u;
        float bot = cc * (1.0f - u) + d * u;
        out_w[c] = top * (1.0f - v) + bot * v;
        total += out_w[c];
    }
    if (total > 0.0001f)
        for (int c = 0; c < 4; ++c) out_w[c] /= total;
    else { out_w[0] = 1.0f; out_w[1] = out_w[2] = out_w[3] = 0.0f; }
}

bool jce_terrain_raycast(const JceTerrain *t,
                         const float origin[3], const float dir[3],
                         float max_dist, float out_hit[3])
{
    if (!t || !origin || !dir) return false;
    /* Step along ray; if origin Y goes from above heightmap to below
     * heightmap, refine with bisection and report hit. */
    float step_world = 0.5f * (t->world_size_x / (float)(t->w - 1)
                             + t->world_size_z / (float)(t->h - 1));
    if (step_world < 0.05f) step_world = 0.05f;
    int   max_steps = (int)(max_dist / step_world) + 2;
    if (max_steps > 4096) max_steps = 4096;
    float prev_t = 0.0f;
    float prev_dy = origin[1] - jce_terrain_sample_height(t, origin[0], origin[2]);
    for (int i = 1; i <= max_steps; ++i) {
        float tt = (float)i * step_world;
        if (tt > max_dist) tt = max_dist;
        float px = origin[0] + dir[0] * tt;
        float py = origin[1] + dir[1] * tt;
        float pz = origin[2] + dir[2] * tt;
        float h  = jce_terrain_sample_height(t, px, pz);
        float dy = py - h;
        if (prev_dy >= 0.0f && dy <= 0.0f) {
            /* Bisect between prev_t and tt. */
            float a = prev_t, b = tt;
            for (int k = 0; k < 16; ++k) {
                float m = 0.5f * (a + b);
                float mx = origin[0] + dir[0] * m;
                float my = origin[1] + dir[1] * m;
                float mz = origin[2] + dir[2] * m;
                float mh = jce_terrain_sample_height(t, mx, mz);
                if (my - mh > 0.0f) a = m; else b = m;
            }
            float m = 0.5f * (a + b);
            if (out_hit) {
                out_hit[0] = origin[0] + dir[0] * m;
                out_hit[1] = origin[1] + dir[1] * m;
                out_hit[2] = origin[2] + dir[2] * m;
            }
            return true;
        }
        prev_t = tt;
        prev_dy = dy;
        if (tt >= max_dist) break;
    }
    return false;
}

/* ───── Mesh build ───────────────────────────────────────────── */

static void chunk_extents(const JceTerrain *t, int cx, int cz,
                          int *x0, int *z0, int *x1, int *z1)
{
    *x0 = cx * t->chunk_size;
    *z0 = cz * t->chunk_size;
    *x1 = *x0 + t->chunk_size;
    *z1 = *z0 + t->chunk_size;
    if (*x1 > t->w - 1) *x1 = t->w - 1;
    if (*z1 > t->h - 1) *z1 = t->h - 1;
}

static int lod_step(int lod) { return lod < 0 ? 1 : (1 << lod); }

void jce_terrain_chunk_mesh_size(const JceTerrain *t, int cx, int cz, int lod,
                                 int *out_v, int *out_i)
{
    int x0, z0, x1, z1;
    if (out_v) *out_v = 0;
    if (out_i) *out_i = 0;
    if (!t) return;
    chunk_extents(t, cx, cz, &x0, &z0, &x1, &z1);
    int step = lod_step(lod);
    int nx = (x1 - x0) / step + 1;
    int nz = (z1 - z0) / step + 1;
    if (nx < 2 || nz < 2) return;
    if (out_v) *out_v = nx * nz;
    if (out_i) *out_i = (nx - 1) * (nz - 1) * 6;
}

static void compute_normal(const JceTerrain *t, int x, int z,
                           float *nx_out, float *ny_out, float *nz_out)
{
    int xm = clampi(x - 1, 0, t->w - 1);
    int xp = clampi(x + 1, 0, t->w - 1);
    int zm = clampi(z - 1, 0, t->h - 1);
    int zp = clampi(z + 1, 0, t->h - 1);
    float dx_world = (float)(xp - xm) * (t->world_size_x / (float)(t->w - 1));
    float dz_world = (float)(zp - zm) * (t->world_size_z / (float)(t->h - 1));
    float hL = t->heights[(size_t)z * t->w + xm] * t->max_height;
    float hR = t->heights[(size_t)z * t->w + xp] * t->max_height;
    float hD = t->heights[(size_t)zm * t->w + x] * t->max_height;
    float hU = t->heights[(size_t)zp * t->w + x] * t->max_height;
    float dHdx = (hR - hL) / (dx_world > 0.001f ? dx_world : 1.0f);
    float dHdz = (hU - hD) / (dz_world > 0.001f ? dz_world : 1.0f);
    float nxv = -dHdx, nyv = 1.0f, nzv = -dHdz;
    float l = sqrtf(nxv * nxv + nyv * nyv + nzv * nzv);
    if (l < 0.0001f) l = 1.0f;
    *nx_out = nxv / l; *ny_out = nyv / l; *nz_out = nzv / l;
}

void jce_terrain_chunk_build_mesh(const JceTerrain *t, int cx, int cz, int lod,
                                  JceTerrainVertex *out_verts, int v_cap,
                                  uint32_t *out_indices,       int i_cap,
                                  int *out_v, int *out_i)
{
    if (out_v) *out_v = 0;
    if (out_i) *out_i = 0;
    if (!t || !out_verts || !out_indices) return;
    int x0, z0, x1, z1;
    chunk_extents(t, cx, cz, &x0, &z0, &x1, &z1);
    int step = lod_step(lod);
    int nx = (x1 - x0) / step + 1;
    int nz = (z1 - z0) / step + 1;
    if (nx < 2 || nz < 2) return;
    if (nx * nz > v_cap) return;
    if ((nx - 1) * (nz - 1) * 6 > i_cap) return;

    float dx_world = t->world_size_x / (float)(t->w - 1);
    float dz_world = t->world_size_z / (float)(t->h - 1);

    int v = 0;
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nx; ++i) {
            int xi = clampi(x0 + i * step, 0, t->w - 1);
            int zi = clampi(z0 + j * step, 0, t->h - 1);
            float wx = (float)xi * dx_world;
            float wz = (float)zi * dz_world;
            float wy = t->heights[(size_t)zi * t->w + xi] * t->max_height;
            float nx_, ny_, nz_;
            compute_normal(t, xi, zi, &nx_, &ny_, &nz_);
            JceTerrainVertex *V = &out_verts[v++];
            V->px = wx; V->py = wy; V->pz = wz;
            V->nx = nx_; V->ny = ny_; V->nz = nz_;
            V->u = wx / t->world_size_x;
            V->v = wz / t->world_size_z;
        }
    }
    int idx = 0;
    for (int j = 0; j < nz - 1; ++j) {
        for (int i = 0; i < nx - 1; ++i) {
            uint32_t a = (uint32_t)( j      * nx + i);
            uint32_t b = (uint32_t)( j      * nx + i + 1);
            uint32_t c = (uint32_t)((j + 1) * nx + i);
            uint32_t d = (uint32_t)((j + 1) * nx + i + 1);
            out_indices[idx++] = a; out_indices[idx++] = c; out_indices[idx++] = b;
            out_indices[idx++] = b; out_indices[idx++] = c; out_indices[idx++] = d;
        }
    }
    if (out_v) *out_v = v;
    if (out_i) *out_i = idx;
}

/* ───── Brushes ──────────────────────────────────────────────── */

static float gaussian_falloff(float dist, float radius)
{
    if (radius <= 0.0001f) return 0.0f;
    float r = dist / radius;
    if (r >= 1.0f) return 0.0f;
    /* Smooth bell: cos(r * pi/2)^2 */
    float c = cosf(r * 1.5707963f);
    return c * c;
}

static void brush_grid_extents(const JceTerrain *t, float wx, float wz,
                               float radius_world,
                               int *x0, int *z0, int *x1, int *z1)
{
    float dx_world = t->world_size_x / (float)(t->w - 1);
    float dz_world = t->world_size_z / (float)(t->h - 1);
    int   ix      = (int)(wx / dx_world);
    int   iz      = (int)(wz / dz_world);
    int   rx      = (int)ceilf(radius_world / dx_world);
    int   rz      = (int)ceilf(radius_world / dz_world);
    *x0 = clampi(ix - rx, 0, t->w - 1);
    *z0 = clampi(iz - rz, 0, t->h - 1);
    *x1 = clampi(ix + rx, 0, t->w - 1);
    *z1 = clampi(iz + rz, 0, t->h - 1);
}

void jce_terrain_sculpt_apply(JceTerrain *t,
                              JceTerrainSculptMode mode,
                              float wx, float wz,
                              float radius_world, float strength,
                              float dt)
{
    if (!t || radius_world <= 0.0f || dt <= 0.0f) return;
    int x0, z0, x1, z1;
    brush_grid_extents(t, wx, wz, radius_world, &x0, &z0, &x1, &z1);
    float dx_world = t->world_size_x / (float)(t->w - 1);
    float dz_world = t->world_size_z / (float)(t->h - 1);
    /* Sample average for flatten. */
    float flatten_target = 0.0f;
    if (mode == JCE_TERRAIN_SCULPT_FLATTEN)
        flatten_target = sample_h_norm(t, wx, wz);

    float amount = strength * dt;
    /* Convert "world strength" to normalized space. */
    float norm_amount = (t->max_height > 0.0001f) ? (amount / t->max_height) : amount;

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            float gx = (float)x * dx_world;
            float gz = (float)z * dz_world;
            float dx = gx - wx, dz = gz - wz;
            float dist = sqrtf(dx * dx + dz * dz);
            float w = gaussian_falloff(dist, radius_world);
            if (w <= 0.0001f) continue;
            size_t idx = (size_t)z * t->w + x;
            float v = t->heights[idx];
            switch (mode) {
            case JCE_TERRAIN_SCULPT_RAISE:
                v += norm_amount * w; break;
            case JCE_TERRAIN_SCULPT_LOWER:
                v -= norm_amount * w; break;
            case JCE_TERRAIN_SCULPT_FLATTEN:
                v += (flatten_target - v) * w * clampf(amount * 4.0f, 0.0f, 1.0f);
                break;
            case JCE_TERRAIN_SCULPT_SMOOTH: {
                /* 3x3 average. */
                float sum = 0.0f; int cnt = 0;
                for (int j = -1; j <= 1; ++j) {
                    int zz = clampi(z + j, 0, t->h - 1);
                    for (int i = -1; i <= 1; ++i) {
                        int xx = clampi(x + i, 0, t->w - 1);
                        sum += t->heights[(size_t)zz * t->w + xx];
                        ++cnt;
                    }
                }
                float avg = sum / (float)cnt;
                v += (avg - v) * w * clampf(amount * 4.0f, 0.0f, 1.0f);
                break;
            }
            }
            t->heights[idx] = clampf(v, 0.0f, 1.0f);
        }
    }
}

void jce_terrain_splat_paint(JceTerrain *t, int layer,
                             float wx, float wz,
                             float radius_world, float strength,
                             float dt)
{
    if (!t || layer < 0 || layer > 3 || radius_world <= 0.0f || dt <= 0.0f) return;
    int x0, z0, x1, z1;
    brush_grid_extents(t, wx, wz, radius_world, &x0, &z0, &x1, &z1);
    float dx_world = t->world_size_x / (float)(t->w - 1);
    float dz_world = t->world_size_z / (float)(t->h - 1);
    float amount = clampf(strength * dt, 0.0f, 1.0f);

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            float gx = (float)x * dx_world;
            float gz = (float)z * dz_world;
            float dx = gx - wx, dz = gz - wz;
            float dist = sqrtf(dx * dx + dz * dz);
            float w = gaussian_falloff(dist, radius_world);
            if (w <= 0.0001f) continue;
            size_t idx  = (size_t)z * t->w + x;
            uint32_t s  = t->splat[idx];
            float vals[4];
            for (int c = 0; c < 4; ++c)
                vals[c] = (float)((s >> (c * 8)) & 0xFFu) / 255.0f;
            float add = amount * w;
            vals[layer] = clampf(vals[layer] + add, 0.0f, 1.0f);
            float sum = vals[0] + vals[1] + vals[2] + vals[3];
            if (sum > 0.0001f)
                for (int c = 0; c < 4; ++c) vals[c] /= sum;
            uint32_t out = 0;
            for (int c = 0; c < 4; ++c) {
                uint32_t b = (uint32_t)(clampf(vals[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                out |= (b & 0xFFu) << (c * 8);
            }
            t->splat[idx] = out;
        }
    }
}
