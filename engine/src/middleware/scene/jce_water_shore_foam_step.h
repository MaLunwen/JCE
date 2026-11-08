#ifndef JCE_WATER_SHORE_FOAM_STEP_H
#define JCE_WATER_SHORE_FOAM_STEP_H
#include "jce_water_shore_step.h"
#include <stdint.h>

typedef struct JceShoreDistance {
    float signed_metres;
    float direction_x;
    float direction_z;
} JceShoreDistance;

typedef struct JceShoreFoamControls {
    float reach_metres;
    float max_depth;
    float full_speed;
    float birth_rate;
    float decay_rate;
} JceShoreFoamControls;

/* Scratch arrays and output each hold side*side elements. Nonzero mask cells
 * mark water. The grid edge alone never seeds a shoreline. */
bool jce_water_shore_jfa(int side, float spacing, const uint8_t *wet_mask,
    int32_t *scratch_a, int32_t *scratch_b, JceShoreDistance *output,
    bool *has_shore);

/* Backtrace in world XZ through the shallow solver momentum field, then
 * integrate bounded density production and exponential loss. */
bool jce_water_shore_foam_step(int side, float spacing, double dt,
    const JceShoreFoamControls *p, const JceShoreDistance *shore, bool has_shore,
    const JceWaterShoreCell *flow, const float *previous, float *next);
#endif
