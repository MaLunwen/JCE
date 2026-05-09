/*
 * jce_gi_probes.c  SH3 light-probe volume.
 *
 * Storage = flat float array of `grid_x * grid_y * grid_z * 27`
 * coefficients.  Cell index is computed as
 *   idx = ((z * grid_y) + y) * grid_x + x
 * so X is the fastest axis — matches the upload order most GPU
 * volume textures expect.
 */

#include <jce/renderer/jce_gi_probes.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "gi_probes"
#define COEFFS_PER_CELL (JCE_SH_BANDS * 3)

struct JceLightProbeVolume {
    jce_vec3 box_min;
    jce_vec3 box_max;
    uint32_t gx, gy, gz;
    float   *coeffs;     /* gx*gy*gz*27 */
};

/* ── Lifecycle ────────────────────────────────────────────────────── */

JceLightProbeVolume *jce_gi_probes_create(jce_vec3 box_min, jce_vec3 box_max,
                                           uint32_t gx, uint32_t gy, uint32_t gz)
{
    if (gx < 2 || gy < 2 || gz < 2) {
        LOG_ERROR(LOG_TAG, "grid dims must be ≥2 (got %u %u %u)", gx, gy, gz);
        return NULL;
    }
    JceLightProbeVolume *v = (JceLightProbeVolume *)JCE_CALLOC(1, sizeof(*v));
    if (!v) return NULL;
    v->box_min = box_min;
    v->box_max = box_max;
    v->gx = gx; v->gy = gy; v->gz = gz;
    size_t n = (size_t)gx * gy * gz * COEFFS_PER_CELL;
    v->coeffs = (float *)JCE_CALLOC(n, sizeof(float));
    if (!v->coeffs) { JCE_FREE(v); return NULL; }
    return v;
}

void jce_gi_probes_destroy(JceLightProbeVolume *v)
{
    if (!v) return;
    JCE_FREE(v->coeffs);
    JCE_FREE(v);
}

jce_vec3 jce_gi_probes_box_min(const JceLightProbeVolume *v)
{ return v ? v->box_min : (jce_vec3){0, 0, 0}; }
jce_vec3 jce_gi_probes_box_max(const JceLightProbeVolume *v)
{ return v ? v->box_max : (jce_vec3){0, 0, 0}; }
uint32_t jce_gi_probes_grid_x(const JceLightProbeVolume *v) { return v ? v->gx : 0; }
uint32_t jce_gi_probes_grid_y(const JceLightProbeVolume *v) { return v ? v->gy : 0; }
uint32_t jce_gi_probes_grid_z(const JceLightProbeVolume *v) { return v ? v->gz : 0; }

/* ── Cell access ──────────────────────────────────────────────────── */

static size_t cell_offset(const JceLightProbeVolume *v,
                          uint32_t x, uint32_t y, uint32_t z)
{
    return (((size_t)z * v->gy + y) * v->gx + x) * COEFFS_PER_CELL;
}

void jce_gi_probes_set_cell(JceLightProbeVolume *v,
                             uint32_t x, uint32_t y, uint32_t z,
                             const float *sh_rgb)
{
    if (!v || !sh_rgb) return;
    if (x >= v->gx || y >= v->gy || z >= v->gz) return;
    memcpy(&v->coeffs[cell_offset(v, x, y, z)], sh_rgb,
           COEFFS_PER_CELL * sizeof(float));
}

void jce_gi_probes_get_cell(const JceLightProbeVolume *v,
                             uint32_t x, uint32_t y, uint32_t z,
                             float *out_sh_rgb)
{
    if (!v || !out_sh_rgb) return;
    if (x >= v->gx || y >= v->gy || z >= v->gz) {
        memset(out_sh_rgb, 0, COEFFS_PER_CELL * sizeof(float));
        return;
    }
    memcpy(out_sh_rgb, &v->coeffs[cell_offset(v, x, y, z)],
           COEFFS_PER_CELL * sizeof(float));
}

/* ── Trilinear sampling ──────────────────────────────────────────── */

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void jce_gi_probes_sample(const JceLightProbeVolume *v,
                           jce_vec3 world_pos, float *out)
{
    if (!v || !out) return;
    /* Normalise position into [0, gx-1] etc. */
    jce_vec3 ext;
    ext.x = v->box_max.x - v->box_min.x;
    ext.y = v->box_max.y - v->box_min.y;
    ext.z = v->box_max.z - v->box_min.z;
    float inv_x = ext.x > 0.0f ? (float)(v->gx - 1) / ext.x : 0.0f;
    float inv_y = ext.y > 0.0f ? (float)(v->gy - 1) / ext.y : 0.0f;
    float inv_z = ext.z > 0.0f ? (float)(v->gz - 1) / ext.z : 0.0f;

    float fx = clamp01((world_pos.x - v->box_min.x) / (ext.x > 0 ? ext.x : 1.0f)) * (v->gx - 1);
    float fy = clamp01((world_pos.y - v->box_min.y) / (ext.y > 0 ? ext.y : 1.0f)) * (v->gy - 1);
    float fz = clamp01((world_pos.z - v->box_min.z) / (ext.z > 0 ? ext.z : 1.0f)) * (v->gz - 1);
    (void)inv_x; (void)inv_y; (void)inv_z;

    uint32_t x0 = (uint32_t)floorf(fx);
    uint32_t y0 = (uint32_t)floorf(fy);
    uint32_t z0 = (uint32_t)floorf(fz);
    uint32_t x1 = x0 + 1u; if (x1 >= v->gx) x1 = v->gx - 1u;
    uint32_t y1 = y0 + 1u; if (y1 >= v->gy) y1 = v->gy - 1u;
    uint32_t z1 = z0 + 1u; if (z1 >= v->gz) z1 = v->gz - 1u;
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    float tz = fz - (float)z0;

    /* 8 corners.  Start with c000 weighted; accumulate the rest. */
    const float *c000 = &v->coeffs[cell_offset(v, x0, y0, z0)];
    const float *c100 = &v->coeffs[cell_offset(v, x1, y0, z0)];
    const float *c010 = &v->coeffs[cell_offset(v, x0, y1, z0)];
    const float *c110 = &v->coeffs[cell_offset(v, x1, y1, z0)];
    const float *c001 = &v->coeffs[cell_offset(v, x0, y0, z1)];
    const float *c101 = &v->coeffs[cell_offset(v, x1, y0, z1)];
    const float *c011 = &v->coeffs[cell_offset(v, x0, y1, z1)];
    const float *c111 = &v->coeffs[cell_offset(v, x1, y1, z1)];

    /* Trilinear weights. */
    float w000 = (1.0f - tx) * (1.0f - ty) * (1.0f - tz);
    float w100 =        tx  * (1.0f - ty) * (1.0f - tz);
    float w010 = (1.0f - tx) *        ty  * (1.0f - tz);
    float w110 =        tx  *        ty  * (1.0f - tz);
    float w001 = (1.0f - tx) * (1.0f - ty) *        tz;
    float w101 =        tx  * (1.0f - ty) *        tz;
    float w011 = (1.0f - tx) *        ty  *        tz;
    float w111 =        tx  *        ty  *        tz;

    for (int i = 0; i < COEFFS_PER_CELL; ++i) {
        out[i] = c000[i] * w000
               + c100[i] * w100
               + c010[i] * w010
               + c110[i] * w110
               + c001[i] * w001
               + c101[i] * w101
               + c011[i] * w011
               + c111[i] * w111;
    }
}

/* ── Persistence ──────────────────────────────────────────────────── */

#define SH3_MAGIC   0x33484553u   /* 'S','H','3','0' little-endian */
#define SH3_VERSION 1u

bool jce_gi_probes_save(const JceLightProbeVolume *v, const char *path)
{
    if (!v || !path) return false;

    size_t coeff_count = (size_t)v->gx * v->gy * v->gz * COEFFS_PER_CELL;
    size_t header = 4 + 4 + 12 + 12 + 12;
    size_t total  = header + coeff_count * sizeof(float);
    uint8_t *buf  = (uint8_t *)JCE_MALLOC(total);
    if (!buf) return false;

    uint8_t *p = buf;
    uint32_t magic = SH3_MAGIC;
    uint32_t ver   = SH3_VERSION;
    memcpy(p, &magic, 4); p += 4;
    memcpy(p, &ver,   4); p += 4;
    memcpy(p, &v->box_min, 12); p += 12;
    memcpy(p, &v->box_max, 12); p += 12;
    uint32_t dims[3] = { v->gx, v->gy, v->gz };
    memcpy(p, dims, 12); p += 12;
    memcpy(p, v->coeffs, coeff_count * sizeof(float));

    bool ok = jce_fs_host_write_all(path, buf, total);
    JCE_FREE(buf);
    return ok;
}

JceLightProbeVolume *jce_gi_probes_load(const char *path)
{
    if (!path) return NULL;
    uint64_t size = 0;
    void *raw = jce_fs_host_read_all(path, &size);
    if (!raw || size < 44) {
        if (raw) jce_fs_buffer_free(raw);
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)raw;
    uint32_t magic = 0, ver = 0;
    memcpy(&magic, p, 4); p += 4;
    memcpy(&ver,   p, 4); p += 4;
    if (magic != SH3_MAGIC || ver != SH3_VERSION) {
        jce_fs_buffer_free(raw);
        return NULL;
    }
    jce_vec3 bmin, bmax;
    memcpy(&bmin, p, 12); p += 12;
    memcpy(&bmax, p, 12); p += 12;
    uint32_t dims[3];
    memcpy(dims, p, 12); p += 12;

    size_t coeff_count = (size_t)dims[0] * dims[1] * dims[2] * COEFFS_PER_CELL;
    if (size < 44 + coeff_count * sizeof(float)) {
        jce_fs_buffer_free(raw);
        return NULL;
    }

    JceLightProbeVolume *v = jce_gi_probes_create(bmin, bmax, dims[0], dims[1], dims[2]);
    if (!v) { jce_fs_buffer_free(raw); return NULL; }
    memcpy(v->coeffs, p, coeff_count * sizeof(float));
    jce_fs_buffer_free(raw);
    return v;
}
