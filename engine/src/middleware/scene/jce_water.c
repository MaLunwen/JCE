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
