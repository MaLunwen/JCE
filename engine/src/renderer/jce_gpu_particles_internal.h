#ifndef JCE_GPU_PARTICLES_INTERNAL_H
#define JCE_GPU_PARTICLES_INTERNAL_H
#include <jce/renderer/jce_gpu_particles.h>
/* Shared by editor and runtime scene emitters. These retain the legacy
 * public point-emission and round-billboard entry points. */
enum { JCE_PARTICLE_COVER_MAX = 8 };
typedef struct JceParticleCover {
    uint16_t count;
    /* Each triangle has three vec4s, as in the terrain cover producer.
     * The first origin's unused w component carries the draw's count. */
    float triangles[JCE_PARTICLE_COVER_MAX][3][4];
} JceParticleCover;

/* A current borrowed nearshore atlas with its padded cell-to-UV mapping.
 * params[0] is min XZ, inv cell size, side. params[1] is inv atlas XY,
 * still world Y and the active flag. The scene owns the texture lifetime. */
typedef struct JceParticleRainSurface {
    JceTextureHandle texture;
    float params[2][4];
} JceParticleRainSurface;

void jce_gpu_particles_update_box(JceGpuParticleSystem *sys, uint16_t view,
    float dt, const JceGpuParticleEmitConfig *cfg, jce_vec3 spawn_box,
    const JceParticleRainSurface *rain, const JceParticleCover *cover);
void jce_gpu_particles_render_stretched(JceGpuParticleSystem *sys, uint16_t view,
    JceTextureHandle texture, bool blend_alpha, const JceGpuParticleSoft *soft,
    float velocity_stretch, const JceParticleCover *cover,
    const JceParticleRainSurface *rain);

/* Optional on-demand post-allocation diagnostics; no new public API. */
void jce_gpu_particles_debug_enable(JceGpuParticleSystem *sys, bool enabled);
/* Current update only: RGBA8 RGB little-endian 24-bit alive count, alpha validity. */
uint16_t jce_gpu_particles_debug_texture(const JceGpuParticleSystem *sys);
#endif
