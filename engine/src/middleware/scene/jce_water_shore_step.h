#ifndef JCE_WATER_SHORE_STEP_H
#define JCE_WATER_SHORE_STEP_H

#include <stdbool.h>

typedef struct JceWaterShoreCell {
    float depth;
    float momentum_x;
    float momentum_z;
} JceWaterShoreCell;

typedef struct JceWaterShoreGrid {
    int side;
    float cell_size;
    const float *bed;
    const JceWaterShoreCell *boundary_target;
} JceWaterShoreGrid;

typedef struct JceWaterShoreControls {
    float implicit_drag;
    float exponential_drag;
    float dry_drag;
    float wet_depth;
    float max_speed;
    float sponge_width;
    float sponge_rate;
} JceWaterShoreControls;

/* Advance a conservative, well-balanced grid by at most frame_dt. Input and
 * output must be distinct grids. The caller swaps them after success. A target
 * specifies still depth or incoming FFT forcing in the boundary strip. */
bool jce_water_shore_substep(const JceWaterShoreGrid *grid,
    const JceWaterShoreControls *controls, const JceWaterShoreCell *current,
    JceWaterShoreCell *next, double frame_dt, double *consumed_dt);

#endif
