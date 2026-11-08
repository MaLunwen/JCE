#include "jce_water_shore_boundary.h"
#include <math.h>

static float boundary_weight(int cell, float width)
{
    const float t = fmaxf(0.0f, 1.0f - ((float)cell + 0.5f) / width);
    return t * t * (3.0f - 2.0f * t);
}

bool jce_water_shore_boundary(int side, const float *bed, float sponge_width,
    float max_incoming_height, float max_incoming_speed, const float *fft_elevation,
    JceWaterShoreCell *target)
{
    if (side < 4 || side > 256 || !bed || !target ||
        !(sponge_width >= 0.0f && sponge_width <= 0.5f) ||
        !(max_incoming_height >= 0.0f && max_incoming_height <= 20.0f) ||
        !(max_incoming_speed >= 0.0f && max_incoming_speed <= 50.0f)) return false;
    const int count = side * side;
    for (int i = 0; i < count; ++i)
        if (!(bed[i] >= -1000.0f && bed[i] <= 1000.0f)) return false;
    if (fft_elevation) {
        for (int i = 0; i < 4 * side; ++i)
            if (!isfinite(fft_elevation[i])) return false;
    }
    const float width = fmaxf(1.0f, side * sponge_width);
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const int i = z * side + x;
            const float rest = fmaxf(0.0f, -bed[i]);
            JceWaterShoreCell cell = {rest, 0.0f, 0.0f};
            if (fft_elevation && rest > 1e-7f) {
                const float w = boundary_weight(x, width);
                const float e = boundary_weight(side - 1 - x, width);
                const float s = boundary_weight(z, width);
                const float n = boundary_weight(side - 1 - z, width);
                const float sum = w + e + s + n;
                if (sum > 0.0f) {
                    const float limit = fminf(max_incoming_height, rest * 0.5f);
                    const float ew = fminf(limit, fmaxf(-limit, fft_elevation[z]));
                    const float ee = fminf(limit, fmaxf(-limit, fft_elevation[side + z]));
                    const float es = fminf(limit, fmaxf(-limit, fft_elevation[2 * side + x]));
                    const float en = fminf(limit, fmaxf(-limit, fft_elevation[3 * side + x]));
                    cell.depth += (w * ew + e * ee + s * es + n * en) / sum;
                    /* Linear incoming shallow-wave characteristic. Corners
                     * blend the two boundary normals before limiting speed. */
                    const float scale = sqrtf(9.80665f / rest) / sum;
                    const float vx = scale * (w * ew - e * ee);
                    const float vz = scale * (s * es - n * en);
                    const float speed = hypotf(vx, vz);
                    const float speed_scale = speed > max_incoming_speed
                        ? max_incoming_speed / speed : 1.0f;
                    cell.momentum_x = cell.depth * speed_scale * vx;
                    cell.momentum_z = cell.depth * speed_scale * vz;
                }
            }
            target[i] = cell;
        }
    }
    return true;
}
