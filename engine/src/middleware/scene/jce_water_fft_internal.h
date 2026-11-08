#ifndef JCE_WATER_FFT_INTERNAL_H
#define JCE_WATER_FFT_INTERNAL_H

#include <jce/middleware/scene/jce_water_fft.h>
#include <math.h>

/* Shared input admission for FFT and the field owner, including dormant
 * non-FFT bodies. No state changes or allocations during validation. */
static inline bool jce_water_fft_spectrum_valid(const JceWaterFftSpectrumDesc *d)
{
    if (!d || d->struct_size < sizeof(*d) ||
        d->version != JCE_WATER_FFT_SPECTRUM_VERSION ||
        d->model < JCE_WATER_FFT_SPECTRUM_JONSWAP ||
        d->model > JCE_WATER_FFT_SPECTRUM_TMA ||
        !isfinite(d->wind_speed) || !(d->wind_speed > 0.0f) ||
        !isfinite(d->fetch) || !isfinite(d->swell) ||
        !isfinite(d->depth) || !isfinite(d->directional_blend) ||
        d->swell < 0.0f || d->swell > 1.0f ||
        d->directional_blend < 0.0f || d->directional_blend > 1.0f ||
        (d->model != JCE_WATER_FFT_SPECTRUM_PM && !(d->fetch > 0.0f)) ||
        (d->model == JCE_WATER_FFT_SPECTRUM_TMA && !(d->depth > 0.0f)))
        return false;

    return true;
}

#endif
