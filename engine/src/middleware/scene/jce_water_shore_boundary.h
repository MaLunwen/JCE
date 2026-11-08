#ifndef JCE_WATER_SHORE_BOUNDARY_H
#define JCE_WATER_SHORE_BOUNDARY_H
#include "jce_water_shore_step.h"

/* Four side-long FFT elevation arrays ordered west, east, south, north.
 * West/east are indexed by Z and south/north by X. Heights are relative to
 * the same still plane as bed. A null elevation array drives still water. */
bool jce_water_shore_boundary(int side, const float *bed, float sponge_width,
    float max_incoming_height, float max_incoming_speed, const float *fft_elevation,
    JceWaterShoreCell *target);
#endif
