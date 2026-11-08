#ifndef JCE_WATER_FFT_MOTION_INTERNAL_H
#define JCE_WATER_FFT_MOTION_INTERNAL_H

#include <jce/middleware/scene/jce_water_fft.h>

/* Apply motion to the displacement grid before measuring its Jacobian. A
 * double cap preserves a positive budget when it is split across two levels. */
void jce_water_fft_evolve_motion(JceWaterFft *fft, float phase_time,
    float horizontal_scale, double max_horizontal_displacement);

/* Solve a single world-space inverse for both packed, repeating textures. */
void jce_water_fft_sample_surface_pair(const JceWaterFft *primary,
    const JceWaterFft *secondary, float world_x, float world_z,
    int iterations, JceWaterFftSample *out);

#endif
