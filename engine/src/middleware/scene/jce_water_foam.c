#include "jce_water_foam.h"
#include "os/core/jce_memory.h"
#include <string.h>

void jce_water_foam_destroy(JceWaterFoamState *state)
{
    if (!state) return;
    JCE_FREE(state->density[0]);
    JCE_FREE(state->density[1]);
    JCE_FREE(state);
}

bool jce_water_foam_matches(const JceWaterFoamState *state, const JceWaterField *field)
{
    const JceWaterFft *a = jce_water_field_fft(field);
    const JceWaterFft *b = jce_water_field_fft2(field);
    return state && a && state->resolution == jce_water_fft_resolution(a) &&
        state->period[0] == jce_water_fft_patch_size(a) &&
        (!!state->density[1] == !!b) && (!b ||
        (state->resolution == jce_water_fft_resolution(b) &&
         state->period[1] == jce_water_fft_patch_size(b)));
}

bool jce_water_foam_configure(JceWaterFoamState **owner, const JceWaterField *field,
    const JceWaterFoamComponent *config, uint64_t identity)
{
    if (!owner) return false;
    if (!config) { jce_water_foam_destroy(*owner); *owner = NULL; return true; }
    if (!field || !identity || !jce_water_foam_valid(config)) return false;
    const JceWaterFft *a = jce_water_field_fft(field);
    const JceWaterFft *b = jce_water_field_fft2(field);
    const int n = a ? jce_water_fft_resolution(a) : 0;
    if (n < 2 || n > 1024 || (n & (n-1)) ||
        !isfinite(jce_water_field_time(field)) ||
        (b && jce_water_fft_resolution(b) != n)) return false;
    if (jce_water_foam_matches(*owner, field)) {
        (*owner)->config = *config;
        (*owner)->config.struct_size = sizeof(*config);
        return true;
    }
    JceWaterFoamState *next = (JceWaterFoamState *)JCE_CALLOC(1, sizeof(*next));
    if (!next) return false;
    next->density[0] = (float *)JCE_CALLOC((size_t)n*(size_t)n, sizeof(float));
    if (b) next->density[1] = (float *)JCE_CALLOC((size_t)n*(size_t)n, sizeof(float));
    if (!next->density[0] || (b && !next->density[1])) {
        jce_water_foam_destroy(next); return false;
    }
    next->config = *config;
    next->config.struct_size = sizeof(*config);
    next->resolution = n;
    next->period[0] = jce_water_fft_patch_size(a);
    next->period[1] = b ? jce_water_fft_patch_size(b) : 0.0f;
    next->time = jce_water_field_time(field);
    next->identity = identity;
    next->revision = 1;
    jce_water_foam_destroy(*owner);
    *owner = next;
    return true;
}

bool jce_water_foam_advance(JceWaterFoamState *state, const JceWaterField *field)
{
    if (!jce_water_foam_matches(state, field)) return false;
    const double now = jce_water_field_time(field), dt = now-state->time;
    if (!isfinite(now) || !isfinite(dt)) return false;
    if (dt == 0.0) return true;
    const size_t cells = (size_t)state->resolution*(size_t)state->resolution;
    if (dt < 0.0) {
        memset(state->density[0], 0, cells*sizeof(float));
        if (state->density[1]) memset(state->density[1], 0, cells*sizeof(float));
    } else {
        const float *jac[2] = {
            jce_water_fft_foam_data(jce_water_field_fft(field)),
            jce_water_fft_foam_data(jce_water_field_fft2(field)) };
        /* Refuse the complete step before publishing any partial history. */
        for (int band = 0; band < 2; ++band) if (state->density[band]) {
            if (!jac[band]) return false;
            for (size_t i = 0; i < cells; ++i) if (!isfinite(jac[band][i])) return false;
        }
        for (int band = 0; band < 2; ++band) if (state->density[band])
            for (size_t i = 0; i < cells; ++i)
                state->density[band][i] = jce_water_foam_integrate(
                    state->density[band][i], jac[band][i], &state->config, dt);
    }
    state->time = now;
    ++state->revision;
    return true;
}
