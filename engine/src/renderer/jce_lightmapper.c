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
