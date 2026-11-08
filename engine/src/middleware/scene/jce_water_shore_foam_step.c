#include "jce_water_shore_foam_step.h"
#include <math.h>

static bool size_valid(int side, float spacing)
{
    return side >= 4 && side <= 256 && spacing >= 0.05f && spacing <= 1000.0f;
}

static int distance_squared(int x, int z, int32_t seed, int side)
{
    const int dx = x - seed % side;
    const int dz = z - seed / side;
    return dx * dx + dz * dz;
}

static bool is_edge(const uint8_t *mask, int side, int x, int z)
{
    const int i = z * side + x;
    const bool wet = mask[i] != 0;
    return (x > 0 && (mask[i - 1] != 0) != wet) ||
           (x + 1 < side && (mask[i + 1] != 0) != wet) ||
           (z > 0 && (mask[i - side] != 0) != wet) ||
           (z + 1 < side && (mask[i + side] != 0) != wet);
}

static void flood(int side, int step, const int32_t *src, int32_t *dst)
{
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            int32_t closest = -1;
            int best = side * side * 2;
            for (int dz = -1; dz <= 1; ++dz) {
                const int nz = z + dz * step;
                if (nz < 0 || nz >= side) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx * step;
                    if (nx < 0 || nx >= side) continue;
                    const int32_t seed = src[nz * side + nx];
                    if (seed < 0) continue;
                    const int score = distance_squared(x, z, seed, side);
                    if (score < best || (score == best && seed < closest)) {
                        best = score;
                        closest = seed;
                    }
                }
            }
            dst[z * side + x] = closest;
        }
    }
}

bool jce_water_shore_jfa(int side, float spacing, const uint8_t *wet_mask,
    int32_t *scratch_a, int32_t *scratch_b, JceShoreDistance *output,
    bool *has_shore)
{
    if (!size_valid(side, spacing) || !wet_mask || !scratch_a || !scratch_b ||
        scratch_a == scratch_b || !output || !has_shore) return false;
    *has_shore = false;
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const int i = z * side + x;
            const bool seed = is_edge(wet_mask, side, x, z);
            scratch_a[i] = seed ? i : -1;
            *has_shore = *has_shore || seed;
        }
    }
    if (!*has_shore) {
        const float far = 2.0f * side * spacing;
        for (int i = 0; i < side * side; ++i)
            output[i] = (JceShoreDistance){wet_mask[i] ? far : -far, 0.0f, 0.0f};
        return true;
    }
    int step = 1;
    while (step * 2 < side) step *= 2;
    int32_t *src = scratch_a;
    int32_t *dst = scratch_b;
    for (; step >= 1; step /= 2) {
        flood(side, step, src, dst);
        int32_t *tmp = src;
        src = dst;
        dst = tmp;
    }
    flood(side, 1, src, dst);
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const int i = z * side + x;
            const int32_t seed = dst[i];
            if (seed < 0) return false;
            const float dx = (float)(seed % side - x);
            const float dz = (float)(seed / side - z);
            const float norm = sqrtf(dx * dx + dz * dz);
            const float inv = norm > 0.0f ? 1.0f / norm : 0.0f;
            output[i] = (JceShoreDistance){
                (wet_mask[i] ? norm : -norm) * spacing, dx * inv, dz * inv,
            };
        }
    }
    return true;
}

static double clamp_unit(double value)
{
    return fmin(1.0, fmax(0.0, value));
}

static double smooth_unit(double value)
{
    const double t = clamp_unit(value);
    return t * t * (3.0 - 2.0 * t);
}

static float masked_sample(const float *density, const JceWaterShoreCell *flow,
                           int side, int x, int z)
{
    if (x < 0 || x >= side || z < 0 || z >= side) return 0.0f;
    const int i = z * side + x;
    return flow[i].depth > 1e-7f ? density[i] : 0.0f;
}

static double advect(const float *density, const JceWaterShoreCell *flow,
                     int side, double x, double z)
{
    if (x <= -1.0 || x >= side || z <= -1.0 || z >= side) return 0.0;
    const int ix = (int)floor(x), iz = (int)floor(z);
    const double dx = x - ix, dz = z - iz;
    const double a = (1.0 - dx) * masked_sample(density, flow, side, ix, iz) +
                     dx * masked_sample(density, flow, side, ix + 1, iz);
    const double b = (1.0 - dx) * masked_sample(density, flow, side, ix, iz + 1) +
                     dx * masked_sample(density, flow, side, ix + 1, iz + 1);
    return (1.0 - dz) * a + dz * b;
}

static bool rate_valid(float v) { return v >= 0.0f && v <= 100.0f; }
static bool scale_valid(float v) { return v >= 0.001f && v <= 10000.0f; }

bool jce_water_shore_foam_step(int side, float spacing, double dt,
    const JceShoreFoamControls *p, const JceShoreDistance *shore, bool has_shore,
    const JceWaterShoreCell *flow, const float *previous, float *next)
{
    if (!size_valid(side, spacing) || !p || !shore || !flow || !previous ||
        !next || next == previous || !(dt >= 0.0 && dt <= 100.0) ||
        !scale_valid(p->reach_metres) || !scale_valid(p->max_depth) ||
        !scale_valid(p->full_speed) || !rate_valid(p->birth_rate) ||
        !rate_valid(p->decay_rate)) return false;
    for (int i = 0; i < side * side; ++i) {
        if (!(previous[i] >= 0.0f && previous[i] <= 1.0f) ||
            !(flow[i].depth >= 0.0f) || !isfinite(flow[i].depth) ||
            !isfinite(flow[i].momentum_x) || !isfinite(flow[i].momentum_z) ||
            !isfinite(shore[i].signed_metres)) return false;
    }
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const int i = z * side + x;
            const double depth = flow[i].depth;
            if (depth <= 1e-7) {
                next[i] = 0.0f;
                continue;
            }
            const double vx = flow[i].momentum_x / depth;
            const double vz = flow[i].momentum_z / depth;
            double density = advect(previous, flow, side,
                x - vx * dt / spacing, z - vz * dt / spacing);
            const double distance = fabs(shore[i].signed_metres);
            const double band = has_shore ? 1.0 - smooth_unit(distance / p->reach_metres) : 0.0;
            const double shallow = 1.0 - smooth_unit(depth / p->max_depth);
            const double speed = clamp_unit(hypot(vx, vz) / p->full_speed);
            const double birth = p->birth_rate * band * shallow * speed;
            const double rate = birth + p->decay_rate;
            if (rate > 0.0)
                density += (birth / rate - density) * -expm1(-rate * dt);
            next[i] = (float)clamp_unit(density);
        }
    }
    return true;
}
