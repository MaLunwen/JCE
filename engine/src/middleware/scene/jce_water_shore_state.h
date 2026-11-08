#ifndef JCE_WATER_SHORE_STATE_H
#define JCE_WATER_SHORE_STATE_H
#include "jce_water_shore_boundary.h"
#include "jce_water_shore_foam_step.h"
#include <stdint.h>

typedef struct JceWaterShoreState JceWaterShoreState;

typedef struct JceWaterShoreStateDesc {
    int side;
    float cell_size;
    float min_x, min_z, still_y;
    float max_incoming_height, max_incoming_speed;
    JceWaterShoreControls water;
    JceShoreFoamControls foam;
} JceWaterShoreStateDesc;

enum { JCE_WATER_SHORE_SIDE = 32, JCE_WATER_SHORE_MAX_SIDE = 64,
       JCE_WATER_SHORE_CELL_BUDGET = 4096 };

typedef struct JceWaterShoreSnapshot {
    int side;
    float cell_size, min_x, min_z;
    const JceWaterShoreCell *cells;
    const float *bed, *foam;
    uint64_t identity, revision;
} JceWaterShoreSnapshot;

typedef struct JceWaterShoreStateSample {
    float surface_y;
    float depth;
    float foam;
    float weight;
} JceWaterShoreStateSample;

/* Copy bed in metres relative to the still plane. Cell centres are half a
 * cell inside the region. The candidate is complete before it is returned. */
JceWaterShoreState *jce_water_shore_state_create(const JceWaterShoreStateDesc *desc,
    const float *bed, double time, uint64_t identity);
void jce_water_shore_state_destroy(JceWaterShoreState *state);

/* Same-time reads are idempotent. A rewind or gap over 1/8 s resets history.
 * Solver failure leaves the previously published cells and time intact. */
bool jce_water_shore_state_evolve(JceWaterShoreState *state, double time,
    const float *fft_edge_elevation);
bool jce_water_shore_state_sample(const JceWaterShoreState *state, float world_x,
    float world_z, JceWaterShoreStateSample *out);
/* Snapshot arrays are borrowed until the next evolve or destroy. */
bool jce_water_shore_state_snapshot(const JceWaterShoreState *state, JceWaterShoreSnapshot *out);
double jce_water_shore_state_time(const JceWaterShoreState *state);
uint64_t jce_water_shore_state_revision(const JceWaterShoreState *state);
#endif
