#ifndef JCE_SR_WATER_MESH_H
#define JCE_SR_WATER_MESH_H
#include <math.h>
enum { SR_WATER_DETAIL_RINGS = 96, SR_WATER_DETAIL_SEGMENTS = 192,
       SR_WATER_DETAIL_NEAR_RINGS = 64 };
/* Match the derivative of the bounded linear near grid at the exponential
 * transition. The old 32-to-43m radial jump exposed whole triangular crests
 * against the terrain. Mesh density is independent of FFT resolution. */
static inline float sr_water_detail_growth(float inner, float near_edge, float outer)
{
    const double n = SR_WATER_DETAIL_RINGS - SR_WATER_DETAIL_NEAR_RINGS;
    const double step = (near_edge - inner) / (SR_WATER_DETAIL_NEAR_RINGS - 1);
    const double gap = outer - near_edge;
    double lo = 0.0, hi = 8.0;
    for (int i = 0; i < 40; ++i) {
        const double k = (lo + hi) * 0.5;
        if (gap * k / expm1(k * n) > step) lo = k;
        else hi = k;
    }
    return (float)((lo + hi) * 0.5);
}
static inline float sr_water_detail_radius(int ring, float inner, float near_edge,
                                         float outer, float growth)
{
    if (ring < SR_WATER_DETAIL_NEAR_RINGS)
        return inner + (near_edge - inner) * (float)ring / (SR_WATER_DETAIL_NEAR_RINGS - 1);
    const double n = SR_WATER_DETAIL_RINGS - SR_WATER_DETAIL_NEAR_RINGS;
    const double t = ring - SR_WATER_DETAIL_NEAR_RINGS + 1;
    const double ratio = growth > 1e-7f ? expm1(growth * t) / expm1(growth * n) : t / n;
    return near_edge + (outer - near_edge) * (float)ratio;
}
#endif
