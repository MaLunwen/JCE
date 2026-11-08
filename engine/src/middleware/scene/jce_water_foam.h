#ifndef JCE_WATER_FOAM_H
#define JCE_WATER_FOAM_H
#include <jce/middleware/scene/jce_scene_water_foam.h>
#include <jce/middleware/scene/jce_water_field.h>
#include <math.h>
typedef struct JceWaterFoamState {
    JceWaterFoamComponent config;
    float *density[2];
    int resolution;
    float period[2];
    double time;
    uint64_t identity, revision;
} JceWaterFoamState;
static inline bool jce_water_foam_valid(const JceWaterFoamComponent *v)
{
    return v && v->struct_size >= sizeof(*v) &&
        v->version == JCE_WATER_FOAM_COMPONENT_VERSION &&
        isfinite(v->generation_rate) && v->generation_rate >= 0.0f && v->generation_rate <= 100.0f &&
        isfinite(v->decay_rate) && v->decay_rate >= 0.0f && v->decay_rate <= 100.0f &&
        isfinite(v->fold_threshold) && v->fold_threshold >= -1.0f && v->fold_threshold <= 1.0f &&
        isfinite(v->opacity) && v->opacity >= 0.0f && v->opacity <= 1.0f &&
        isfinite(v->texture_scale) && v->texture_scale >= 0.0001f && v->texture_scale <= 1000.0f;
}
/* Exact constant-source solution of dF/dt = birth*(1-F)-decay*F.
 * This is a per-step source approximation, not a hydrodynamic foam model. */
static inline float jce_water_foam_integrate(float previous, float jacobian,
    const JceWaterFoamComponent *v, double dt)
{
    double source = (double)v->fold_threshold - jacobian;
    if (source < 0.0) source = 0.0;
    if (source > 1.0) source = 1.0;
    const double birth = source * v->generation_rate;
    const double rate = birth + v->decay_rate;
    if (rate == 0.0 || dt <= 0.0) return previous;
    const double target = birth / rate;
    const double result = previous + (target - previous) * (-expm1(-rate * dt));
    return (float)(result < 0.0 ? 0.0 : result > 1.0 ? 1.0 : result);
}
bool jce_water_foam_matches(const JceWaterFoamState *state, const JceWaterField *field);
bool jce_water_foam_configure(JceWaterFoamState **owner, const JceWaterField *field,
    const JceWaterFoamComponent *config, uint64_t identity);
bool jce_water_foam_advance(JceWaterFoamState *state, const JceWaterField *field);
void jce_water_foam_destroy(JceWaterFoamState *state);
#endif
