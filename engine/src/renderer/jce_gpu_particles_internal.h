#ifndef JCE_GPU_PARTICLES_INTERNAL_H
#define JCE_GPU_PARTICLES_INTERNAL_H
#include <jce/renderer/jce_gpu_particles.h>
/* Optional on-demand post-allocation diagnostics; no new public API. */
void jce_gpu_particles_debug_enable(JceGpuParticleSystem *sys, bool enabled);
/* Current update only: RGBA8 RGB little-endian 24-bit alive count, alpha validity. */
uint16_t jce_gpu_particles_debug_texture(const JceGpuParticleSystem *sys);
#endif
