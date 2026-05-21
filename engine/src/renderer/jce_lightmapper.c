/*
 * jce_lightmapper.c  CPU direct-light baker (Sprint 4 #15).
 *
 * For each receiver texel:
 *   1) Compute world position via origin + u*u_axis + v*v_axis.
 *   2) For each light, evaluate L (direction) and visibility V via
 *      ray-vs-occluder intersection on the line segment from texel to
 *      the light position (point/spot) or to a far point along the
 *      light's anti-direction (directional).
 *   3) Accumulate Lambertian: max(0, dot(N, L)) * attenuation * color.
 *   4) Add ambient and write tonemapped sRGB byte to RGBA8 output.
 *
 * Occluders are box/sphere primitives; this is sufficient for proxy
 * occlusion and matches what the editor was already feeding the AO
 * baker.  Real CSG / triangle-soup baking is deferred to a future
 * Embree integration.
 */

#include <jce/renderer/jce_lightmapper.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>

static void v3_sub(float *o, const float *a, const float *b)
{ o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }

static float v3_dot(const float *a, const float *b)
{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static float v3_len(const float *a)
{ return sqrtf(v3_dot(a, a)); }

static void v3_norm(float *o)
{
    float l = v3_len(o);
    if (l > 1e-8f) { o[0]/=l; o[1]/=l; o[2]/=l; }
}

/* ── Ray-vs-occluder helpers (segment from p0 to p1). ───────────────── */

static int seg_hit_sphere(const float *p0, const float *p1,
                          const float *c, float r)
{
    float d[3]; v3_sub(d, p1, p0);
    float m[3]; v3_sub(m, p0, c);
    float a = v3_dot(d, d);
    float b = v3_dot(m, d);
    float cc = v3_dot(m, m) - r*r;
    float disc = b*b - a*cc;
    if (disc < 0) return 0;
    float sq = sqrtf(disc);
    float t0 = (-b - sq) / a;
    float t1 = (-b + sq) / a;
    if (t0 > 1.0f || t1 < 0.0f) return 0;
    return 1;
}

static int seg_hit_aabb(const float *p0, const float *p1,
                        const float *c, const float *e)
{
    float bmn[3] = { c[0]-e[0], c[1]-e[1], c[2]-e[2] };
    float bmx[3] = { c[0]+e[0], c[1]+e[1], c[2]+e[2] };
    float d[3]; v3_sub(d, p1, p0);
    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d[i]) < 1e-8f) {
            if (p0[i] < bmn[i] || p0[i] > bmx[i]) return 0;
        } else {
            float inv = 1.0f / d[i];
            float t1 = (bmn[i] - p0[i]) * inv;
            float t2 = (bmx[i] - p0[i]) * inv;
            if (t1 > t2) { float tmp=t1; t1=t2; t2=tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return 0;
        }
    }
    return 1;
}

static int occluded(const float *p0, const float *p1,
                    const JceLightmapOccluder *occ, int n)
{
    for (int i = 0; i < n; ++i) {
        if (occ[i].kind == JCE_LM_OCC_SPHERE) {
            if (seg_hit_sphere(p0, p1, occ[i].center, occ[i].extent_or_radius[0]))
                return 1;
        } else {
            if (seg_hit_aabb(p0, p1, occ[i].center, occ[i].extent_or_radius))
                return 1;
        }
    }
    return 0;
}

static uint8_t to_srgb_u8(float lin)
{
    if (lin <= 0) return 0;
    if (lin >= 1) return 255;
    float s = (lin <= 0.0031308f) ? lin*12.92f
                                  : 1.055f * powf(lin, 1.0f/2.4f) - 0.055f;
    int v = (int)(s * 255.0f + 0.5f);
    if (v < 0) v = 0; if (v > 255) v = 255;
    return (uint8_t)v;
}

JCE_API int JCE_CALL
jce_lightmapper_bake_direct(const JceLightmapBakeDesc      *desc,
                            const JceLightmapOccluder *occ, int occ_count,
                            const JceLightmapLight    *lights, int light_count,
                            uint8_t                  *out_rgba)
{
    if (!desc || !out_rgba || desc->width <= 0 || desc->height <= 0) return -1;

    float N[3] = { desc->normal[0], desc->normal[1], desc->normal[2] };
    v3_norm(N);

    int W = desc->width, H = desc->height;
    for (int y = 0; y < H; ++y) {
        float fv = (y + 0.5f) / (float)H;
        for (int x = 0; x < W; ++x) {
            float fu = (x + 0.5f) / (float)W;
            float P[3] = {
                desc->origin[0] + fu * desc->u_axis[0] + fv * desc->v_axis[0],
                desc->origin[1] + fu * desc->u_axis[1] + fv * desc->v_axis[1],
                desc->origin[2] + fu * desc->u_axis[2] + fv * desc->v_axis[2],
            };
            float Eacc[3] = { desc->ambient[0], desc->ambient[1], desc->ambient[2] };

            for (int li = 0; li < light_count; ++li) {
                const JceLightmapLight *L = &lights[li];
                float Ldir[3], dist = 0;
                if (L->kind == JCE_LM_LIGHT_DIRECTIONAL) {
                    Ldir[0] = -L->direction[0];
                    Ldir[1] = -L->direction[1];
                    Ldir[2] = -L->direction[2];
                    dist = 1e6f;
                } else {
                    v3_sub(Ldir, L->position, P);
                    dist = v3_len(Ldir);
                    if (dist > 1e-6f) { Ldir[0]/=dist; Ldir[1]/=dist; Ldir[2]/=dist; }
                    if (L->range > 0 && dist > L->range) continue;
                }
                float ndotl = v3_dot(N, Ldir);
                if (ndotl <= 0) continue;

                /* Spotlight cone test. */
                if (L->kind == JCE_LM_LIGHT_SPOT) {
                    float toFrag[3] = { -Ldir[0], -Ldir[1], -Ldir[2] };
                    float c = v3_dot(toFrag, L->direction);
                    if (c < L->cone_cos) continue;
                }

                /* Visibility ray. */
                float endP[3];
                if (L->kind == JCE_LM_LIGHT_DIRECTIONAL) {
                    endP[0] = P[0] + Ldir[0] * 1e4f;
                    endP[1] = P[1] + Ldir[1] * 1e4f;
                    endP[2] = P[2] + Ldir[2] * 1e4f;
                } else {
                    endP[0] = L->position[0]; endP[1] = L->position[1]; endP[2] = L->position[2];
                }
                /* Bias origin away from receiver to avoid self-shadowing. */
                float startP[3] = { P[0] + N[0]*1e-3f, P[1] + N[1]*1e-3f, P[2] + N[2]*1e-3f };
                if (occluded(startP, endP, occ, occ_count)) continue;

                float atten = 1.0f;
                if (L->kind != JCE_LM_LIGHT_DIRECTIONAL && L->range > 0) {
                    float t = dist / L->range;
                    atten = 1.0f - t; if (atten < 0) atten = 0;
                    atten *= atten;
                }
                float k = ndotl * atten * L->intensity;
                Eacc[0] += L->color[0] * k;
                Eacc[1] += L->color[1] * k;
                Eacc[2] += L->color[2] * k;
            }

            uint8_t *o = &out_rgba[(y*W + x) * 4];
            o[0] = to_srgb_u8(Eacc[0]);
            o[1] = to_srgb_u8(Eacc[1]);
            o[2] = to_srgb_u8(Eacc[2]);
            o[3] = 255;
        }
    }
    /* Suppress unused warning when shadow_samples is 0/1 (reserved for future MSAA). */
    (void)desc->shadow_samples;
    return 0;
}

/* ── SH9 light probe bake ──────────────────────────────────────────── */

/* Real spherical harmonics (L0 + L1 + L2): basis evaluation for direction d. */
static void sh9_eval(float *sh, const float *d)
{
    float x = d[0], y = d[1], z = d[2];
    sh[0] =  0.282095f;                          /* L0,0 */
    sh[1] =  0.488603f * y;                      /* L1,-1 */
    sh[2] =  0.488603f * z;                      /* L1, 0 */
    sh[3] =  0.488603f * x;                      /* L1, 1 */
    sh[4] =  1.092548f * x * y;                  /* L2,-2 */
    sh[5] =  1.092548f * y * z;                  /* L2,-1 */
    sh[6] =  0.315392f * (3.0f * z*z - 1.0f);   /* L2, 0 */
    sh[7] =  1.092548f * x * z;                  /* L2, 1 */
    sh[8] =  0.546274f * (x*x - y*y);            /* L2, 2 */
}

/* Evaluate irradiance from all lights at point P in direction L (hemisphere
   sample direction). Returns irradiance energy per channel. */
static void eval_light_irradiance(const float *P,
                                  const float *Ldir,
                                  const JceLightmapOccluder *occ, int occ_count,
                                  const JceLightmapLight *lights, int light_count,
                                  float *out_rgb)
{
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;

    for (int li = 0; li < light_count; ++li) {
        const JceLightmapLight *L = &lights[li];

        /* Check hemisphere agreement: sample dir vs light dir. */
        float light_dir[3];
        if (L->kind == JCE_LM_LIGHT_DIRECTIONAL) {
            light_dir[0] = -L->direction[0];
            light_dir[1] = -L->direction[1];
            light_dir[2] = -L->direction[2];
        } else {
            float tmp[3];
            v3_sub(tmp, L->position, P);
            float d = v3_len(tmp);
            if (d < 1e-8f) continue;
            light_dir[0] = tmp[0]/d; light_dir[1] = tmp[1]/d; light_dir[2] = tmp[2]/d;
        }

        float ndotl = v3_dot(Ldir, light_dir);
        if (ndotl <= 0.0f) continue;

        /* Range and spot checks. */
        float dist = 0.0f;
        if (L->kind != JCE_LM_LIGHT_DIRECTIONAL) {
            float tmp[3]; v3_sub(tmp, L->position, P);
            dist = v3_len(tmp);
            if (L->range > 0 && dist > L->range) continue;
        }
        if (L->kind == JCE_LM_LIGHT_SPOT) {
            float toFrag[3] = { -light_dir[0], -light_dir[1], -light_dir[2] };
            if (v3_dot(toFrag, L->direction) < L->cone_cos) continue;
        }

        /* Visibility segment. */
        float startP[3] = { P[0] + Ldir[0]*1e-3f,
                            P[1] + Ldir[1]*1e-3f,
                            P[2] + Ldir[2]*1e-3f };
        float endP[3];
        if (L->kind == JCE_LM_LIGHT_DIRECTIONAL) {
            endP[0] = P[0] + light_dir[0]*1e4f;
            endP[1] = P[1] + light_dir[1]*1e4f;
            endP[2] = P[2] + light_dir[2]*1e4f;
        } else {
            endP[0] = L->position[0];
            endP[1] = L->position[1];
            endP[2] = L->position[2];
        }
        if (occluded(startP, endP, occ, occ_count)) continue;

        float atten = 1.0f;
        if (L->kind != JCE_LM_LIGHT_DIRECTIONAL && L->range > 0) {
            float t = dist / L->range;
            atten = 1.0f - t; if (atten < 0.0f) atten = 0.0f;
            atten *= atten;
        }

        float k = ndotl * atten * L->intensity;
        out_rgb[0] += L->color[0] * k;
        out_rgb[1] += L->color[1] * k;
        out_rgb[2] += L->color[2] * k;
    }
}

/*
 * LCG pseudo-random float in [0, 1) — lightweight, avoids rand() which is
 * not thread-safe on all platforms and requires <stdlib.h> globally.
 */
static float lcg_randf(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return (float)(*state >> 8) * (1.0f / 16777216.0f);
}

/* Cosine-weighted hemisphere sample in the canonical (0,0,1) space then
   rotated to the surface normal frame.  normal must be unit-length. */
static void cosine_sample_hemisphere(float *out, const float *normal,
                                     unsigned *rng)
{
    float u1 = lcg_randf(rng);
    float u2 = lcg_randf(rng);
    /* Malley's method: uniform disk then project up. */
    float r   = sqrtf(u1);
    float phi = 6.28318530f * u2;
    float lx  = r * cosf(phi);
    float ly  = r * sinf(phi);
    float lz  = sqrtf(1.0f - u1 < 0.0f ? 0.0f : 1.0f - u1);

    /* Build ONB around normal (Duff et al. 2017). */
    float nx = normal[0], ny = normal[1], nz = normal[2];
    float sign = (nz >= 0.0f) ? 1.0f : -1.0f;
    float a  = -1.0f / (sign + nz);
    float b  = nx * ny * a;
    float tx = 1.0f + sign * nx * nx * a;
    float ty = sign * b;
    float tz = -sign * nx;
    float bx = b;
    float by = sign + ny * ny * a;
    float bz = -ny;

    out[0] = lx*tx + ly*bx + lz*nx;
    out[1] = lx*ty + ly*by + lz*ny;
    out[2] = lx*tz + ly*bz + lz*nz;
}

int JCE_CALL
jce_lightmapper_bake_sh9(const float          (*positions)[3],
                         int                   probe_count,
                         const JceLightmapOccluder *occ,  int occ_count,
                         const JceLightmapLight    *lights, int light_count,
                         int                   sample_count,
                         float               (*out_sh9)[9][3])
{
    if (!positions || probe_count <= 0 || !out_sh9 || sample_count <= 0)
        return -1;

    /* Use upward hemisphere (Y+) per probe as the dominant normal; for a
       full-sphere probe we sample the whole sphere by using both +Y and -Y
       halves (alternating). */
    const float up[3]   = { 0.0f, 1.0f, 0.0f };
    const float down[3] = { 0.0f, -1.0f, 0.0f };

    for (int pi = 0; pi < probe_count; ++pi) {
        float acc[9][3];
        for (int i = 0; i < 9; ++i) {
            acc[i][0] = acc[i][1] = acc[i][2] = 0.0f;
        }

        unsigned rng = (unsigned)(pi * 2654435761u + 1u);
        float inv_n = 1.0f / (float)sample_count;
        const float *P = positions[pi];

        for (int si = 0; si < sample_count; ++si) {
            /* Alternate hemisphere to approximate full-sphere coverage. */
            const float *normal = (si & 1) ? down : up;
            float sdir[3];
            cosine_sample_hemisphere(sdir, normal, &rng);

            float irr[3];
            eval_light_irradiance(P, sdir, occ, occ_count, lights, light_count, irr);

            float basis[9];
            sh9_eval(basis, sdir);

            /* SH projection weight: π (cosine-weighted PDF = cos/π). */
            float w = 3.14159265f * inv_n;
            for (int c = 0; c < 9; ++c) {
                acc[c][0] += irr[0] * basis[c] * w;
                acc[c][1] += irr[1] * basis[c] * w;
                acc[c][2] += irr[2] * basis[c] * w;
            }
        }

        for (int c = 0; c < 9; ++c) {
            out_sh9[pi][c][0] = acc[c][0];
            out_sh9[pi][c][1] = acc[c][1];
            out_sh9[pi][c][2] = acc[c][2];
        }
    }
    return 0;
}
