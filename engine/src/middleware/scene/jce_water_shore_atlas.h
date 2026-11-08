#ifndef JCE_WATER_SHORE_ATLAS_H
#define JCE_WATER_SHORE_ATLAS_H
#include "jce_water_shore_state.h"
#include "os/core/jce_half_internal.h"
#include <math.h>
#include <string.h>

/* The physical free surface bed+h is undefined in a dry cell. Extend the
 * adjacent wet surface through one texel of dry padding before bilinear
 * filtering; otherwise dry terrain raises the drawn water along the coast.
 * All values are relative to the still plane, never absolute world Y. */
static inline float jce_water_shore_render_offset(int side,
    const JceWaterShoreCell *cells, const float *bed, int index)
{
    const float depth = cells[index].depth;
    if (isfinite(depth) && depth > 0.001f)
        return bed[index] + depth;
    const int x = index % side, z = index / side;
    float sum = 0.0f, weights = 0.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((!dx && !dz) || x + dx < 0 || x + dx >= side ||
                z + dz < 0 || z + dz >= side) continue;
            const int n = (z + dz) * side + x + dx;
            if (!isfinite(cells[n].depth) || cells[n].depth <= 0.001f) continue;
            const float eta = bed[n] + cells[n].depth;
            if (!isfinite(eta)) continue;
            const float w = (dx && dz) ? 0.5f : 1.0f;
            sum += eta * w;
            weights += w;
        }
    }
    return weights > 0.0f ? sum / weights : 0.0f;
}

static inline int jce_water_shore_padded_sample(int index, int side)
{
    return index < 1 ? 0 : (index > side ? side - 1 : index - 1);
}

static inline uint16_t jce_water_shore_half(float value)
{
    if (!isfinite(value)) return 0;
    return jce_float_to_half(fmaxf(-65504.0f, fminf(65504.0f, value)));
}

static inline void jce_water_shore_pack_atlas(uint16_t *pixels, int pitch, int height,
                       const JceWaterShoreSnapshot *shore, const float *wake, int wake_side)
{
    memset(pixels, 0, (size_t)pitch * height * 4u * sizeof(*pixels));
    const int side = shore->side;
    for (int z = 0; z < side + 2; ++z) {
        const int src_z = jce_water_shore_padded_sample(z, side) * side;
        uint16_t *row = pixels + (size_t)z * pitch * 4u;
        uint16_t *flow_row = pixels + (size_t)(z + side + 2) * pitch * 4u;
        for (int x = 0; x < side + 2; ++x) {
            const int i = src_z + jce_water_shore_padded_sample(x, side);
            const float depth = shore->cells[i].depth;
            row[x * 4]     = jce_water_shore_half(jce_water_shore_render_offset(side, shore->cells, shore->bed, i));
            row[x * 4 + 1] = jce_water_shore_half(depth);
            row[x * 4 + 2] = jce_water_shore_half(shore->foam[i]);
            float vx = 0.0f, vz = 0.0f;
            if (isfinite(depth) && depth > 0.03f &&
                isfinite(shore->cells[i].momentum_x) && isfinite(shore->cells[i].momentum_z)) {
                vx = shore->cells[i].momentum_x / depth;
                vz = shore->cells[i].momentum_z / depth;
                const float speed = hypotf(vx, vz);
                if (isfinite(speed) && speed > 8.0f) { vx *= 8.0f / speed; vz *= 8.0f / speed; }
                if (!isfinite(speed)) { vx = 0.0f; vz = 0.0f; }
            }
            flow_row[x * 4] = jce_water_shore_half(vx);
            flow_row[x * 4 + 1] = jce_water_shore_half(vz);
        }
    }
    if (!wake_side) return;
    for (int z = 0; z < wake_side + 2; ++z) {
        const int src_z = jce_water_shore_padded_sample(z, wake_side) * wake_side;
        uint16_t *row = pixels + ((size_t)z * pitch + side + 2) * 4u;
        for (int x = 0; x < wake_side + 2; ++x)
            row[x * 4] = jce_water_shore_half(wake[src_z + jce_water_shore_padded_sample(x, wake_side)]);
    }
}

#endif
