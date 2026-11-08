#ifndef JCE_WATER_FIELD_INTERNAL_H
#define JCE_WATER_FIELD_INTERNAL_H

#include <jce/middleware/scene/jce_water_field.h>
#include "jce_water_foam.h"
#include "jce_water_shore_state.h"

typedef struct JceWaterFieldMotion {
    float phase_rate;
    float horizontal_scale;
    float max_horizontal_displacement; /* metres, zero for unlimited */
} JceWaterFieldMotion;

/* A null motion preserves the live setting or supplies {1, 1, 0} for a new
 * field. Scene owners removing authored motion should pass explicit defaults. */
JceWaterField *jce_water_field_set_acquire_motion(JceWaterFieldSet *set,
    uint64_t key, const JceWaterFieldDesc *desc,
    const JceWaterFftSpectrumDesc *spectrum, const JceWaterFieldMotion *motion);

/* Evaluated spectral phase, including the phase-preserving WaterMotion rate.
 * Unlike scene time this freezes at rate zero; no new clock is accumulated. */
double jce_water_field_phase(const JceWaterField *field);

/* Lazily evaluate a caustics-only FFT at the field's current phase. The
 * fraction is relative to the actual primary period and must yield a period
 * shorter than the geometric secondary. Zero releases this optional source.
 * The returned pointer is borrowed until the next edit or destroy. Revision
 * is per live field and changes when the source is rebuilt or reevaluated;
 * failure writes zero and never changes either geometric cascade. */
const JceWaterFft *jce_water_field_caustic_fft(JceWaterField *field,
    float period_fraction, uint64_t *source_revision);

/* Samples the existing evaluated FFT field at final world XZ. Render meshes
 * may extend beyond the authored buoyancy rectangle, notably ocean rings. */
bool jce_water_field_sample_render(const JceWaterField *field,
                                  float world_x, float world_z,
                                  JceWaterSample *out);

bool jce_water_field_set_configure_foam(JceWaterFieldSet *set, uint64_t key,
    const JceWaterFoamComponent *config);
const JceWaterFoamState *jce_water_field_set_foam(const JceWaterFieldSet *set, uint64_t key);

/* This key describes the sampled bed and solver settings. Terrain cache
 * revisions are unique within a scene; the world matrix covers parenting. */
typedef struct JceWaterShoreBinding {
    uint64_t terrain, terrain_revision;
    jce_mat4 terrain_world;
    float still_y, min_x, min_z, extent, drag, absorption;
} JceWaterShoreBinding;

bool jce_water_field_set_shore_matches(const JceWaterFieldSet *set, uint64_t key,
    const JceWaterShoreBinding *binding);
void jce_water_field_set_configure_shore(JceWaterFieldSet *set, uint64_t key,
    const JceWaterShoreBinding *binding, const JceWaterShoreStateDesc *desc, const float *bed);
const JceWaterShoreState *jce_water_field_set_shore(const JceWaterFieldSet *set, uint64_t key);

void jce_scene_prepare_water_nearshore(JceScene *scene, JceEntity water,
    const JceWaterFieldDesc *desc, const JceWaterField *field);

#endif
