/*
 * jce_water.c -- Pure Gerstner / sum-of-sines water surface model.
 *
 * See jce_water.h for the exact wave equation (the single source of truth the
 * unit test and the next-phase water vertex shader must both match).  Pure
 * math; depends only on the C math library.  No allocation, no globals, no RNG,
 * so a given (waves, x, z, t) is fully reproducible across runs and platforms.
 */

#include <jce/middleware/scene/jce_water.h>

#include <math.h>

#define WATER_PI     3.14159265358979323846f
#define WATER_TWO_PI 6.28318530717958647692f

/* Normalize a 2D direction in place; returns 0 (and leaves d unchanged) for a
 * zero-length input so the caller can skip the wave. */
static int water_norm2(float *dx, float *dz)
{
    const float len2 = (*dx) * (*dx) + (*dz) * (*dz);
    if (len2 <= 1e-12f) return 0;
    const float inv = 1.0f / sqrtf(len2);
    *dx *= inv;
    *dz *= inv;
    return 1;
}

float JCE_CALL jce_water_sample_height(const JceWaterWave *waves, int n,
                                       float base_y, float x, float z, float t)
{
    if (!waves || n <= 0) return base_y;

    float y = base_y;
    for (int i = 0; i < n; ++i) {
        const JceWaterWave w = waves[i];
        if (w.wavelength <= 0.0f) continue;
        float dx = w.dir_x, dz = w.dir_z;
        if (!water_norm2(&dx, &dz)) continue;

        const float k     = WATER_TWO_PI / w.wavelength; /* angular wavenumber */
        const float omega = k * w.speed;                 /* temporal frequency */
        const float phase = k * (dx * x + dz * z) + omega * t;
        y += w.amplitude * sinf(phase);
    }
    return y;
}

void JCE_CALL jce_water_sample_displacement(const JceWaterWave *waves, int n,
                                            float base_y, float x, float z,
                                            float t, float *out_xyz)
{
    if (!out_xyz) return;

    float px = x, py = base_y, pz = z;
    if (waves && n > 0) {
        for (int i = 0; i < n; ++i) {
            const JceWaterWave w = waves[i];
            if (w.wavelength <= 0.0f) continue;
            float dx = w.dir_x, dz = w.dir_z;
            if (!water_norm2(&dx, &dz)) continue;

            const float k     = WATER_TWO_PI / w.wavelength;
            const float omega = k * w.speed;
            const float phase = k * (dx * x + dz * z) + omega * t;
            const float c     = cosf(phase);
            const float s     = sinf(phase);
            const float qa    = w.steepness * w.amplitude;

            px += qa * dx * c;       /* horizontal Gerstner roll */
            pz += qa * dz * c;
            py += w.amplitude * s;   /* vertical sine */
        }
    }
    out_xyz[0] = px;
    out_xyz[1] = py;
    out_xyz[2] = pz;
}

void JCE_CALL jce_water_sample_normal(const JceWaterWave *waves, int n,
                                      float x, float z, float t, float *out_xyz)
{
    if (!out_xyz) return;

    float nx = 0.0f, nz = 0.0f, ny = 1.0f;
    if (waves && n > 0) {
        for (int i = 0; i < n; ++i) {
            const JceWaterWave w = waves[i];
            if (w.wavelength <= 0.0f) continue;
            float dx = w.dir_x, dz = w.dir_z;
            if (!water_norm2(&dx, &dz)) continue;

            const float k     = WATER_TWO_PI / w.wavelength;
            const float omega = k * w.speed;
            const float phase = k * (dx * x + dz * z) + omega * t;
            const float ka    = k * w.amplitude;
            const float c     = cosf(phase);
            const float s     = sinf(phase);

            nx -= dx * ka * c;
            nz -= dz * ka * c;
            ny -= w.steepness * ka * s;
        }
    }

    float len2 = nx * nx + ny * ny + nz * nz;
    if (len2 <= 1e-12f) {        /* degenerate -> straight up */
        out_xyz[0] = 0.0f;
        out_xyz[1] = 1.0f;
        out_xyz[2] = 0.0f;
        return;
    }
    const float inv = 1.0f / sqrtf(len2);
    out_xyz[0] = nx * inv;
    out_xyz[1] = ny * inv;
    out_xyz[2] = nz * inv;
}

float JCE_CALL jce_water_buoyancy_force(float submersion, float vel_y,
                                        float strength, float drag)
{
    if (submersion <= 0.0f) return 0.0f;   /* fully above water -> no force */
    if (strength < 0.0f) strength = 0.0f;
    if (drag     < 0.0f) drag     = 0.0f;

    const float f_buoy = strength * submersion;            /* upward (+Y)      */
    const float f_drag = -drag * vel_y * submersion;       /* opposes vert vel */
    return f_buoy + f_drag;
}

/* ── Inverse-displacement surface query (Gerstner) ──────────────────
 *
 * jce_water_sample_height returns only the vertical sine sum.  vs_water.sc
 * ALSO rolls the vertex horizontally by the Gerstner steepness term, so for
 * steepness > 0 the surface point at world XZ is not the one that sampler
 * returns -- it is the one authored at some other XZ that got rolled to here.
 * A body floating on the naive answer sits at visibly the wrong place on a
 * steep wave, worst at crests.
 *
 * Same fix as the FFT path: solve  x + horizontalRoll(x) = p  by fixed-point
 * iteration, then evaluate the height there.  Converges while Q*k*A < 1, which
 * is also the condition that keeps a Gerstner wave from self-intersecting, so
 * a surface too steep for this to converge is a surface that was already
 * geometrically invalid. */
float JCE_CALL jce_water_sample_height_displaced(const JceWaterWave *waves,
                                                 int n, float base_y,
                                                 float x, float z, float t,
                                                 int iterations)
{
    if (!waves || n <= 0) return base_y;
    if (iterations < 1)  iterations = 1;
    if (iterations > 16) iterations = 16;

    float wx = x, wz = z;
    for (int it = 0; it < iterations; ++it) {
        /* Horizontal roll contributed by the authored point (wx, wz). */
        float rx = 0.0f, rz = 0.0f;
        for (int i = 0; i < n; ++i) {
            const JceWaterWave w = waves[i];
            if (w.wavelength <= 0.0f) continue;
            float dx = w.dir_x, dz = w.dir_z;
            if (!water_norm2(&dx, &dz)) continue;

            const float k     = WATER_TWO_PI / w.wavelength;
            const float omega = k * w.speed;
            const float phase = k * (dx * wx + dz * wz) + omega * t;
            const float q     = w.steepness * w.amplitude;
            const float c     = cosf(phase);
            rx += q * dx * c;
            rz += q * dz * c;
        }
        wx = x - rx;
        wz = z - rz;
    }

    return jce_water_sample_height(waves, n, base_y, wx, wz, t);
}

/* ── Concentric-ring ocean geometry ────────────────────────────────────
 * See jce_water.h. */

bool JCE_CALL jce_water_ring_mesh_size(int rings, int segments,
                                       uint32_t *out_verts,
                                       uint32_t *out_indices)
{
    if (out_verts)   *out_verts   = 0u;
    if (out_indices) *out_indices = 0u;
    if (rings < 1 || segments < 3) return false;
    if (rings > 64 || segments > 512) return false;

    /* One centre vertex, then `segments` per ring. */
    const uint32_t verts = 1u + (uint32_t)rings * (uint32_t)segments;
    /* The cap is a fan of `segments` triangles; each subsequent gap between
     * two rings is a quad strip of `segments` quads = 2*segments triangles. */
    const uint32_t tris = (uint32_t)segments
                        + (uint32_t)(rings - 1) * (uint32_t)segments * 2u;

    if (out_verts)   *out_verts   = verts;
    if (out_indices) *out_indices = tris * 3u;
    return true;
}

bool JCE_CALL jce_water_ring_build(int rings, int segments,
                                   float inner, float outer,
                                   JceWaterRingVertex *out_verts,
                                   uint32_t *out_indices)
{
    if (!out_verts || !out_indices) return false;
    if (!jce_water_ring_mesh_size(rings, segments, NULL, NULL)) return false;
    if (!(inner > 0.0f) || !(outer > inner)) return false;

    /* Centre vertex: without it the innermost ring is a hole, and the hole
     * sits directly under the camera where it is most visible. */
    out_verts[0].x = 0.0f;
    out_verts[0].z = 0.0f;
    out_verts[0].radius = 0.0f;

    /* Geometric radii: r_i = inner * (outer/inner)^(i/(rings-1)).  Geometric
     * rather than linear because perspective shrinks by ratio, not by
     * difference -- linear spacing would waste rings near the camera and leave
     * the horizon under-tessellated. */
    const float ratio = outer / inner;
    for (int r = 0; r < rings; ++r) {
        const float t = (rings > 1) ? (float)r / (float)(rings - 1) : 0.0f;
        const float radius = inner * powf(ratio, t);
        for (int sIdx = 0; sIdx < segments; ++sIdx) {
            const float a = 6.28318530717958647692f *
                            (float)sIdx / (float)segments;
            const uint32_t vi = 1u + (uint32_t)r * (uint32_t)segments +
                                (uint32_t)sIdx;
            out_verts[vi].x = cosf(a) * radius;
            out_verts[vi].z = sinf(a) * radius;
            out_verts[vi].radius = radius;
        }
    }

    uint32_t k = 0;
    /* Cap fan. */
    for (int sIdx = 0; sIdx < segments; ++sIdx) {
        const uint32_t a = 1u + (uint32_t)sIdx;
        const uint32_t b = 1u + (uint32_t)((sIdx + 1) % segments);
        out_indices[k++] = 0u;
        out_indices[k++] = b;
        out_indices[k++] = a;
    }
    /* Ring strips.  Both sides of every boundary use the SAME segment count,
     * so vertices coincide exactly and no T-junction can open. */
    for (int r = 0; r + 1 < rings; ++r) {
        const uint32_t base0 = 1u + (uint32_t)r * (uint32_t)segments;
        const uint32_t base1 = base0 + (uint32_t)segments;
        for (int sIdx = 0; sIdx < segments; ++sIdx) {
            const uint32_t s0 = (uint32_t)sIdx;
            const uint32_t s1 = (uint32_t)((sIdx + 1) % segments);
            const uint32_t i00 = base0 + s0, i01 = base0 + s1;
            const uint32_t i10 = base1 + s0, i11 = base1 + s1;
            out_indices[k++] = i00; out_indices[k++] = i01; out_indices[k++] = i10;
            out_indices[k++] = i10; out_indices[k++] = i01; out_indices[k++] = i11;
        }
    }
    return true;
}
